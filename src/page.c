/*
 * Copyright 2015-2017 Leonid Yuriev <leo@yuriev.ru>
 * and other libmdbx authors: please see AUTHORS file.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "./bits.h"
#include "./proto.h"

/* Allocate memory for a page.
 * Re-use old malloc'd pages first for singletons, otherwise just malloc.
 * Set MDBX_TXN_ERROR on failure. */
static page_t *page_malloc(MDBX_txn *txn, unsigned num) {
  MDBX_milieu *bk = txn->mt_book;
  page_t *np = bk->me_dpages;
  size_t size = bk->me_psize;
  if (likely(num == 1 && np)) {
    ASAN_UNPOISON_MEMORY_REGION(np, size);
    VALGRIND_MEMPOOL_ALLOC(bk, np, size);
    VALGRIND_MAKE_MEM_DEFINED(&np->mp_next, sizeof(np->mp_next));
    bk->me_dpages = np->mp_next;
  } else {
    size = pgno2bytes(bk, num);
    np = malloc(size);
    if (unlikely(!np)) {
      txn->mt_flags |= MDBX_TXN_ERROR;
      return np;
    }
    VALGRIND_MEMPOOL_ALLOC(bk, np, size);
  }

  if ((bk->me_flags32 & MDBX_NOMEMINIT) == 0) {
    /* For a single page alloc, we init everything after the page header.
     * For multi-page, we init the final page; if the caller needed that
     * many pages they will be filling in at least up to the last page. */
    size_t skip = PAGEHDRSZ;
    if (num > 1)
      skip += pgno2bytes(bk, num - 1);
    memset((char *)np + skip, 0, size - skip);
  }
  VALGRIND_MAKE_MEM_UNDEFINED(np, size);
  np->mp_flags16 = 0;
  np->mp_pages = num;
  return np;
}

/* Free a dirty page.
 * Saves singlev (not multi-page overflow) pages to a list, for future reuse. */
static void dpage_free(MDBX_milieu *bk, page_t *dp) {
  if (!IS_OVERFLOW(dp) || dp->mp_pages == 1) {
    dp->mp_next = bk->me_dpages;
    VALGRIND_MEMPOOL_FREE(bk, dp);
    bk->me_dpages = dp;
  } else {
    /* large pages just get freed directly */
    VALGRIND_MEMPOOL_FREE(bk, dp);
    free(dp);
  }
}

/* Return all dirty pages to dpage list */
static void dlist_free(MDBX_txn *txn) {
  MDBX_milieu *bk = txn->mt_book;
  MDBX_ID2L dl = txn->mt_rw_dirtylist;
  size_t i, n = dl[0].mid;

  for (i = 1; i <= n; i++) {
    dpage_free(bk, dl[i].mptr);
  }
  dl[0].mid = 0;
}

static void __cold kill_page(MDBX_milieu *bk, pgno_t pgno) {
  const size_t offs = pgno2bytes(bk, pgno);
  const size_t shift = offsetof(page_t, mp_pages);

  if (bk->me_flags32 & MDBX_WRITEMAP) {
    page_t *mp = (page_t *)(bk->me_map + offs);
    memset(&mp->mp_pages, 0x6F /* 'o', 111 */, bk->me_psize - shift);
    VALGRIND_MAKE_MEM_NOACCESS(&mp->mp_pages, bk->me_psize - shift);
    ASAN_POISON_MEMORY_REGION(&mp->mp_pages, bk->me_psize - shift);
  } else {
    intptr_t len = bk->me_psize - shift;
    void *buf = alloca(len);
    memset(buf, 0x6F /* 'o', 111 */, len);
    (void)mdbx_pwrite(bk->me_dxb_fd, buf, len, offs + shift);
  }
}

/* Loosen or free a single page.
 *
 * Saves single pages to a list for future reuse
 * in this same txn. It has been pulled from the GACO
 * and already resides on the dirty list, but has been
 * deleted. Use these pages first before pulling again
 * from the GACO.
 *
 * If the page wasn't dirtied in this txn, just add it
 * to this txn's free list. */
static int page_loose(cursor_t *mc, page_t *mp) {
  int loose = 0;
  const pgno_t pgno = mp->mp_pgno;
  MDBX_txn *txn = mc->mc_txn;

  if ((mp->mp_flags16 & P_DIRTY) && mc->mc_aht != mc->mc_txn->txn_aht_array) {
    if (txn->mt_parent) {
      MDBX_ID2 *dl = txn->mt_rw_dirtylist;
      /* If txn has a parent,
       * make sure the page is in our dirty list. */
      if (dl[0].mid) {
        unsigned x = mdbx_mid2l_search(dl, pgno);
        if (x <= dl[0].mid && dl[x].mid == pgno) {
          if (unlikely(mp != dl[x].mptr)) { /* bad cursor? */
            mdbx_error("wrong page 0x%p #%" PRIaPGNO
                       " in the dirtylist[%d], expecting %p",
                       dl[x].mptr, pgno, x, mp);
            mc->mc_state8 &= ~(C_INITIALIZED | C_EOF);
            txn->mt_flags |= MDBX_TXN_ERROR;
            return MDBX_PROBLEM;
          }
          /* ok, it's ours */
          loose = 1;
        }
      }
    } else {
      /* no parent txn, so it's just ours */
      loose = 1;
    }
  }
  if (loose) {
    mdbx_debug("loosen db %d page %" PRIaPGNO, DAAH(mc), mp->mp_pgno);
    page_t **link = &NEXT_LOOSE_PAGE(mp);
    if (unlikely(txn->mt_book->me_flags32 & MDBX_PAGEPERTURB)) {
      kill_page(txn->mt_book, pgno);
      VALGRIND_MAKE_MEM_UNDEFINED(link, sizeof(page_t *));
      ASAN_UNPOISON_MEMORY_REGION(link, sizeof(page_t *));
    }
    *link = txn->mt_loose_pages;
    txn->mt_loose_pages = mp;
    txn->mt_loose_count++;
    mp->mp_flags16 |= P_LOOSE;
  } else {
    int rc = mdbx_pnl_append(&txn->mt_befree_pages, pgno);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  return MDBX_SUCCESS;
}

/* Set or clear P_KEEP in dirty, non-overflow, non-sub pages watched by txn.
 *
 * [in] pflags  Flags of the pages to update:
 *                - P_DIRTY to set P_KEEP,
 *                - P_DIRTY|P_KEEP to clear it.
 * [in] all     No shortcuts. Needed except after a full page_flush().
 *
 * Returns 0 on success, non-zero on failure. */

static inline void page_toogle_keep(page_t *page, const unsigned match_flags) {
  const unsigned MatchMask = P_SUBP | P_DIRTY | P_LOOSE | P_KEEP;
  if ((page->mp_flags16 & MatchMask) == match_flags)
    page->mp_flags16 ^= P_KEEP;
}

static void cursor_toogle_keep(cursor_t *cursor, unsigned pageflags_match) {
tail_recursion:
  if (cursor->mc_state8 & C_INITIALIZED) {
    page_t *page = nullptr;
    for (unsigned i = 0; i < cursor->mc_snum; ++i) {
      page = cursor->mc_pg[i];
      page_toogle_keep(page, pageflags_match);
    }

    if ((cursor->mc_kind8 & S_HAVESUB) && page && (page->mp_flags16 & P_LEAF)) {
      node_t *leaf = node_ptr(page, cursor->mc_ki[cursor->mc_snum - 1]);
      /* Proceed to subordinate if it is at a sub-database */
      if (leaf->node_flags8 & NODE_SUBTREE) {
        cursor = cursor_subordinate(cursor);
        goto tail_recursion;
      }
    }
  }
}

static int pages_xkeep(MDBX_txn *txn, unsigned pageflags_match, bool all) {
  for (unsigned i = 0; i < txn->txn_ah_num; ++i) {
    for (MDBX_cursor *mc = txn->mt_cursors[i]; mc != nullptr; mc = mc->mc_next)
      cursor_toogle_keep(&mc->primal, pageflags_match);
  }

  if (all) {
    /* Mark dirty root pages */
    aht_t *const end = &txn->txn_aht_array[txn->txn_ah_num];
    for (aht_t *aht = txn->txn_aht_array; aht < end; ++aht) {
      if ((aht->ah.state8 & MDBX_AAH_DIRTY) && aht->aa.root != P_INVALID) {
        int level;
        page_t *dp;
        int rc = page_get(txn, aht->aa.root, &dp, &level);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        if (level <= 1)
          page_toogle_keep(dp, pageflags_match);
      }
    }
  }

  return MDBX_SUCCESS;
}

/* Add a page to the txn's dirty list */
static void mdbx_page_dirty(MDBX_txn *txn, page_t *mp) {
  MDBX_ID2 mid;
  int rc, (*insert)(MDBX_ID2L, MDBX_ID2 *);

  if (txn->mt_flags & MDBX_WRITEMAP) {
    insert = mdbx_mid2l_append;
  } else {
    insert = mdbx_mid2l_insert;
  }
  mid.mid = mp->mp_pgno;
  mid.mptr = mp;
  rc = insert(txn->mt_rw_dirtylist, &mid);
  assert(rc == 0);
  txn->mt_dirtyroom--;
}

/* Spill pages from the dirty list back to disk.
 * This is intended to prevent running into MDBX_TXN_FULL situations,
 * but note that they may still occur in a few cases:
 *
 * 1) our estimate of the txn size could be too small. Currently this
 *  seems unlikely, except with a large number of MDBX_IUD_MULTIPLE items.
 *
 * 2) child txns may run out of space if their parents dirtied a
 *  lot of pages and never spilled them. TODO: we probably should do
 *  a preemptive spill during mdbx_tn_begin() of a child txn, if
 *  the parent's dirtyroom is below a given threshold.
 *
 * Otherwise, if not using nested txns, it is expected that apps will
 * not run into MDBX_TXN_FULL any more. The pages are flushed to disk
 * the same way as for a txn commit, e.g. their P_DIRTY flag is cleared.
 * If the txn never references them again, they can be left alone.
 * If the txn only reads them, they can be used without any fuss.
 * If the txn writes them again, they can be dirtied immediately without
 * going thru all of the work of page_touch(). Such references are
 * handled by page_unspill().
 *
 * Also note, we never spill AA root pages, nor pages of active cursors,
 * because we'll need these back again soon anyway. And in nested txns,
 * we can't spill a page in a child txn if it was already spilled in a
 * parent txn. That would alter the parent txns' data even though
 * the child hasn't committed yet, and we'd have no way to undo it if
 * the child aborted.
 *
 * [in] m0    cursor A cursor handle identifying the transaction and
 *            database for which we are checking space.
 * [in] key   For a put operation, the key being stored.
 * [in] data  For a put operation, the data being stored.
 *
 * Returns 0 on success, non-zero on failure. */
static int page_spill(cursor_t *cursor, MDBX_iov *key, MDBX_iov *data) {
  if (cursor->mc_kind8 & S_SUBCURSOR)
    return MDBX_SUCCESS;

  MDBX_txn *txn = cursor->mc_txn;
  /* Estimate how much space this op will take */
  pgno_t need = cursor->mc_aht->aa.depth16 + aht_gaco(txn)->aa.depth16;
  /* Named AAs also dirty the main AA */
  if ((cursor->mc_aht->aa.flags16 & (MDBX_AAH_GACO | MDBX_AAH_MAIN)) == 0)
    need += aht_main(txn)->aa.depth16;
  /* For puts, roughly factor in the key+data size */
  if (key) {
    size_t sz = LEAFSIZE(key, data);
    if (sz > txn->mt_book->me_nodemax)
      /* put requires an overflow page */
      sz += sizeof(pgno_t) + PAGEHDRSZ + txn->mt_book->me_psize - 1;
    need += bytes2pgno(txn->mt_book, sz);
  }

  if (likely(txn->mt_dirtyroom > need))
    return MDBX_SUCCESS;

  if (!txn->mt_spill_pages) {
    txn->mt_spill_pages = mdbx_pnl_alloc(MDBX_PNL_UM_MAX);
    if (unlikely(!txn->mt_spill_pages))
      return MDBX_ENOMEM;
  } else {
    /* purge deleted slots */
    MDBX_PNL sl = txn->mt_spill_pages;
    pgno_t num = sl[0], j = 0;
    for (unsigned i = 1; i <= num; i++) {
      if (!(sl[i] & 1))
        sl[++j] = sl[i];
    }
    sl[0] = j;
  }

  /* Preserve pages which may soon be dirtied again */
  int rc = pages_xkeep(txn, P_DIRTY, true);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  /* Less aggressive spill - we originally spilled the entire dirty list,
   * with a few exceptions for cursor pages and AA root pages. But this
   * turns out to be a lot of wasted effort because in a large txn many
   * of those pages will need to be used again. So now we spill only 1/8th
   * of the dirty pages. Testing revealed this to be a good tradeoff,
   * better than 1/2, 1/4, or 1/10. */
  need += need; /* double it for good measure */
  if (need < MDBX_PNL_UM_MAX / 8)
    need = MDBX_PNL_UM_MAX / 8;

  /* Save the page IDs of all the pages we're flushing */
  /* flush from the tail forward, this saves a lot of shifting later on. */
  MDBX_ID2L dl = txn->mt_rw_dirtylist;
  unsigned left_dirty;
  for (left_dirty = dl[0].mid; left_dirty && need; left_dirty--) {
    pgno_t pn = dl[left_dirty].mid << 1;
    page_t *dp = dl[left_dirty].mptr;
    if (dp->mp_flags16 & (P_LOOSE | P_KEEP))
      continue;
    /* Can't spill twice,
     * make sure it's not already in a parent's spill list. */
    if (txn->mt_parent) {
      MDBX_txn *tx2;
      for (tx2 = txn->mt_parent; tx2; tx2 = tx2->mt_parent) {
        if (tx2->mt_spill_pages) {
          unsigned j = mdbx_pnl_search(tx2->mt_spill_pages, pn);
          if (j <= tx2->mt_spill_pages[0] && tx2->mt_spill_pages[j] == pn) {
            dp->mp_flags16 |= P_KEEP;
            break;
          }
        }
      }
      if (tx2)
        continue;
    }
    rc = mdbx_pnl_append(&txn->mt_spill_pages, pn);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
    need--;
  }
  mdbx_pnl_sort(txn->mt_spill_pages);

  /* Flush the spilled part of dirty list */
  rc = page_flush(txn, left_dirty);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  /* Reset any dirty pages we kept that page_flush didn't see */
  rc = pages_xkeep(txn, P_DIRTY | P_KEEP, left_dirty != 0);

bailout:
  txn->mt_flags |= rc ? MDBX_TXN_ERROR : MDBX_TXN_SPILLS;
  return rc;
}

/* Allocate page numbers and memory for writing.  Maintain me_last_reclaimed,
 * me_reclaimed_pglist and mt_next_pgno.  Set MDBX_TXN_ERROR on failure.
 *
 * If there are free pages available from older transactions, they
 * are re-used first. Otherwise allocate a new page at mt_next_pgno.
 * Do not modify the GACO, just merge GACO records into me_reclaimed_pglist
 * and move me_last_reclaimed to say which records were consumed.  Only this
 * function can create me_reclaimed_pglist and move
 * me_last_reclaimed/mt_next_pgno.
 *
 * [in] mc    cursor A cursor handle identifying the transaction and
 *            database for which we are allocating.
 * [in] num   the number of pages to allocate.
 * [out] mp   Address of the allocated page(s). Requests for multiple pages
 *            will always be satisfied by a single contiguous chunk of memory.
 *
 * Returns 0 on success, non-zero on failure.*/

static int page_alloc(cursor_t *mc, unsigned num, page_t **mp, int flags) {
  int rc;
  MDBX_txn *txn = mc->mc_txn;
  MDBX_milieu *bk = txn->mt_book;
  page_t *np;

  if (likely(flags & MDBX_ALLOC_GC)) {
    flags |= bk->me_flags32 & (MDBX_COALESCE | MDBX_LIFORECLAIM);
    if (unlikely(mc->mc_state8 & C_RECLAIMING)) {
      /* If mc is updating the GACO, then the freelist cannot play
       * catch-up with itself by growing while trying to save it. */
      flags &=
          ~(MDBX_ALLOC_GC | MDBX_ALLOC_KICK | MDBX_COALESCE | MDBX_LIFORECLAIM);
    }
  }

  if (likely(flags & MDBX_ALLOC_CACHE)) {
    /* If there are any loose pages, just use them */
    assert(mp && num);
    if (likely(num == 1 && txn->mt_loose_pages)) {
      np = txn->mt_loose_pages;
      txn->mt_loose_pages = NEXT_LOOSE_PAGE(np);
      txn->mt_loose_count--;
      mdbx_debug("db %d use loose page %" PRIaPGNO, DAAH(mc), np->mp_pgno);
      ASAN_UNPOISON_MEMORY_REGION(np, bk->me_psize);
      assert(np->mp_pgno < txn->mt_next_pgno);
      *mp = np;
      return MDBX_SUCCESS;
    }
  }

  assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
  pgno_t pgno, *repg_list = bk->me_reclaimed_pglist;
  unsigned repg_pos = 0, repg_len = repg_list ? repg_list[0] : 0;
  txnid_t oldest = 0, last = 0;
  const unsigned wanna_range = num - 1;

  while (1) { /* RBR-kick retry loop */
    /* If our dirty list is already full, we can't do anything */
    if (unlikely(txn->mt_dirtyroom == 0)) {
      rc = MDBX_TXN_FULL;
      goto fail;
    }

    MDBX_cursor recur;
    for (enum MDBX_cursor_op op = MDBX_FIRST;;
         op = (flags & MDBX_LIFORECLAIM) ? MDBX_PREV : MDBX_NEXT) {
      MDBX_iov key, data;

      /* Seek a big enough contiguous page range.
       * Prefer pages with lower pgno. */
      assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
      if (likely(flags & MDBX_ALLOC_CACHE) && repg_len > wanna_range &&
          (!(flags & MDBX_COALESCE) || op == MDBX_FIRST)) {
#if MDBX_PNL_ASCENDING
        for (repg_pos = 1; repg_pos <= repg_len - wanna_range; ++repg_pos) {
          pgno = repg_list[repg_pos];
          if (likely(repg_list[repg_pos + wanna_range - 1] ==
                     pgno + wanna_range - 1))
            goto done;
        }
#else
        repg_pos = repg_len;
        do {
          pgno = repg_list[repg_pos];
          if (likely(repg_list[repg_pos - wanna_range] == pgno + wanna_range))
            goto done;
        } while (--repg_pos > wanna_range);
#endif /* MDBX_PNL sort-order */
      }

      if (op == MDBX_FIRST) { /* 1st iteration, setup cursor, etc */
        if (unlikely(!(flags & MDBX_ALLOC_GC)))
          break /* reclaiming is prohibited for now */;

        /* Prepare to fetch more and coalesce */
        oldest =
            (flags & MDBX_LIFORECLAIM) ? find_oldest(txn) : bk->me_oldest[0];
        rc = cursor_init(&recur, txn, aht_gaco(txn));
        if (unlikely(rc != MDBX_SUCCESS))
          break;
        if (flags & MDBX_LIFORECLAIM) {
          /* Begin from oldest reader if any */
          if (oldest > 2) {
            last = oldest - 1;
            op = MDBX_SET_RANGE;
          }
        } else if (bk->me_last_reclaimed) {
          /* Continue lookup from bk->me_last_reclaimed to oldest reader */
          last = bk->me_last_reclaimed;
          op = MDBX_SET_RANGE;
        }

        key.iov_base = &last;
        key.iov_len = sizeof(last);
      }

      if (!(flags & MDBX_LIFORECLAIM)) {
        /* Do not try fetch more if the record will be too recent */
        if (op != MDBX_FIRST && ++last >= oldest) {
          oldest = find_oldest(txn);
          if (oldest <= last)
            break;
        }
      }

      rc = mdbx_cursor_get(&recur, &key, nullptr, op);
      if (rc == MDBX_NOTFOUND && (flags & MDBX_LIFORECLAIM)) {
        if (op == MDBX_SET_RANGE)
          continue;
        if (oldest < find_oldest(txn)) {
          oldest = *bk->me_oldest;
          last = oldest - 1;
          key.iov_base = &last;
          key.iov_len = sizeof(last);
          op = MDBX_SET_RANGE;
          rc = mdbx_cursor_get(&recur, &key, nullptr, op);
        }
      }
      if (unlikely(rc != MDBX_SUCCESS)) {
        if (rc == MDBX_NOTFOUND)
          break;
        goto fail;
      }

      last = *(txnid_t *)key.iov_base;
      if (oldest <= last) {
        oldest = find_oldest(txn);
        if (oldest <= last) {
          if (flags & MDBX_LIFORECLAIM)
            continue;
          break;
        }
      }

      if (flags & MDBX_LIFORECLAIM) {
        /* skip IDs of records that already reclaimed */
        if (txn->mt_lifo_reclaimed) {
          unsigned i;
          for (i = (unsigned)txn->mt_lifo_reclaimed[0]; i > 0; --i)
            if (txn->mt_lifo_reclaimed[i] == last)
              break;
          if (i)
            continue;
        }
      }

      /* Reading next GACO record */
      np = recur.primal.mc_pg[recur.primal.mc_top];
      node_t *leaf = node_ptr(np, recur.primal.mc_ki[recur.primal.mc_top]);
      if (unlikely((rc = node_read(&recur.primal, leaf, &data)) !=
                   MDBX_SUCCESS))
        goto fail;

      if ((flags & MDBX_LIFORECLAIM) && !txn->mt_lifo_reclaimed) {
        txn->mt_lifo_reclaimed = mdbx_txl_alloc();
        if (unlikely(!txn->mt_lifo_reclaimed)) {
          rc = MDBX_ENOMEM;
          goto fail;
        }
      }

      /* Append PNL from GACO record to me_reclaimed_pglist */
      pgno_t *re_pnl = (pgno_t *)data.iov_base;
      assert(re_pnl[0] == 0 ||
             data.iov_len == (re_pnl[0] + 1) * sizeof(pgno_t));
      assert(mdbx_pnl_check(re_pnl));
      repg_pos = re_pnl[0];
      if (!repg_list) {
        if (unlikely(!(bk->me_reclaimed_pglist = repg_list =
                           mdbx_pnl_alloc(repg_pos)))) {
          rc = MDBX_ENOMEM;
          goto fail;
        }
      } else {
        if (unlikely((rc = mdbx_pnl_need(&bk->me_reclaimed_pglist, repg_pos)) !=
                     0))
          goto fail;
        repg_list = bk->me_reclaimed_pglist;
      }

      /* Remember ID of GACO record */
      if (flags & MDBX_LIFORECLAIM) {
        if ((rc = mdbx_txl_append(&txn->mt_lifo_reclaimed, last)) != 0)
          goto fail;
      }
      bk->me_last_reclaimed = last;

      if (mdbx_debug_enabled(MDBX_DBG_EXTRA)) {
        mdbx_debug_extra("PNL read txn %" PRIaTXN " root %" PRIaPGNO
                         " num %u, PNL",
                         last, aht_gaco(txn)->aa.root, repg_pos);
        unsigned i;
        for (i = repg_pos; i; i--)
          mdbx_debug_extra_print(" %" PRIaPGNO "", re_pnl[i]);
        mdbx_debug_extra_print("\n");
      }

      /* Merge in descending sorted order */
      mdbx_pnl_xmerge(repg_list, re_pnl);
      repg_len = repg_list[0];
      if (unlikely((flags & MDBX_ALLOC_CACHE) == 0)) {
        /* Done for a kick-reclaim mode, actually no page needed */
        return MDBX_SUCCESS;
      }

      assert(repg_len == 0 || repg_list[repg_len] < txn->mt_next_pgno);
      if (repg_len) {
        /* Refund suitable pages into "unallocated" space */
        pgno_t tail = txn->mt_next_pgno;
        pgno_t *const begin = repg_list + 1;
        pgno_t *const end = begin + repg_len;
        pgno_t *higest;
#if MDBX_PNL_ASCENDING
        for (higest = end; --higest >= begin;) {
#else
        for (higest = begin; higest < end; ++higest) {
#endif /* MDBX_PNL sort-order */
          assert(*higest >= NUM_METAS && *higest < tail);
          if (*higest != tail - 1)
            break;
          tail -= 1;
        }
        if (tail != txn->mt_next_pgno) {
#if MDBX_PNL_ASCENDING
          repg_len = (unsigned)(higest + 1 - begin);
#else
          repg_len -= (unsigned)(higest - begin);
          for (pgno_t *move = begin; higest < end; ++move, ++higest)
            *move = *higest;
#endif /* MDBX_PNL sort-order */
          repg_list[0] = repg_len;
          mdbx_info("refunded %" PRIaPGNO " pages: %" PRIaPGNO " -> %" PRIaPGNO,
                    tail - txn->mt_next_pgno, tail, txn->mt_next_pgno);
          txn->mt_next_pgno = tail;
          assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
        }
      }

      /* Don't try to coalesce too much. */
      if (repg_len > MDBX_PNL_UM_SIZE / 2)
        break;
      if (flags & MDBX_COALESCE) {
        if (repg_len /* current size */ >= bk->me_maxfree_1pg / 2 ||
            repg_pos /* prev size */ >= bk->me_maxfree_1pg / 4)
          flags &= ~MDBX_COALESCE;
      }
    }

    if ((flags & (MDBX_COALESCE | MDBX_ALLOC_CACHE)) ==
            (MDBX_COALESCE | MDBX_ALLOC_CACHE) &&
        repg_len > wanna_range) {
#if MDBX_PNL_ASCENDING
      for (repg_pos = 1; repg_pos <= repg_len - wanna_range; ++repg_pos) {
        pgno = repg_list[repg_pos];
        if (likely(repg_list[repg_pos + wanna_range - 1] ==
                   pgno + wanna_range - 1))
          goto done;
      }
#else
      repg_pos = repg_len;
      do {
        pgno = repg_list[repg_pos];
        if (likely(repg_list[repg_pos - wanna_range] == pgno + wanna_range))
          goto done;
      } while (--repg_pos > wanna_range);
#endif /* MDBX_PNL sort-order */
    }

    /* Use new pages from the map when nothing suitable in the GACO */
    repg_pos = 0;
    pgno = txn->mt_next_pgno;
    rc = MDBX_MAP_FULL;
    const pgno_t next = pgno_add(pgno, num);
    if (likely(next <= txn->mt_end_pgno)) {
      rc = MDBX_NOTFOUND;
      if (likely(flags & MDBX_ALLOC_NEW))
        goto done;
    }

    const meta_t *head = meta_head(bk);
    if ((flags & MDBX_ALLOC_GC) &&
        ((flags & MDBX_ALLOC_KICK) || rc == MDBX_MAP_FULL)) {
      meta_t *steady = meta_steady(bk);

      if (oldest == meta_txnid_stable(bk, steady) && !META_IS_STEADY(head) &&
          META_IS_STEADY(steady)) {
        /* LY: Here an RBR was happened:
         *  - all pages had allocated;
         *  - reclaiming was stopped at the last steady-sync;
         *  - the head-sync is weak.
         * Now we need make a sync to resume reclaiming. If both
         * MDBX_NOSYNC and MDBX_MAPASYNC flags are set, then assume that
         * utterly no-sync write mode was requested. In such case
         * don't make a steady-sync, but only a legacy-mode checkpoint,
         * just for resume reclaiming only, not for data consistency. */

        mdbx_debug("kick-gc: head %" PRIaTXN "-%s, tail %" PRIaTXN
                   "-%s, oldest %" PRIaTXN "",
                   meta_txnid_stable(bk, head), durable_str(head),
                   meta_txnid_stable(bk, steady), durable_str(steady), oldest);

        const unsigned syncflags = F_ISSET(bk->me_flags32, MDBX_UTTERLY_NOSYNC)
                                       ? bk->me_flags32
                                       : bk->me_flags32 & MDBX_WRITEMAP;
        meta_t meta = *head;
        if (mdbx_sync_locked(bk, syncflags, &meta) == MDBX_SUCCESS) {
          txnid_t snap = find_oldest(txn);
          if (snap > oldest)
            continue;
        }
      }

      if (rc == MDBX_MAP_FULL && oldest < txn->mt_txnid - 1) {
        if (rbr(bk, oldest) > oldest)
          continue;
      }
    }

    if (rc == MDBX_MAP_FULL && next < head->mm_geo.upper) {
      mdbx_assert(bk, next > txn->mt_end_pgno);
      pgno_t aligned = pgno_align2os_pgno(
          bk, pgno_add(next, head->mm_geo.grow16 - next % head->mm_geo.grow16));

      if (aligned > head->mm_geo.upper)
        aligned = head->mm_geo.upper;
      mdbx_assert(bk, aligned > txn->mt_end_pgno);

      mdbx_info("try growth datafile to %" PRIaPGNO " pages (+%" PRIaPGNO ")",
                aligned, aligned - txn->mt_end_pgno);
      rc = mdbx_mapresize(bk, aligned, head->mm_geo.upper);
      if (rc == MDBX_SUCCESS) {
        assert(txn->mt_end_pgno >= next);
        if (!mp)
          return rc;
        goto done;
      }

      mdbx_warning("unable growth datafile to %" PRIaPGNO "pages (+%" PRIaPGNO
                   "), errcode %d",
                   aligned, aligned - txn->mt_end_pgno, rc);
    }

  fail:
    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
    if (mp) {
      *mp = nullptr;
      txn->mt_flags |= MDBX_TXN_ERROR;
    }
    assert(rc);
    return rc;
  }

done:
  assert(mp && num);
  assert(pgno >= NUM_METAS);
  if (unlikely(pgno < NUM_METAS)) {
    rc = MDBX_PANIC;
    goto fail;
  }

  if (bk->me_flags32 & MDBX_WRITEMAP) {
    np = pgno2page(bk, pgno);
    /* LY: reset no-access flag from kill_page() */
    VALGRIND_MAKE_MEM_UNDEFINED(np, pgno2bytes(bk, num));
    ASAN_UNPOISON_MEMORY_REGION(np, pgno2bytes(bk, num));
  } else {
    if (unlikely(!(np = page_malloc(txn, num)))) {
      rc = MDBX_ENOMEM;
      goto fail;
    }
  }

  if (repg_pos) {
    assert(pgno < txn->mt_next_pgno);
    assert(pgno == repg_list[repg_pos]);
    /* Cutoff allocated pages from me_reclaimed_pglist */
    repg_list[0] = repg_len -= num;
    for (unsigned i = repg_pos - num; i < repg_len;)
      repg_list[++i] = repg_list[++repg_pos];
    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
  } else {
    txn->mt_next_pgno = pgno + num;
    assert(txn->mt_next_pgno <= txn->mt_end_pgno);
  }

  if (bk->me_flags32 & MDBX_PAGEPERTURB)
    memset(np, 0x71 /* 'q', 113 */, pgno2bytes(bk, num));
  VALGRIND_MAKE_MEM_UNDEFINED(np, pgno2bytes(bk, num));

  np->mp_pgno = pgno;
  np->mp_leaf2_ksize16 = 0;
  np->mp_flags16 = 0;
  np->mp_pages = num;
  mdbx_page_dirty(txn, np);
  *mp = np;

  assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
  return MDBX_SUCCESS;
}

/* Copy the used portions of a non-overflow page.
 * [in] dst page to copy into
 * [in] src page to copy from
 * [in] psize size of a page */
static void page_copy(page_t *dst, page_t *src, unsigned psize) {
  STATIC_ASSERT(UINT16_MAX > MAX_PAGESIZE - PAGEHDRSZ);
  STATIC_ASSERT(MIN_PAGESIZE > PAGEHDRSZ + NODESIZE * 42);
  enum { Align = sizeof(pgno_t) };
  indx_t upper = src->mp_upper, lower = src->mp_lower, unused = upper - lower;

  /* If page isn't full, just copy the used portion. Adjust
   * alignment so memcpy may copy words instead of bytes. */
  if ((unused &= -Align) && !IS_DFL(src)) {
    upper = (upper + PAGEHDRSZ) & -Align;
    memcpy(dst, src, (lower + PAGEHDRSZ + (Align - 1)) & -Align);
    memcpy((pgno_t *)((char *)dst + upper), (pgno_t *)((char *)src + upper),
           psize - upper);
  } else {
    memcpy(dst, src, psize - unused);
  }
}

/* Pull a page off the txn's spill list, if present.
 *
 * If a page being referenced was spilled to disk in this txn, bring
 * it back and make it dirty/writable again.
 *
 * [in] txn   the transaction handle.
 * [in] mp    the page being referenced. It must not be dirty.
 * [out] ret  the writable page, if any.
 *            ret is unchanged if mp wasn't spilled. */
static int page_unspill(MDBX_txn *txn, page_t *mp, page_t **ret) {
  MDBX_milieu *bk = txn->mt_book;
  const MDBX_txn *tx2;
  unsigned x;
  pgno_t pgno = mp->mp_pgno, pn = pgno << 1;

  for (tx2 = txn; tx2; tx2 = tx2->mt_parent) {
    if (!tx2->mt_spill_pages)
      continue;
    x = mdbx_pnl_search(tx2->mt_spill_pages, pn);
    if (x <= tx2->mt_spill_pages[0] && tx2->mt_spill_pages[x] == pn) {
      page_t *np;
      int num;
      if (txn->mt_dirtyroom == 0)
        return MDBX_TXN_FULL;
      num = IS_OVERFLOW(mp) ? mp->mp_pages : 1;
      if (bk->me_flags32 & MDBX_WRITEMAP) {
        np = mp;
      } else {
        np = page_malloc(txn, num);
        if (unlikely(!np))
          return MDBX_ENOMEM;
        if (unlikely(num > 1))
          memcpy(np, mp, pgno2bytes(bk, num));
        else
          page_copy(np, mp, bk->me_psize);
      }
      mdbx_debug("unspill page %" PRIaPGNO, mp->mp_pgno);
      if (tx2 == txn) {
        /* If in current txn, this page is no longer spilled.
         * If it happens to be the last page, truncate the spill list.
         * Otherwise mark it as deleted by setting the LSB. */
        if (x == txn->mt_spill_pages[0])
          txn->mt_spill_pages[0]--;
        else
          txn->mt_spill_pages[x] |= 1;
      } /* otherwise, if belonging to a parent txn, the
         * page remains spilled until child commits */

      mdbx_page_dirty(txn, np);
      np->mp_flags16 |= P_DIRTY;
      *ret = np;
      break;
    }
  }
  return MDBX_SUCCESS;
}

/* Flush (some) dirty pages to the map, after clearing their dirty flag.
 * [in] txn   the transaction that's being committed
 * [in] keep  number of initial pages in dirtylist to keep dirty.
 * Returns 0 on success, non-zero on failure. */
static int page_flush(MDBX_txn *txn, pgno_t keep) {
  MDBX_milieu *bk = txn->mt_book;
  MDBX_ID2L dl = txn->mt_rw_dirtylist;
  unsigned i, j, pagecount = dl[0].mid;
  int rc;
  size_t size = 0, pos = 0;
  pgno_t pgno = 0;
  page_t *dp = nullptr;
  struct iovec iov[MDBX_COMMIT_PAGES];
  intptr_t wpos = 0, wsize = 0;
  size_t next_pos = 1; /* impossible pos, so pos != next_pos */
  int n = 0;

  j = i = keep;

  if (bk->me_flags32 & MDBX_WRITEMAP) {
    /* Clear dirty flags */
    while (++i <= pagecount) {
      dp = dl[i].mptr;
      /* Don't flush this page yet */
      if (dp->mp_flags16 & (P_LOOSE | P_KEEP)) {
        dp->mp_flags16 &= ~P_KEEP;
        dl[++j] = dl[i];
        continue;
      }
      dp->mp_flags16 &= ~P_DIRTY;
      dp->page_checksum = 0 /* TODO */;
      bk->me_lck->li_dirty_volume +=
          IS_OVERFLOW(dp) ? pgno2bytes(bk, dp->mp_pages) : bk->me_psize;
    }
    goto done;
  }

  /* Write the pages */
  for (;;) {
    if (++i <= pagecount) {
      dp = dl[i].mptr;
      /* Don't flush this page yet */
      if (dp->mp_flags16 & (P_LOOSE | P_KEEP)) {
        dp->mp_flags16 &= ~P_KEEP;
        dl[i].mid = 0;
        continue;
      }
      pgno = dl[i].mid;
      /* clear dirty flag */
      dp->mp_flags16 &= ~P_DIRTY;
      dp->page_checksum = 0 /* TODO */;
      pos = pgno2bytes(bk, pgno);
      size = IS_OVERFLOW(dp) ? pgno2bytes(bk, dp->mp_pages) : bk->me_psize;
      bk->me_lck->li_dirty_volume += size;
    }
    /* Write up to MDBX_COMMIT_PAGES dirty pages at a time. */
    if (pos != next_pos || n == MDBX_COMMIT_PAGES || wsize + size > MAX_WRITE) {
      if (n) {
        /* Write previous page(s) */
        rc = mdbx_pwritev(bk->me_dxb_fd, iov, n, wpos, wsize);
        if (unlikely(rc != MDBX_SUCCESS)) {
          mdbx_debug("Write error: %s", strerror(rc));
          return rc;
        }
        n = 0;
      }
      if (i > pagecount)
        break;
      wpos = pos;
      wsize = 0;
    }
    mdbx_debug("committing page %" PRIaPGNO "", pgno);
    next_pos = pos + size;
    iov[n].iov_len = size;
    iov[n].iov_base = (char *)dp;
    wsize += size;
    n++;
  }

  mdbx_invalidate_cache(bk->me_map, pgno2bytes(bk, txn->mt_next_pgno));

  for (i = keep; ++i <= pagecount;) {
    dp = dl[i].mptr;
    /* This is a page we skipped above */
    if (!dl[i].mid) {
      dl[++j] = dl[i];
      dl[j].mid = dp->mp_pgno;
      continue;
    }
    dpage_free(bk, dp);
  }

done:
  i--;
  txn->mt_dirtyroom += i - j;
  dl[0].mid = j;
  return MDBX_SUCCESS;
}
