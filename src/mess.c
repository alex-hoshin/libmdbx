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

/*----------------------------------------------------------------------------*/

/* Find oldest txnid still referenced. */
static txnid_t find_oldest(MDBX_txn *txn) {
  assert((txn->mt_flags & MDBX_RDONLY) == 0);
  const MDBX_milieu *bk = txn->mt_book;
  MDBX_lockinfo *const lck = bk->me_lck;

  txnid_t oldest = reclaiming_detent(bk);
  assert(oldest <= txn->mt_txnid - 1);
  const txnid_t last_oldest = lck->li_oldest;
  assert(oldest >= last_oldest);
  if (last_oldest == oldest ||
      lck->li_reader_finished_flag == MDBX_STRING_TETRAD("None"))
    return last_oldest;

  const unsigned snap_nreaders = lck->li_numreaders;
  lck->li_reader_finished_flag = MDBX_STRING_TETRAD("None");
  for (unsigned i = 0; i < snap_nreaders; ++i) {
    if (lck->li_readers[i].mr_pid) {
      /* mdbx_jitter4testing(true); */
      const txnid_t snap = lck->li_readers[i].mr_txnid;
      if (oldest > snap && last_oldest <= /* ignore pending updates */ snap) {
        oldest = snap;
        if (oldest == last_oldest)
          break;
      }
    }
  }

  if (oldest != last_oldest) {
    mdbx_notice("update oldest %" PRIaTXN " -> %" PRIaTXN, last_oldest, oldest);
    assert(oldest >= lck->li_oldest);
    lck->li_oldest = oldest;
  }
  return oldest;
}

static int mdbx_mapresize(MDBX_milieu *bk, const pgno_t size_pgno,
                          const pgno_t limit_pgno) {
#ifdef USE_VALGRIND
  const size_t prev_mapsize = bk->me_mapsize;
  void *const prev_mapaddr = bk->me_map;
#endif

  const size_t limit_bytes = pgno_align2os_bytes(bk, limit_pgno);
  const size_t size_bytes = pgno_align2os_bytes(bk, size_pgno);

  mdbx_info("resize datafile/mapping: "
            "present %" PRIuPTR " -> %" PRIuPTR ", "
            "limit %" PRIuPTR " -> %" PRIuPTR,
            bk->me_bookgeo.now, size_bytes, bk->me_bookgeo.upper, limit_bytes);

  mdbx_assert(bk, limit_bytes >= size_bytes);
  mdbx_assert(bk, bytes2pgno(bk, size_bytes) == size_pgno);
  mdbx_assert(bk, bytes2pgno(bk, limit_bytes) == limit_pgno);
  const int rc =
      mdbx_mresize(bk->me_flags32, &bk->me_dxb_mmap, size_bytes, limit_bytes);

  if (rc == MDBX_SUCCESS) {
    bk->me_bookgeo.now = size_bytes;
    bk->me_bookgeo.upper = limit_bytes;
  } else if (rc != MDBX_RESULT_TRUE) {
    mdbx_error("failed resize datafile/mapping: "
               "present %" PRIuPTR " -> %" PRIuPTR ", "
               "limit %" PRIuPTR " -> %" PRIuPTR ", errcode %d",
               bk->me_bookgeo.now, size_bytes, bk->me_bookgeo.upper,
               limit_bytes, rc);
    return rc;
  } else {
    mdbx_notice("unable resize datafile/mapping: "
                "present %" PRIuPTR " -> %" PRIuPTR ", "
                "limit %" PRIuPTR " -> %" PRIuPTR ", errcode %d",
                bk->me_bookgeo.now, size_bytes, bk->me_bookgeo.upper,
                limit_bytes, rc);
  }

#ifdef USE_VALGRIND
  if (prev_mapsize != bk->me_mapsize || prev_mapaddr != bk->me_map) {
    VALGRIND_DISCARD(bk->me_valgrind_handle);
    bk->me_valgrind_handle = 0;
    if (bk->me_mapsize)
      bk->me_valgrind_handle =
          VALGRIND_CREATE_BLOCK(bk->me_map, bk->me_mapsize, "mdbx");
  }
#endif

  if (bk->me_current_txn) {
    assert(size_pgno >= bk->me_current_txn->mt_next_pgno);
    bk->me_current_txn->mt_end_pgno = size_pgno;
  }
  return MDBX_SUCCESS;
}

int mdbx_bk_sync(MDBX_milieu *bk, int force) {
  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  unsigned flags = bk->me_flags32 & ~MDBX_NOMETASYNC;
  if (unlikely(flags & (MDBX_RDONLY | MDBX_FATAL_ERROR)))
    return MDBX_EACCESS;

  if (unlikely(!bk->me_lck))
    return MDBX_PANIC;

  const bool outside_txn =
      (!bk->me_wpa_txn || bk->me_wpa_txn->mt_owner != mdbx_thread_self());

  if (outside_txn) {
    int rc = mdbx_tn_lock(bk);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  meta_t *head = meta_head(bk);
  if (!META_IS_STEADY(head) || bk->me_lck->li_dirty_volume) {

    if (force ||
        (bk->me_lck->li_autosync_threshold &&
         bk->me_lck->li_dirty_volume >= bk->me_lck->li_autosync_threshold))
      flags &= MDBX_WRITEMAP /* clear flags for full steady sync */;

    if (outside_txn &&
        bk->me_lck->li_dirty_volume >
            pgno2bytes(bk, 16 /* FIXME: define threshold */) &&
        (flags & MDBX_NOSYNC) == 0) {
      assert(((flags ^ bk->me_flags32) & MDBX_WRITEMAP) == 0);
      const size_t usedbytes = pgno_align2os_bytes(bk, head->mm_geo.next);

      mdbx_tn_unlock(bk);

      /* LY: pre-sync without holding lock to reduce latency for writer(s) */
      int rc = (flags & MDBX_WRITEMAP)
                   ? mdbx_msync(&bk->me_dxb_mmap, 0, usedbytes,
                                flags & MDBX_MAPASYNC)
                   : mdbx_filesync(bk->me_dxb_fd, false);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;

      rc = mdbx_tn_lock(bk);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;

      /* LY: head may be changed. */
      head = meta_head(bk);
    }

    if (!META_IS_STEADY(head) || bk->me_lck->li_dirty_volume) {
      mdbx_debug("meta-head %" PRIaPGNO ", %s, sync_pending %" PRIuPTR,
                 container_of(head, page_t, mp_data)->mp_pgno,
                 durable_str(head), bk->me_lck->li_dirty_volume);
      meta_t meta = *head;
      int rc = mdbx_sync_locked(bk, flags | MDBX_SHRINK_ALLOWED, &meta);
      if (unlikely(rc != MDBX_SUCCESS)) {
        if (outside_txn)
          mdbx_tn_unlock(bk);
        return rc;
      }
    }
  }

  if (outside_txn)
    mdbx_tn_unlock(bk);
  return MDBX_SUCCESS;
}

/* Close this write txn's cursors, give parent txn's cursors back to parent.
 *
 * [in] txn     the transaction handle.
 * [in] merge   true to keep changes to parent cursors, false to revert.
 *
 * Returns 0 on success, non-zero on failure. */
static void cursors_eot(MDBX_txn *txn, unsigned merge) {
  for (size_t i = txn->txn_ah_num; i > 0;) {
    for (MDBX_cursor *next, *mc = txn->mt_cursors[--i]; mc; mc = next) {
      mdbx_ensure(txn->mt_book, mc->mc_signature == MDBX_MC_SIGNATURE ||
                                    mc->mc_signature == MDBX_MC_WAIT4EOT);
      next = mc->mc_next;
      if (mc->mc_backup) {
        mdbx_ensure(txn->mt_book,
                    mc->mc_backup->mc_signature == MDBX_MC_BACKUP);
        cursor_unshadow(mc, merge);
        if (mc->mc_backup)
          continue;
      }

      if (mc->mc_signature == MDBX_MC_WAIT4EOT) {
        mc->mc_signature = 0;
        free(mc);
      } else {
        mc->mc_signature = MDBX_MC_READY4CLOSE;
        mc->primal.mc_state8 = 0 /* reset C_UNTRACK */;
      }
    }
    txn->mt_cursors[i] = nullptr;
  }
}

MDBX_milieu *mdbx_tn_book(MDBX_txn *txn) {
  if (unlikely(!txn || txn->mt_signature != MDBX_MT_SIGNATURE))
    return nullptr;
  return txn->mt_book;
}

uint64_t mdbx_tn_id(MDBX_txn *txn) {
  if (unlikely(!txn || txn->mt_signature != MDBX_MT_SIGNATURE))
    return ~(txnid_t)0;
  return txn->mt_txnid;
}

int mdbx_tn_reset(MDBX_txn *txn) {
  if (unlikely(!txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  /* This call is only valid for read-only txns */
  if (unlikely(!(txn->mt_flags & MDBX_RDONLY)))
    return MDBX_EINVAL;

  return txn_end(txn, MDBX_END_RESET);
}

int mdbx_tn_abort(MDBX_txn *txn) {
  if (unlikely(!txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  if (txn->mt_child)
    mdbx_tn_abort(txn->mt_child);

  return txn_end(txn, MDBX_END_ABORT | MDBX_END_SLOT | MDBX_END_FREE);
}

static inline int backlog_size(MDBX_txn *txn) {
  int reclaimed = txn->mt_book->me_reclaimed_pglist
                      ? txn->mt_book->me_reclaimed_pglist[0]
                      : 0;
  return reclaimed + txn->mt_loose_count + txn->mt_end_pgno - txn->mt_next_pgno;
}

/* LY: Prepare a backlog of pages to modify GACO itself,
 * while reclaiming is prohibited. It should be enough to prevent search
 * in page_alloc() during a deleting, when GACO tree is unbalanced. */
static int prep_backlog(MDBX_txn *txn, cursor_t *mc) {
  /* LY: extra page(s) for b-tree rebalancing */
  const int extra = (txn->mt_book->me_flags32 & MDBX_LIFORECLAIM) ? 2 : 1;

  if (backlog_size(txn) < mc->mc_aht->aa.depth16 + extra) {
    int rc = cursor_touch(mc);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;

    int backlog;
    while (unlikely((backlog = backlog_size(txn)) < extra)) {
      rc = page_alloc(mc, 1, nullptr, MDBX_ALLOC_GC);
      if (unlikely(rc != MDBX_SUCCESS)) {
        if (unlikely(rc != MDBX_NOTFOUND))
          return rc;
        break;
      }
    }
  }

  return MDBX_SUCCESS;
}

/* Save the freelist as of this transaction to the GACO.
 * This changes the freelist. Keep trying until it stabilizes. */
static int freelist_save(MDBX_txn *txn) {
  /* bk->me_reclaimed_pglist[] can grow and shrink during this call.
   * bk->me_last_reclaimed and txn->mt_free_pages[] can only grow.
   * Page numbers cannot disappear from txn->mt_free_pages[]. */
  MDBX_cursor mc;
  MDBX_milieu *bk = txn->mt_book;
  int rc, more = 1;
  txnid_t cleanup_reclaimed_id = 0, head_id = 0;
  pgno_t befree_count = 0;
  intptr_t head_room = 0, total_room = 0;
  unsigned cleanup_reclaimed_pos = 0, refill_reclaimed_pos = 0;
  const bool lifo = (bk->me_flags32 & MDBX_LIFORECLAIM) != 0;

  rc = cursor_init(&mc, txn, aht_gaco(txn));
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* MDBX_IUD_RESERVE cancels meminit in ovpage malloc (when no WRITEMAP) */
  const intptr_t clean_limit =
      (bk->me_flags32 & (MDBX_NOMEMINIT | MDBX_WRITEMAP)) ? SSIZE_MAX
                                                          : bk->me_maxfree_1pg;

  assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
again_on_freelist_change:
  assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
  while (1) {
    /* Come back here after each Put() in case freelist changed */
    MDBX_iov key, data;

    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
    if (!lifo) {
      /* If using records from GACO which we have not yet deleted,
       * now delete them and any we reserved for me_reclaimed_pglist. */
      while (cleanup_reclaimed_id < bk->me_last_reclaimed) {
        rc = cursor_first(&mc.primal, &key, nullptr);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        rc = prep_backlog(txn, &mc.primal);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        cleanup_reclaimed_id = head_id = *(txnid_t *)key.iov_base;
        total_room = head_room = 0;
        more = 1;
        assert(cleanup_reclaimed_id <= bk->me_last_reclaimed);
        mc.primal.mc_state8 |= C_RECLAIMING;
        rc = mdbx_cursor_delete(&mc, 0);
        mc.primal.mc_state8 ^= C_RECLAIMING;
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
    } else if (txn->mt_lifo_reclaimed) {
      /* LY: cleanup reclaimed records. */
      while (cleanup_reclaimed_pos < txn->mt_lifo_reclaimed[0]) {
        cleanup_reclaimed_id = txn->mt_lifo_reclaimed[++cleanup_reclaimed_pos];
        key.iov_base = &cleanup_reclaimed_id;
        key.iov_len = sizeof(cleanup_reclaimed_id);
        rc = mdbx_cursor_get(&mc, &key, nullptr, MDBX_SET);
        if (likely(rc != MDBX_NOTFOUND)) {
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
          rc = prep_backlog(txn, &mc.primal);
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
          mc.primal.mc_state8 |= C_RECLAIMING;
          rc = mdbx_cursor_delete(&mc, 0);
          mc.primal.mc_state8 ^= C_RECLAIMING;
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
        }
      }
    }

    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
    if (txn->mt_loose_pages) {
      /* Return loose page numbers to me_reclaimed_pglist,
       * though usually none are left at this point.
       * The pages themselves remain in dirtylist. */
      if (unlikely(!bk->me_reclaimed_pglist) &&
          !(lifo && bk->me_last_reclaimed > 1)) {
        /* Put loose page numbers in mt_free_pages,
         * since unable to return them to me_reclaimed_pglist. */
        page_t *mp = txn->mt_loose_pages;
        if (unlikely((rc = mdbx_pnl_need(&txn->mt_befree_pages,
                                         txn->mt_loose_count)) != 0))
          return rc;
        for (; mp; mp = NEXT_LOOSE_PAGE(mp))
          mdbx_pnl_xappend(txn->mt_befree_pages, mp->mp_pgno);
      } else {
        /* RRBR for loose pages + temp PNL with same */
        if ((rc = mdbx_pnl_need(&bk->me_reclaimed_pglist,
                                2 * txn->mt_loose_count + 1)) != 0)
          goto bailout;
        MDBX_PNL loose = bk->me_reclaimed_pglist +
                         MDBX_PNL_ALLOCLEN(bk->me_reclaimed_pglist) -
                         txn->mt_loose_count;
        unsigned count = 0;
        for (page_t *mp = txn->mt_loose_pages; mp; mp = NEXT_LOOSE_PAGE(mp))
          loose[++count] = mp->mp_pgno;
        loose[0] = count;
        mdbx_pnl_sort(loose);
        mdbx_pnl_xmerge(bk->me_reclaimed_pglist, loose);
      }

      txn->mt_loose_pages = nullptr;
      txn->mt_loose_count = 0;
    }

    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
    if (bk->me_reclaimed_pglist) {
      /* Refund suitable pages into "unallocated" space */
      pgno_t tail = txn->mt_next_pgno;
      pgno_t *const begin = bk->me_reclaimed_pglist + 1;
      pgno_t *const end = begin + bk->me_reclaimed_pglist[0];
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
        bk->me_reclaimed_pglist[0] = (unsigned)(higest + 1 - begin);
#else
        bk->me_reclaimed_pglist[0] -= (unsigned)(higest - begin);
        for (pgno_t *move = begin; higest < end; ++move, ++higest)
          *move = *higest;
#endif /* MDBX_PNL sort-order */
        mdbx_info("refunded %" PRIaPGNO " pages: %" PRIaPGNO " -> %" PRIaPGNO,
                  tail - txn->mt_next_pgno, tail, txn->mt_next_pgno);
        txn->mt_next_pgno = tail;
        assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
      }
    }

    /* Save the PNL of pages freed by this txn, to a single record */
    if (befree_count < txn->mt_befree_pages[0]) {
      if (unlikely(!befree_count)) {
        /* Make sure last page of GACO is touched and on freelist */
        rc = page_search(&mc.primal, nullptr, MDBX_PS_LAST | MDBX_PS_MODIFY);
        if (unlikely(rc && rc != MDBX_NOTFOUND))
          goto bailout;
      }
      pgno_t *befree_pages = txn->mt_befree_pages;
      /* Write to last page of GACO */
      key.iov_len = sizeof(txn->mt_txnid);
      key.iov_base = &txn->mt_txnid;
      do {
        befree_count = befree_pages[0];
        data.iov_len = MDBX_PNL_SIZEOF(befree_pages);
        rc = cursor_put(&mc.primal, &key, &data, MDBX_IUD_RESERVE);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        /* Retry if mt_free_pages[] grew during the Put() */
        befree_pages = txn->mt_befree_pages;
      } while (befree_count < befree_pages[0]);

      mdbx_pnl_sort(befree_pages);
      memcpy(data.iov_base, befree_pages, data.iov_len);

      if (mdbx_debug_enabled(MDBX_DBG_EXTRA)) {
        unsigned i = (unsigned)befree_pages[0];
        mdbx_debug_extra("PNL write txn %" PRIaTXN " root %" PRIaPGNO
                         " num %u, PNL",
                         txn->mt_txnid, aht_gaco(txn)->aa.root, i);
        for (; i; i--)
          mdbx_debug_extra_print(" %" PRIaPGNO "", befree_pages[i]);
        mdbx_debug_extra_print("\n");
      }
      continue;
    }

    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
    const intptr_t rpl_len =
        (bk->me_reclaimed_pglist ? bk->me_reclaimed_pglist[0] : 0) +
        txn->mt_loose_count;
    if (rpl_len && refill_reclaimed_pos == 0)
      refill_reclaimed_pos = 1;

    /* Reserve records for me_reclaimed_pglist[]. Split it if multi-page,
     * to avoid searching GACO for a page range. Use keys in
     * range [1,me_last_reclaimed]: Smaller than txnid of oldest reader. */
    if (total_room >= rpl_len) {
      if (total_room == rpl_len || --more < 0)
        break;
    } else if (head_room >= (intptr_t)bk->me_maxfree_1pg && head_id > 1) {
      /* Keep current record (overflow page), add a new one */
      head_id--;
      refill_reclaimed_pos++;
      head_room = 0;
    }

    if (lifo) {
      if (refill_reclaimed_pos >
          (txn->mt_lifo_reclaimed ? txn->mt_lifo_reclaimed[0] : 0)) {
        /* LY: need just a txn-id for save page list. */
        rc =
            page_alloc(&mc.primal, 0, nullptr, MDBX_ALLOC_GC | MDBX_ALLOC_KICK);
        if (likely(rc == 0))
          /* LY: ok, reclaimed from GACO. */
          continue;
        if (unlikely(rc != MDBX_NOTFOUND))
          /* LY: other troubles... */
          goto bailout;

        /* LY: GACO is empty, will look any free txn-id in high2low order. */
        if (unlikely(bk->me_last_reclaimed < 1)) {
          /* LY: not any txn in the past of GACO. */
          rc = MDBX_MAP_FULL;
          goto bailout;
        }

        if (unlikely(!txn->mt_lifo_reclaimed)) {
          txn->mt_lifo_reclaimed = mdbx_txl_alloc();
          if (unlikely(!txn->mt_lifo_reclaimed)) {
            rc = MDBX_ENOMEM;
            goto bailout;
          }
        }
        /* LY: append the list. */
        rc =
            mdbx_txl_append(&txn->mt_lifo_reclaimed, bk->me_last_reclaimed - 1);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
        --bk->me_last_reclaimed;
        /* LY: note that GACO cleanup is not needed. */
        ++cleanup_reclaimed_pos;
      }
      assert(txn->mt_lifo_reclaimed != nullptr);
      head_id = txn->mt_lifo_reclaimed[refill_reclaimed_pos];
    } else {
      assert(txn->mt_lifo_reclaimed == nullptr);
    }

    /* (Re)write {key = head_id, PNL length = head_room} */
    total_room -= head_room;
    head_room = rpl_len - total_room;
    if (head_room > (intptr_t)bk->me_maxfree_1pg && head_id > 1) {
      /* Overflow multi-page for part of me_reclaimed_pglist */
      head_room /= (head_id < INT16_MAX) ? (pgno_t)head_id
                                         : INT16_MAX; /* amortize page sizes */
      head_room += bk->me_maxfree_1pg - head_room % (bk->me_maxfree_1pg + 1);
    } else if (head_room < 0) {
      /* Rare case, not bothering to delete this record */
      head_room = 0;
      continue;
    }
    key.iov_len = sizeof(head_id);
    key.iov_base = &head_id;
    data.iov_len = (head_room + 1) * sizeof(pgno_t);
    rc = cursor_put(&mc.primal, &key, &data, MDBX_IUD_RESERVE);
    assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;

    /* PNL is initially empty, zero out at least the length */
    pgno_t *pgs = (pgno_t *)data.iov_base;
    intptr_t i = head_room > clean_limit ? head_room : 0;
    do {
      pgs[i] = 0;
    } while (--i >= 0);
    total_room += head_room;
    continue;
  }

  assert(cleanup_reclaimed_pos ==
         (txn->mt_lifo_reclaimed ? txn->mt_lifo_reclaimed[0] : 0));

  /* Fill in the reserved me_reclaimed_pglist records */
  rc = MDBX_SUCCESS;
  assert(mdbx_pnl_check(bk->me_reclaimed_pglist));
  if (bk->me_reclaimed_pglist && bk->me_reclaimed_pglist[0]) {
    MDBX_iov key, data;
    key.iov_len = data.iov_len = 0; /* avoid MSVC warning */
    key.iov_base = data.iov_base = nullptr;

    size_t rpl_left = bk->me_reclaimed_pglist[0];
    pgno_t *rpl_end = bk->me_reclaimed_pglist + rpl_left;
    if (txn->mt_lifo_reclaimed == 0) {
      assert(lifo == 0);
      rc = cursor_first(&mc.primal, &key, &data);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
    } else {
      assert(lifo != 0);
    }

    while (1) {
      txnid_t id;
      if (txn->mt_lifo_reclaimed == 0) {
        assert(lifo == 0);
        id = *(txnid_t *)key.iov_base;
        assert(id <= bk->me_last_reclaimed);
      } else {
        assert(lifo != 0);
        assert(refill_reclaimed_pos > 0 &&
               refill_reclaimed_pos <= txn->mt_lifo_reclaimed[0]);
        id = txn->mt_lifo_reclaimed[refill_reclaimed_pos--];
        key.iov_base = &id;
        key.iov_len = sizeof(id);
        rc = mdbx_cursor_get(&mc, &key, &data, MDBX_SET);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
      assert(cleanup_reclaimed_pos ==
             (txn->mt_lifo_reclaimed ? txn->mt_lifo_reclaimed[0] : 0));

      assert(data.iov_len >= sizeof(pgno_t) * 2);
      size_t chunk_len = (data.iov_len / sizeof(pgno_t)) - 1;
      if (chunk_len > rpl_left)
        chunk_len = rpl_left;
      data.iov_len = (chunk_len + 1) * sizeof(pgno_t);
      key.iov_base = &id;
      key.iov_len = sizeof(id);

      rpl_end -= chunk_len;
      data.iov_base = rpl_end;
      pgno_t save = rpl_end[0];
      rpl_end[0] = (pgno_t)chunk_len;
      assert(mdbx_pnl_check(rpl_end));
      mc.primal.mc_state8 |= C_RECLAIMING;
      rc = cursor_put(&mc.primal, &key, &data, MDBX_IUD_CURRENT);
      mc.primal.mc_state8 ^= C_RECLAIMING;
      assert(cleanup_reclaimed_pos ==
             (txn->mt_lifo_reclaimed ? txn->mt_lifo_reclaimed[0] : 0));
      rpl_end[0] = save;
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;

      rpl_left -= chunk_len;
      if (rpl_left == 0)
        break;

      if (!lifo) {
        rc = cursor_next(&mc.primal, &key, &data, MDBX_NEXT);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
    }
  }

bailout:
  if (txn->mt_lifo_reclaimed) {
    assert(rc || cleanup_reclaimed_pos == txn->mt_lifo_reclaimed[0]);
    if (rc == MDBX_SUCCESS &&
        cleanup_reclaimed_pos != txn->mt_lifo_reclaimed[0]) {
      assert(cleanup_reclaimed_pos < txn->mt_lifo_reclaimed[0]);
      /* LY: zeroed cleanup_idx to force cleanup
       * and refill created GACO records. */
      cleanup_reclaimed_pos = 0;
      /* LY: restart filling */
      total_room = head_room = refill_reclaimed_pos = 0;
      more = 1;
      goto again_on_freelist_change;
    }
    txn->mt_lifo_reclaimed[0] = 0;
    if (txn != bk->me_wpa_txn) {
      mdbx_txl_free(txn->mt_lifo_reclaimed);
      txn->mt_lifo_reclaimed = nullptr;
    }
  }

  return rc;
}

/* Read the databook header before mapping it into memory. */
static int __cold mdbx_read_header(MDBX_milieu *bk, meta_t *meta) {
  assert(offsetof(page_t, mp_meta) == PAGEHDRSZ);
  memset(meta, 0, sizeof(meta_t));
  meta->mm_datasync_sign = MDBX_DATASIGN_WEAK;
  int rc = MDBX_CORRUPTED;

  /* Read twice all meta pages so we can find the latest one. */
  unsigned loop_limit = NUM_METAS * 2;
  for (unsigned loop_count = 0; loop_count < loop_limit; ++loop_count) {
    page_t page;

    /* We don't know the page size on first time.
     * So, just guess it. */
    unsigned guess_pagesize = meta->mm_psize32;
    if (guess_pagesize == 0)
      guess_pagesize =
          (loop_count > NUM_METAS) ? bk->me_psize : bk->me_os_psize;

    const unsigned meta_number = loop_count % NUM_METAS;
    const unsigned offset = guess_pagesize * meta_number;

    unsigned retryleft = 42;
    while (1) {
      int err = mdbx_pread(bk->me_dxb_fd, &page, sizeof(page), offset);
      if (err != MDBX_SUCCESS) {
        mdbx_error("read meta[%u,%u]: %i, %s", offset, (unsigned)sizeof(page),
                   err, mdbx_strerror(err));
        return err;
      }

      page_t again;
      err = mdbx_pread(bk->me_dxb_fd, &again, sizeof(again), offset);
      if (err != MDBX_SUCCESS) {
        mdbx_error("read meta[%u,%u]: %i, %s", offset, (unsigned)sizeof(again),
                   err, mdbx_strerror(err));
        return err;
      }

      if (memcmp(&page, &again, sizeof(page)) == 0 || --retryleft == 0)
        break;

      mdbx_info("meta[%u] was updated, re-read it", meta_number);
    }

    if (!retryleft) {
      mdbx_error("meta[%u] is too volatile, skip it", meta_number);
      continue;
    }

    if (page.mp_pgno != meta_number) {
      mdbx_error("meta[%u] has invalid pageno %" PRIaPGNO, meta_number,
                 page.mp_pgno);
      return MDBX_INVALID;
    }

    if (!F_ISSET(page.mp_flags16, P_META)) {
      mdbx_error("page #%u not a meta-page", meta_number);
      return MDBX_INVALID;
    }

    if (page.mp_meta.mm_magic_and_version != MDBX_DATA_MAGIC) {
      mdbx_error("meta[%u] has invalid magic/version", meta_number);
      return ((page.mp_meta.mm_magic_and_version >> 8) != MDBX_MAGIC)
                 ? MDBX_INVALID
                 : MDBX_VERSION_MISMATCH;
    }

    if (page.mp_meta.mm_txnid_a != page.mp_meta.mm_txnid_b) {
      mdbx_warning("meta[%u] not completely updated, skip it", meta_number);
      continue;
    }

    /* LY: check signature as a checksum */
    if (META_IS_STEADY(&page.mp_meta) &&
        page.mp_meta.mm_datasync_sign != meta_sign(&page.mp_meta)) {
      mdbx_notice("meta[%u] has invalid steady-checksum (0x%" PRIx64
                  " != 0x%" PRIx64 "), skip it",
                  meta_number, page.mp_meta.mm_datasync_sign,
                  meta_sign(&page.mp_meta));
      continue;
    }

    /* LY: check pagesize */
    if (!is_power_of_2(page.mp_meta.mm_psize32) ||
        page.mp_meta.mm_psize32 < MIN_PAGESIZE ||
        page.mp_meta.mm_psize32 > MAX_PAGESIZE) {
      mdbx_notice("meta[%u] has invalid pagesize (%u), skip it", meta_number,
                  page.mp_meta.mm_psize32);
      rc = MDBX_VERSION_MISMATCH;
      continue;
    }

    mdbx_debug("read meta%" PRIaPGNO " = root %" PRIaPGNO "/%" PRIaPGNO
               ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
               " +%u -%u, txn_id %" PRIaTXN ", %s",
               page.mp_pgno, page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_root,
               page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_root,
               page.mp_meta.mm_geo.lower, page.mp_meta.mm_geo.next,
               page.mp_meta.mm_geo.now, page.mp_meta.mm_geo.upper,
               page.mp_meta.mm_geo.grow16, page.mp_meta.mm_geo.shrink16,
               page.mp_meta.mm_txnid_a, durable_str(&page.mp_meta));

    /* LY: check min-pages value */
    if (page.mp_meta.mm_geo.lower < MIN_PAGENO ||
        page.mp_meta.mm_geo.lower > MAX_PAGENO) {
      mdbx_notice("meta[%u] has invalid min-pages (%" PRIaPGNO "), skip it",
                  meta_number, page.mp_meta.mm_geo.lower);
      rc = MDBX_INVALID;
      continue;
    }

    /* LY: check max-pages value */
    if (page.mp_meta.mm_geo.upper < MIN_PAGENO ||
        page.mp_meta.mm_geo.upper > MAX_PAGENO ||
        page.mp_meta.mm_geo.upper < page.mp_meta.mm_geo.lower) {
      mdbx_notice("meta[%u] has invalid max-pages (%" PRIaPGNO "), skip it",
                  meta_number, page.mp_meta.mm_geo.upper);
      rc = MDBX_INVALID;
      continue;
    }

    /* LY: check mapsize limits */
    const uint64_t mapsize_min =
        page.mp_meta.mm_geo.lower * (uint64_t)page.mp_meta.mm_psize32;
    const uint64_t mapsize_max =
        page.mp_meta.mm_geo.upper * (uint64_t)page.mp_meta.mm_psize32;
    STATIC_ASSERT(MAX_MAPSIZE < SSIZE_MAX - MAX_PAGESIZE);
    STATIC_ASSERT(MIN_MAPSIZE < MAX_MAPSIZE);
    if (mapsize_min < MIN_MAPSIZE || mapsize_max > MAX_MAPSIZE) {
      mdbx_notice("meta[%u] has invalid min-mapsize (%" PRIu64 "), skip it",
                  meta_number, mapsize_min);
      rc = MDBX_VERSION_MISMATCH;
      continue;
    }

    STATIC_ASSERT(MIN_MAPSIZE < MAX_MAPSIZE);
    if (mapsize_max > MAX_MAPSIZE ||
        MAX_PAGENO < mdbx_roundup2((size_t)mapsize_max, bk->me_os_psize) /
                         (uint64_t)page.mp_meta.mm_psize32) {
      mdbx_notice("meta[%u] has too large max-mapsize (%" PRIu64 "), skip it",
                  meta_number, mapsize_max);
      rc = MDBX_TOO_LARGE;
      continue;
    }

    /* LY: check end_pgno */
    if (page.mp_meta.mm_geo.now < page.mp_meta.mm_geo.lower ||
        page.mp_meta.mm_geo.now > page.mp_meta.mm_geo.upper) {
      mdbx_notice("meta[%u] has invalid end-pageno (%" PRIaPGNO "), skip it",
                  meta_number, page.mp_meta.mm_geo.now);
      rc = MDBX_CORRUPTED;
      continue;
    }

    /* LY: check last_pgno */
    if (page.mp_meta.mm_geo.next < MIN_PAGENO ||
        page.mp_meta.mm_geo.next - 1 > MAX_PAGENO) {
      mdbx_notice("meta[%u] has invalid next-pageno (%" PRIaPGNO "), skip it",
                  meta_number, page.mp_meta.mm_geo.next);
      rc = MDBX_CORRUPTED;
      continue;
    }

    if (page.mp_meta.mm_geo.next > page.mp_meta.mm_geo.now) {
      mdbx_notice("meta[%u] next-pageno (%" PRIaPGNO
                  ") is beyond end-pgno (%" PRIaPGNO "), skip it",
                  meta_number, page.mp_meta.mm_geo.next,
                  page.mp_meta.mm_geo.now);
      rc = MDBX_CORRUPTED;
      continue;
    }

    /* LY: GACO root */
    if (page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_root == P_INVALID) {
      if (page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_branch_pages ||
          page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_depth16 ||
          page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_entries ||
          page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_leaf_pages ||
          page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_overflow_pages) {
        mdbx_notice("meta[%u] has false-empty GACO, skip it", meta_number);
        rc = MDBX_CORRUPTED;
        continue;
      }
    } else if (page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_root >=
               page.mp_meta.mm_geo.next) {
      mdbx_notice("meta[%u] has invalid GACO-root %" PRIaPGNO ", skip it",
                  meta_number, page.mp_meta.mm_aas[MDBX_GACO_AAH].aa_root);
      rc = MDBX_CORRUPTED;
      continue;
    }

    /* LY: MainDB root */
    if (page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_root == P_INVALID) {
      if (page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_branch_pages ||
          page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_depth16 ||
          page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_entries ||
          page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_leaf_pages ||
          page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_overflow_pages) {
        mdbx_notice("meta[%u] has false-empty maindb", meta_number);
        rc = MDBX_CORRUPTED;
        continue;
      }
    } else if (page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_root >=
               page.mp_meta.mm_geo.next) {
      mdbx_notice("meta[%u] has invalid maindb-root %" PRIaPGNO ", skip it",
                  meta_number, page.mp_meta.mm_aas[MDBX_MAIN_AAH].aa_root);
      rc = MDBX_CORRUPTED;
      continue;
    }

    if (page.mp_meta.mm_txnid_a == 0) {
      mdbx_warning("meta[%u] has zero txnid, skip it", meta_number);
      continue;
    }

    if (meta_ot(prefer_steady, bk, meta, &page.mp_meta)) {
      *meta = page.mp_meta;
      if (META_IS_WEAK(meta))
        loop_limit += 1; /* LY: should re-read to hush race with update */
      mdbx_info("latch meta[%u]", meta_number);
    }
  }

  if (META_IS_WEAK(meta)) {
    mdbx_error("no usable meta-pages, database is corrupted");
    return rc;
  }

  return MDBX_SUCCESS;
}

static int mdbx_sync_locked(MDBX_milieu *bk, unsigned flags,
                            meta_t *const pending) {
  mdbx_assert(bk, ((bk->me_flags32 ^ flags) & MDBX_WRITEMAP) == 0);
  meta_t *const meta0 = metapage(bk, 0);
  meta_t *const meta1 = metapage(bk, 1);
  meta_t *const meta2 = metapage(bk, 2);
  meta_t *const head = meta_head(bk);

  mdbx_assert(bk, meta_eq_mask(bk) == 0);
  mdbx_assert(bk,
              pending < metapage(bk, 0) || pending > metapage(bk, NUM_METAS));
  mdbx_assert(bk, (bk->me_flags32 & (MDBX_RDONLY | MDBX_FATAL_ERROR)) == 0);
  mdbx_assert(bk, !META_IS_STEADY(head) || bk->me_lck->li_dirty_volume != 0);
  mdbx_assert(bk, pending->mm_geo.next <= pending->mm_geo.now);

  const size_t usedbytes = pgno_align2os_bytes(bk, pending->mm_geo.next);
  if (bk->me_lck->li_autosync_threshold &&
      bk->me_lck->li_dirty_volume >= bk->me_lck->li_autosync_threshold)
    flags &= MDBX_WRITEMAP | MDBX_SHRINK_ALLOWED;

  /* LY: step#1 - sync previously written/updated data-pages */
  int rc = MDBX_RESULT_TRUE;
  if (bk->me_lck->li_dirty_volume && (flags & MDBX_NOSYNC) == 0) {
    mdbx_assert(bk, ((flags ^ bk->me_flags32) & MDBX_WRITEMAP) == 0);
    meta_t *const steady = meta_steady(bk);
    if (flags & MDBX_WRITEMAP) {
      rc = mdbx_msync(&bk->me_dxb_mmap, 0, usedbytes, flags & MDBX_MAPASYNC);
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
      if ((flags & MDBX_MAPASYNC) == 0) {
        if (unlikely(pending->mm_geo.next > steady->mm_geo.now)) {
          rc = mdbx_filesize_sync(bk->me_dxb_fd);
          if (unlikely(rc != MDBX_SUCCESS))
            goto fail;
        }
        bk->me_lck->li_dirty_volume = 0;
      }
    } else {
      rc = mdbx_filesync(bk->me_dxb_fd,
                         pending->mm_geo.next > steady->mm_geo.now);
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
      bk->me_lck->li_dirty_volume = 0;
    }
  }

#if defined(_WIN32) || defined(_WIN64)
/* Windows is unable shrinking a mapped file */
#else
  /* LY: check conditions to shrink datafile */
  pgno_t shrink = 0;
  if ((flags & MDBX_SHRINK_ALLOWED) && pending->mm_geo.shrink16 &&
      pending->mm_geo.now - pending->mm_geo.next > pending->mm_geo.shrink16) {
    const pgno_t aligner = pending->mm_geo.grow16 ? pending->mm_geo.grow16
                                                  : pending->mm_geo.shrink16;
    const pgno_t aligned = pgno_align2os_pgno(
        bk, pending->mm_geo.next + aligner - pending->mm_geo.next % aligner);
    if (pending->mm_geo.now > aligned) {
      shrink = pending->mm_geo.now - aligned;
      pending->mm_geo.now = aligned;
      if (meta_txnid_stable(bk, head) == pending->mm_txnid_a)
        meta_set_txnid(bk, pending, pending->mm_txnid_a + 1);
    }
  }
#endif /* not a Windows */

  /* Steady or Weak */
  if (bk->me_lck->li_dirty_volume == 0) {
    pending->mm_datasync_sign = meta_sign(pending);
  } else {
    pending->mm_datasync_sign =
        (flags & MDBX_UTTERLY_NOSYNC) == MDBX_UTTERLY_NOSYNC
            ? MDBX_DATASIGN_NONE
            : MDBX_DATASIGN_WEAK;
  }

  meta_t *target = nullptr;
  if (meta_txnid_stable(bk, head) == pending->mm_txnid_a) {
    mdbx_assert(
        bk, memcmp(&head->mm_aas, &pending->mm_aas, sizeof(head->mm_aas)) == 0);
    mdbx_assert(bk, memcmp(&head->mm_canary, &pending->mm_canary,
                           sizeof(head->mm_canary)) == 0);
    mdbx_assert(bk, memcmp(&head->mm_geo, &pending->mm_geo,
                           sizeof(pending->mm_geo)) == 0);
    if (!META_IS_STEADY(head) && META_IS_STEADY(pending))
      target = head;
    else {
      mdbx_ensure(bk, meta_eq(bk, head, pending));
      mdbx_debug("skip update meta");
      return MDBX_SUCCESS;
    }
  } else if (head == meta0)
    target = meta_ancient(prefer_steady, bk, meta1, meta2);
  else if (head == meta1)
    target = meta_ancient(prefer_steady, bk, meta0, meta2);
  else {
    mdbx_assert(bk, head == meta2);
    target = meta_ancient(prefer_steady, bk, meta0, meta1);
  }

  /* LY: step#2 - update meta-page. */
  mdbx_debug("writing meta%" PRIaPGNO " = root %" PRIaPGNO "/%" PRIaPGNO
             ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
             " +%u -%u, txn_id %" PRIaTXN ", %s",
             container_of(target, page_t, mp_data)->mp_pgno,
             pending->mm_aas[MDBX_MAIN_AAH].aa_root,
             pending->mm_aas[MDBX_GACO_AAH].aa_root, pending->mm_geo.lower,
             pending->mm_geo.next, pending->mm_geo.now, pending->mm_geo.upper,
             pending->mm_geo.grow16, pending->mm_geo.shrink16,
             pending->mm_txnid_a, durable_str(pending));

  mdbx_debug("meta0: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO
             "/%" PRIaPGNO,
             (meta0 == head) ? "head" : (meta0 == target) ? "tail" : "stay",
             durable_str(meta0), meta_txnid_fluid(bk, meta0),
             meta0->mm_aas[MDBX_MAIN_AAH].aa_root,
             meta0->mm_aas[MDBX_GACO_AAH].aa_root);
  mdbx_debug("meta1: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO
             "/%" PRIaPGNO,
             (meta1 == head) ? "head" : (meta1 == target) ? "tail" : "stay",
             durable_str(meta1), meta_txnid_fluid(bk, meta1),
             meta1->mm_aas[MDBX_MAIN_AAH].aa_root,
             meta1->mm_aas[MDBX_GACO_AAH].aa_root);
  mdbx_debug("meta2: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO
             "/%" PRIaPGNO,
             (meta2 == head) ? "head" : (meta2 == target) ? "tail" : "stay",
             durable_str(meta2), meta_txnid_fluid(bk, meta2),
             meta2->mm_aas[MDBX_MAIN_AAH].aa_root,
             meta2->mm_aas[MDBX_GACO_AAH].aa_root);

  mdbx_assert(bk, !meta_eq(bk, pending, meta0));
  mdbx_assert(bk, !meta_eq(bk, pending, meta1));
  mdbx_assert(bk, !meta_eq(bk, pending, meta2));

  mdbx_assert(bk, ((bk->me_flags32 ^ flags) & MDBX_WRITEMAP) == 0);
  mdbx_ensure(bk, target == head ||
                      meta_txnid_stable(bk, target) < pending->mm_txnid_a);
  if (bk->me_flags32 & MDBX_WRITEMAP) {
    jitter4testing(true);
    if (likely(target != head)) {
      /* LY: 'invalidate' the meta. */
      target->mm_datasync_sign = MDBX_DATASIGN_WEAK;
      meta_update_begin(bk, target, pending->mm_txnid_a);
#ifndef NDEBUG
      /* debug: provoke failure to catch a violators */
      memset(target->mm_aas, 0xCC,
             sizeof(target->mm_aas) + sizeof(target->mm_canary));
      jitter4testing(false);
#endif

      /* LY: update info */
      target->mm_geo = pending->mm_geo;
      target->mm_aas[MDBX_GACO_AAH] = pending->mm_aas[MDBX_GACO_AAH];
      target->mm_aas[MDBX_MAIN_AAH] = pending->mm_aas[MDBX_MAIN_AAH];
      target->mm_canary = pending->mm_canary;
      jitter4testing(true);
      mdbx_coherent_barrier();

      /* LY: 'commit' the meta */
      meta_update_end(bk, target, pending->mm_txnid_b);
      jitter4testing(true);
    } else {
      /* dangerous case (target == head), only mm_datasync_sign could
       * me updated, check assertions once again */
      mdbx_ensure(bk, meta_txnid_stable(bk, head) == pending->mm_txnid_a &&
                          !META_IS_STEADY(head) && META_IS_STEADY(pending));
      mdbx_ensure(bk, memcmp(&head->mm_geo, &pending->mm_geo,
                             sizeof(head->mm_geo)) == 0);
      mdbx_ensure(bk, memcmp(&head->mm_aas, &pending->mm_aas,
                             sizeof(head->mm_aas)) == 0);
      mdbx_ensure(bk, memcmp(&head->mm_canary, &pending->mm_canary,
                             sizeof(head->mm_canary)) == 0);
    }
    target->mm_datasync_sign = pending->mm_datasync_sign;
    mdbx_coherent_barrier();
    jitter4testing(true);
  } else {
    rc = mdbx_pwrite(bk->me_dxb_fd, pending, sizeof(meta_t),
                     (uint8_t *)target - bk->me_map);
    if (unlikely(rc != MDBX_SUCCESS)) {
    undo:
      mdbx_debug("write failed, disk error?");
      /* On a failure, the pagecache still contains the new data.
       * Try write some old data back, to prevent it from being used. */
      mdbx_pwrite(bk->me_dxb_fd, (void *)target, sizeof(meta_t),
                  (uint8_t *)target - bk->me_map);
      goto fail;
    }
    mdbx_invalidate_cache(target, sizeof(meta_t));
  }

  /* LY: step#3 - sync meta-pages. */
  mdbx_assert(bk, ((bk->me_flags32 ^ flags) & MDBX_WRITEMAP) == 0);
  if ((flags & (MDBX_NOSYNC | MDBX_NOMETASYNC)) == 0) {
    mdbx_assert(bk, ((flags ^ bk->me_flags32) & MDBX_WRITEMAP) == 0);
    if (flags & MDBX_WRITEMAP) {
      const size_t offset = ((uint8_t *)container_of(head, page_t, mp_meta)) -
                            bk->me_dxb_mmap.dxb;
      const size_t paged_offset = offset & ~(bk->me_os_psize - 1);
      const size_t paged_length =
          mdbx_roundup2(bk->me_psize + offset - paged_offset, bk->me_os_psize);
      rc = mdbx_msync(&bk->me_dxb_mmap, paged_offset, paged_length,
                      flags & MDBX_MAPASYNC);
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
    } else {
      rc = mdbx_filesync(bk->me_dxb_fd, false);
      if (rc != MDBX_SUCCESS)
        goto undo;
    }
  }

#if defined(_WIN32) || defined(_WIN64)
/* Windows is unable shrinking a mapped file */
#else
  /* LY: shrink datafile if needed */
  if (unlikely(shrink)) {
    mdbx_info("shrink to %" PRIaPGNO " pages (-%" PRIaPGNO ")",
              pending->mm_geo.now, shrink);
    rc = mdbx_mapresize(bk, pending->mm_geo.now, pending->mm_geo.upper);
    if (MDBX_IS_ERROR(rc))
      goto fail;
  }
#endif /* not a Windows */

  return MDBX_SUCCESS;

fail:
  bk->me_flags32 |= MDBX_FATAL_ERROR;
  return rc;
}

#define mdbx_nodemax(pagesize)                                                 \
  (((((pagesize)-PAGEHDRSZ) / MDBX_MINKEYS) & -(intptr_t)2) - sizeof(indx_t))

#define mdbx_maxkey(nodemax) ((nodemax) - (NODESIZE + sizeof(aatree_t)))

#define mdbx_maxfree1pg(pagesize) (((pagesize)-PAGEHDRSZ) / sizeof(pgno_t) - 1)

int mdbx_pagesize2maxkeylen(size_t pagesize) {
  if (pagesize == 0)
    pagesize = mdbx_syspagesize();

  intptr_t nodemax = mdbx_nodemax(pagesize);
  if (nodemax < 0)
    return -MDBX_EINVAL;

  intptr_t maxkey = mdbx_maxkey(nodemax);
  return (maxkey > 0 && maxkey < INT_MAX) ? (int)maxkey : -MDBX_EINVAL;
}

static void __cold setup_pagesize(MDBX_milieu *bk, const size_t pagesize) {
  STATIC_ASSERT(SSIZE_MAX > MAX_MAPSIZE);
  STATIC_ASSERT(MIN_PAGESIZE > sizeof(page_t));
  mdbx_ensure(bk, is_power_of_2(pagesize));
  mdbx_ensure(bk, pagesize >= MIN_PAGESIZE);
  mdbx_ensure(bk, pagesize <= MAX_PAGESIZE);
  bk->me_psize = (unsigned)pagesize;

  STATIC_ASSERT(mdbx_maxfree1pg(MIN_PAGESIZE) > 42);
  STATIC_ASSERT(mdbx_maxfree1pg(MAX_PAGESIZE) < MDBX_PNL_DB_MAX);
  const intptr_t maxfree_1pg = (pagesize - PAGEHDRSZ) / sizeof(pgno_t) - 1;
  mdbx_ensure(bk, maxfree_1pg > 42 && maxfree_1pg < MDBX_PNL_DB_MAX);
  bk->me_maxfree_1pg = (unsigned)maxfree_1pg;

  STATIC_ASSERT(mdbx_nodemax(MIN_PAGESIZE) > 42);
  STATIC_ASSERT(mdbx_nodemax(MAX_PAGESIZE) < UINT16_MAX);
  const intptr_t nodemax = mdbx_nodemax(pagesize);
  mdbx_ensure(bk, nodemax > 42 && nodemax < UINT16_MAX);
  bk->me_nodemax = (unsigned)nodemax;

  STATIC_ASSERT(mdbx_maxkey(MIN_PAGESIZE) > 42);
  STATIC_ASSERT(mdbx_maxkey(MIN_PAGESIZE) < MIN_PAGESIZE);
  STATIC_ASSERT(mdbx_maxkey(MAX_PAGESIZE) > 42);
  STATIC_ASSERT(mdbx_maxkey(MAX_PAGESIZE) < MAX_PAGESIZE);
  const intptr_t maxkey_limit = mdbx_maxkey(bk->me_nodemax);
  mdbx_ensure(bk, maxkey_limit > 42 && (size_t)maxkey_limit < pagesize);
  bk->me_keymax = (unsigned)maxkey_limit;

  bk->me_psize2log = uint_log2_ceil(pagesize);
  mdbx_assert(bk, pgno2bytes(bk, 1) == pagesize);
  mdbx_assert(bk, bytes2pgno(bk, pagesize + pagesize) == 2);
}

int __cold mdbx_bk_init(MDBX_milieu **pbk) {
  MDBX_milieu *bk = calloc(1, sizeof(MDBX_milieu));
  if (!bk)
    return MDBX_ENOMEM;

  bk->me_maxreaders = DEFAULT_READERS;
  bk->env_ah_max = bk->env_ah_num = CORE_AAH;
  bk->me_dxb_fd = MDBX_INVALID_FD;
  bk->me_lck_fd = MDBX_INVALID_FD;
  bk->me_pid = mdbx_getpid();

  int rc;
  const size_t os_psize = mdbx_syspagesize();
  if (!is_power_of_2(os_psize) || os_psize < MIN_PAGESIZE) {
    mdbx_error("unsuitable system pagesize %" PRIuPTR, os_psize);
    rc = MDBX_INCOMPATIBLE;
    goto bailout;
  }
  bk->me_os_psize = (unsigned)os_psize;
  setup_pagesize(bk, bk->me_os_psize);

  rc = mdbx_fastmutex_init(&bk->me_aah_lock);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  VALGRIND_CREATE_MEMPOOL(bk, 0, 0);
  bk->me_signature = MDBX_ME_SIGNATURE;
  *pbk = bk;
  return MDBX_SUCCESS;

bailout:
  free(bk);
  *pbk = nullptr;
  return rc;
}

static int __cold mdbx_bk_map(MDBX_milieu *bk, size_t usedsize) {
  int rc = mdbx_mmap(bk->me_flags32, &bk->me_dxb_mmap, bk->me_bookgeo.now,
                     bk->me_bookgeo.upper);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

#ifdef MADV_DONTFORK
  if (madvise(bk->me_map, bk->me_mapsize, MADV_DONTFORK))
    return mdbx_get_errno();
#endif

#ifdef MADV_NOHUGEPAGE
  (void)madvise(bk->me_map, bk->me_mapsize, MADV_NOHUGEPAGE);
#endif

#if defined(MADV_DODUMP) && defined(MADV_DONTDUMP)
  const size_t meta_length = pgno2bytes(bk, NUM_METAS);
  (void)madvise(bk->me_map, meta_length, MADV_DODUMP);
  if (!(bk->me_flags32 & MDBX_PAGEPERTURB))
    (void)madvise(bk->me_map + meta_length, bk->me_mapsize - meta_length,
                  MADV_DONTDUMP);
#endif

#ifdef MADV_REMOVE
  if (usedsize && (bk->me_flags32 & MDBX_WRITEMAP)) {
    (void)madvise(bk->me_map + usedsize, bk->me_mapsize - usedsize,
                  MADV_REMOVE);
  }
#else
  (void)usedsize;
#endif

#if defined(MADV_RANDOM) && defined(MADV_WILLNEED)
  /* Turn on/off readahead. It's harmful when the databook is larger than RAM.
   */
  if (madvise(bk->me_map, bk->me_mapsize,
              (bk->me_flags32 & MDBX_NORDAHEAD) ? MADV_RANDOM : MADV_WILLNEED))
    return mdbx_get_errno();
#endif

#ifdef USE_VALGRIND
  bk->me_valgrind_handle =
      VALGRIND_CREATE_BLOCK(bk->me_map, bk->me_mapsize, "mdbx");
#endif

  return MDBX_SUCCESS;
}

LIBMDBX_API int __cold mdbx_set_geometry(MDBX_milieu *bk, intptr_t size_lower,
                                         intptr_t size_now, intptr_t size_upper,
                                         intptr_t growth_step,
                                         intptr_t shrink_threshold,
                                         intptr_t pagesize) {
  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  const bool outside_txn =
      (!bk->me_wpa_txn || bk->me_wpa_txn->mt_owner != mdbx_thread_self());

#if MDBX_DEBUG
  if (growth_step < 0)
    growth_step = 1;
  if (shrink_threshold < 0)
    shrink_threshold = 1;
#endif

  int rc = MDBX_PROBLEM;
  if (bk->me_map) {
    /* bk already mapped */
    if (!bk->me_lck || (bk->me_flags32 & MDBX_RDONLY))
      return MDBX_EACCESS;

    if (outside_txn) {
      int err = mdbx_tn_lock(bk);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    meta_t *head = meta_head(bk);

    if (pagesize < 0)
      /* TODO: ioctl(BLKSSZGET) */
      pagesize = bk->me_psize;
    if (pagesize != (intptr_t)bk->me_psize) {
      rc = MDBX_EINVAL;
      goto bailout;
    }

    if (size_lower < 0)
      size_lower = pgno2bytes(bk, head->mm_geo.lower);
    if (size_now < 0)
      size_now = pgno2bytes(bk, head->mm_geo.now);
    if (size_upper < 0)
      size_upper = pgno2bytes(bk, head->mm_geo.upper);
    if (growth_step < 0)
      growth_step = pgno2bytes(bk, head->mm_geo.grow16);
    if (shrink_threshold < 0)
      shrink_threshold = pgno2bytes(bk, head->mm_geo.shrink16);

    const size_t usedbytes = pgno2bytes(bk, head->mm_geo.next);
    if ((size_t)size_upper < usedbytes) {
      rc = MDBX_MAP_FULL;
      goto bailout;
    }
    if ((size_t)size_now < usedbytes)
      size_now = usedbytes;
#if defined(_WIN32) || defined(_WIN64)
    if ((size_t)size_now < bk->me_dbgeo.now ||
        (size_t)size_upper < bk->me_dbgeo.upper) {
      /* Windows is unable shrinking a mapped file */
      return ERROR_USER_MAPPED_FILE;
    }
#endif /* Windows */
  } else {
    /* bk NOT yet mapped */
    if (!outside_txn) {
      rc = MDBX_PANIC;
      goto bailout;
    }

    if (pagesize < 0) {
      pagesize = bk->me_os_psize;
      if (pagesize > MAX_PAGESIZE)
        pagesize = MAX_PAGESIZE;
      mdbx_assert(bk, pagesize >= MIN_PAGESIZE);
    }
  }

  if (pagesize < MIN_PAGESIZE || pagesize > MAX_PAGESIZE ||
      !is_power_of_2(pagesize)) {
    rc = MDBX_EINVAL;
    goto bailout;
  }

  if (size_lower < 0) {
    size_lower = MIN_MAPSIZE;
    if (MIN_MAPSIZE / pagesize < MIN_PAGENO)
      size_lower = MIN_PAGENO * pagesize;
  }

  if (size_now < 0) {
    size_now = DEFAULT_MAPSIZE;
    if (size_now < size_lower)
      size_now = size_lower;
  }

  if (size_upper < 0) {
    if ((size_t)size_now >= MAX_MAPSIZE / 2)
      size_upper = MAX_MAPSIZE;
    else if (MAX_MAPSIZE != MAX_MAPSIZE32 &&
             (size_t)size_now >= MAX_MAPSIZE32 / 2)
      size_upper = MAX_MAPSIZE32;
    else {
      size_upper = size_now + size_now;
      if ((size_t)size_upper < DEFAULT_MAPSIZE * 2)
        size_upper = DEFAULT_MAPSIZE * 2;
    }
    if ((size_t)size_upper / pagesize > MAX_PAGENO)
      size_upper = pagesize * MAX_PAGENO;
  }

  if (unlikely(size_lower < MIN_MAPSIZE || size_lower > size_upper)) {
    rc = MDBX_EINVAL;
    goto bailout;
  }

  if ((uint64_t)size_lower / pagesize < MIN_PAGENO) {
    rc = MDBX_EINVAL;
    goto bailout;
  }

  if (unlikely((size_t)size_upper > MAX_MAPSIZE ||
               (uint64_t)size_upper / pagesize > MAX_PAGENO)) {
    rc = MDBX_TOO_LARGE;
    goto bailout;
  }

  size_lower = mdbx_roundup2(size_lower, bk->me_os_psize);
  size_upper = mdbx_roundup2(size_upper, bk->me_os_psize);
  size_now = mdbx_roundup2(size_now, bk->me_os_psize);

  /* LY: подбираем значение size_upper:
   *  - кратное размеру системной страницы
   *  - без нарушения MAX_MAPSIZE или MAX_PAGENO */
  while (unlikely((size_t)size_upper > MAX_MAPSIZE ||
                  (uint64_t)size_upper / pagesize > MAX_PAGENO)) {
    if ((size_t)size_upper < bk->me_os_psize + MIN_MAPSIZE ||
        (size_t)size_upper < bk->me_os_psize * (MIN_PAGENO + 1)) {
      /* паранойа на случай переполнения при невероятных значениях */
      rc = MDBX_EINVAL;
      goto bailout;
    }
    size_upper -= bk->me_os_psize;
    if ((size_t)size_upper > (size_t)size_lower)
      size_lower = size_upper;
  }
  mdbx_assert(bk, (size_upper - size_lower) % bk->me_os_psize == 0);

  if (size_now < size_lower)
    size_now = size_lower;
  if (size_now > size_upper)
    size_now = size_upper;

  if (growth_step < 0) {
    growth_step = ((size_t)(size_upper - size_lower)) / 42;
    if (growth_step > size_lower)
      growth_step = size_lower;
    if (growth_step < 65536)
      growth_step = 65536;
    if ((size_t)growth_step > MEGABYTE * 16)
      growth_step = MEGABYTE * 16;
  }
  growth_step = mdbx_roundup2(growth_step, bk->me_os_psize);
  if (bytes2pgno(bk, growth_step) > UINT16_MAX)
    growth_step = pgno2bytes(bk, UINT16_MAX);

  if (shrink_threshold < 0) {
    shrink_threshold = growth_step + growth_step;
    if (shrink_threshold < growth_step)
      shrink_threshold = growth_step;
  }
  shrink_threshold = mdbx_roundup2(shrink_threshold, bk->me_os_psize);
  if (bytes2pgno(bk, shrink_threshold) > UINT16_MAX)
    shrink_threshold = pgno2bytes(bk, UINT16_MAX);

  /* save user's geo-params for future open/create */
  bk->me_bookgeo.lower = size_lower;
  bk->me_bookgeo.now = size_now;
  bk->me_bookgeo.upper = size_upper;
  bk->me_bookgeo.grow = growth_step;
  bk->me_bookgeo.shrink = shrink_threshold;
  rc = MDBX_SUCCESS;

  if (bk->me_map) {
    /* apply new params */
    mdbx_assert(bk, pagesize == (intptr_t)bk->me_psize);

    meta_t *head = meta_head(bk);
    meta_t meta = *head;
    meta.mm_geo.lower = bytes2pgno(bk, bk->me_bookgeo.lower);
    meta.mm_geo.now = bytes2pgno(bk, bk->me_bookgeo.now);
    meta.mm_geo.upper = bytes2pgno(bk, bk->me_bookgeo.upper);
    meta.mm_geo.grow16 = (uint16_t)bytes2pgno(bk, bk->me_bookgeo.grow);
    meta.mm_geo.shrink16 = (uint16_t)bytes2pgno(bk, bk->me_bookgeo.shrink);

    mdbx_assert(bk, bk->me_bookgeo.lower >= MIN_MAPSIZE);
    mdbx_assert(bk, meta.mm_geo.lower >= MIN_PAGENO);
    mdbx_assert(bk, bk->me_bookgeo.upper <= MAX_MAPSIZE);
    mdbx_assert(bk, meta.mm_geo.upper <= MAX_PAGENO);
    mdbx_assert(bk, meta.mm_geo.now >= meta.mm_geo.next);
    mdbx_assert(bk, bk->me_bookgeo.upper >= bk->me_bookgeo.lower);
    mdbx_assert(bk, meta.mm_geo.upper >= meta.mm_geo.now);
    mdbx_assert(bk, meta.mm_geo.now >= meta.mm_geo.lower);
    mdbx_assert(bk, meta.mm_geo.grow16 == bytes2pgno(bk, bk->me_bookgeo.grow));
    mdbx_assert(bk,
                meta.mm_geo.shrink16 == bytes2pgno(bk, bk->me_bookgeo.shrink));

    if (memcmp(&meta.mm_geo, &head->mm_geo, sizeof(meta.mm_geo))) {
      if (meta.mm_geo.now != head->mm_geo.now ||
          meta.mm_geo.upper != head->mm_geo.upper) {
        rc = mdbx_mapresize(bk, meta.mm_geo.now, meta.mm_geo.upper);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      }
      meta_set_txnid(bk, &meta, meta_txnid_stable(bk, head) + 1);
      rc = mdbx_sync_locked(bk, bk->me_flags32, &meta);
    }
  } else if (pagesize != (intptr_t)bk->me_psize) {
    setup_pagesize(bk, pagesize);
  }

bailout:
  if (bk->me_map && outside_txn)
    mdbx_tn_unlock(bk);
  return rc;
}

int __cold mdbx_set_mapsize(MDBX_milieu *bk, size_t size) {
  return mdbx_set_geometry(bk, -1, size, -1, -1, -1, -1);
}

int __cold mdbx_set_max_handles(MDBX_milieu *bk, MDBX_aah count) {
  if (unlikely(count > MAX_AAH))
    return MDBX_EINVAL;

  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(bk->me_map))
    return MDBX_EPERM;

  bk->env_ah_max = count + CORE_AAH;
  return MDBX_SUCCESS;
}

int __cold mdbx_set_maxreaders(MDBX_milieu *bk, unsigned readers) {
  if (unlikely(readers < 1 || readers > INT16_MAX))
    return MDBX_EINVAL;

  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(bk->me_map))
    return MDBX_EPERM;

  bk->me_maxreaders = readers;
  return MDBX_SUCCESS;
}

int __cold mdbx_get_maxreaders(MDBX_milieu *bk, unsigned *readers) {
  if (!bk || !readers)
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  *readers = bk->me_maxreaders;
  return MDBX_SUCCESS;
}

/* Further setup required for opening an MDBX databook */
static int __cold mdbx_setup_dxb(MDBX_milieu *bk, int lck_rc) {
  meta_t meta;
  int rc = MDBX_RESULT_FALSE;
  int err = mdbx_read_header(bk, &meta);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE || err != MDBX_ENODATA ||
        (bk->me_flags32 & MDBX_RDONLY) != 0)
      return err;

    mdbx_debug("create new database");
    rc = /* new database */ MDBX_RESULT_TRUE;

    if (!bk->me_bookgeo.now) {
      /* set defaults if not configured */
      err = mdbx_set_mapsize(bk, DEFAULT_MAPSIZE);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }

    void *buffer = calloc(NUM_METAS, bk->me_psize);
    if (!buffer)
      return MDBX_ENOMEM;

    meta = init_metas(bk, buffer)->mp_meta;
    err = mdbx_pwrite(bk->me_dxb_fd, buffer, bk->me_psize * NUM_METAS, 0);
    free(buffer);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    err = mdbx_ftruncate(bk->me_dxb_fd, bk->me_bookgeo.now);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

#ifndef NDEBUG /* just for checking */
    err = mdbx_read_header(bk, &meta);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
#endif
  }

  mdbx_info(
      "header: root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO "/%" PRIaPGNO
      "-%" PRIaPGNO "/%" PRIaPGNO " +%u -%u, txn_id %" PRIaTXN ", %s",
      meta.mm_aas[MDBX_MAIN_AAH].aa_root, meta.mm_aas[MDBX_GACO_AAH].aa_root,
      meta.mm_geo.lower, meta.mm_geo.next, meta.mm_geo.now, meta.mm_geo.upper,
      meta.mm_geo.grow16, meta.mm_geo.shrink16, meta.mm_txnid_a,
      durable_str(&meta));

  setup_pagesize(bk, meta.mm_psize32);
  if ((bk->me_flags32 & MDBX_RDONLY) /* readonly */
      || lck_rc != MDBX_RESULT_TRUE /* not exclusive */) {
    /* use present params from db */
    err = mdbx_set_geometry(
        bk, meta.mm_geo.lower * meta.mm_psize32,
        meta.mm_geo.now * meta.mm_psize32, meta.mm_geo.upper * meta.mm_psize32,
        meta.mm_geo.grow16 * meta.mm_psize32,
        meta.mm_geo.shrink16 * meta.mm_psize32, meta.mm_psize32);
    if (unlikely(err != MDBX_SUCCESS)) {
      mdbx_error("could not use present dbsize-params from db");
      return MDBX_INCOMPATIBLE;
    }
  } else if (bk->me_bookgeo.now) {
    /* silently growth to last used page */
    const size_t used_bytes = pgno2bytes(bk, meta.mm_geo.next);
    if (bk->me_bookgeo.lower < used_bytes)
      bk->me_bookgeo.lower = used_bytes;
    if (bk->me_bookgeo.now < used_bytes)
      bk->me_bookgeo.now = used_bytes;
    if (bk->me_bookgeo.upper < used_bytes)
      bk->me_bookgeo.upper = used_bytes;

    /* apply preconfigured params, but only if substantial changes:
     *  - upper or lower limit changes
     *  - shrink theshold or growth step
     * But ignore just chagne just a 'now/current' size. */
    if (bytes_align2os_bytes(bk, bk->me_bookgeo.upper) !=
            pgno_align2os_bytes(bk, meta.mm_geo.upper) ||
        bytes_align2os_bytes(bk, bk->me_bookgeo.lower) !=
            pgno_align2os_bytes(bk, meta.mm_geo.lower) ||
        bytes_align2os_bytes(bk, bk->me_bookgeo.shrink) !=
            pgno_align2os_bytes(bk, meta.mm_geo.shrink16) ||
        bytes_align2os_bytes(bk, bk->me_bookgeo.grow) !=
            pgno_align2os_bytes(bk, meta.mm_geo.grow16)) {

      if (bk->me_bookgeo.shrink && bk->me_bookgeo.now > used_bytes)
        /* pre-shrink if enabled */
        bk->me_bookgeo.now = used_bytes + bk->me_bookgeo.shrink -
                             used_bytes % bk->me_bookgeo.shrink;

      err = mdbx_set_geometry(bk, bk->me_bookgeo.lower, bk->me_bookgeo.now,
                              bk->me_bookgeo.upper, bk->me_bookgeo.grow,
                              bk->me_bookgeo.shrink, meta.mm_psize32);
      if (unlikely(err != MDBX_SUCCESS)) {
        mdbx_error("could not apply preconfigured dbsize-params to db");
        return MDBX_INCOMPATIBLE;
      }

      /* update meta fields */
      meta.mm_geo.now = bytes2pgno(bk, bk->me_bookgeo.now);
      meta.mm_geo.lower = bytes2pgno(bk, bk->me_bookgeo.lower);
      meta.mm_geo.upper = bytes2pgno(bk, bk->me_bookgeo.upper);
      meta.mm_geo.grow16 = (uint16_t)bytes2pgno(bk, bk->me_bookgeo.grow);
      meta.mm_geo.shrink16 = (uint16_t)bytes2pgno(bk, bk->me_bookgeo.shrink);

      mdbx_info("amended: root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO
                "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
                " +%u -%u, txn_id %" PRIaTXN ", %s",
                meta.mm_aas[MDBX_MAIN_AAH].aa_root,
                meta.mm_aas[MDBX_GACO_AAH].aa_root, meta.mm_geo.lower,
                meta.mm_geo.next, meta.mm_geo.now, meta.mm_geo.upper,
                meta.mm_geo.grow16, meta.mm_geo.shrink16, meta.mm_txnid_a,
                durable_str(&meta));
    }
    mdbx_ensure(bk, meta.mm_geo.now >= meta.mm_geo.next);
  } else {
    /* geo-params not pre-configured by user,
     * get current values from a meta. */
    bk->me_bookgeo.now = pgno2bytes(bk, meta.mm_geo.now);
    bk->me_bookgeo.lower = pgno2bytes(bk, meta.mm_geo.lower);
    bk->me_bookgeo.upper = pgno2bytes(bk, meta.mm_geo.upper);
    bk->me_bookgeo.grow = pgno2bytes(bk, meta.mm_geo.grow16);
    bk->me_bookgeo.shrink = pgno2bytes(bk, meta.mm_geo.shrink16);
  }

  uint64_t filesize_before_mmap;
  err = mdbx_filesize(bk->me_dxb_fd, &filesize_before_mmap);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  const size_t expected_bytes =
      mdbx_roundup2(pgno2bytes(bk, meta.mm_geo.now), bk->me_os_psize);
  const size_t used_bytes = pgno2bytes(bk, meta.mm_geo.next);
  mdbx_ensure(bk, expected_bytes >= used_bytes);
  if (filesize_before_mmap != expected_bytes) {
    if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE) {
      mdbx_info("filesize mismatch (expect %" PRIuPTR "/%" PRIaPGNO
                ", have %" PRIu64 "/%" PRIaPGNO "), "
                "assume collision in non-exclusive mode",
                expected_bytes, bytes2pgno(bk, expected_bytes),
                filesize_before_mmap,
                bytes2pgno(bk, (size_t)filesize_before_mmap));
    } else {
      mdbx_notice("filesize mismatch (expect %" PRIuPTR "/%" PRIaPGNO
                  ", have %" PRIu64 "/%" PRIaPGNO ")",
                  expected_bytes, bytes2pgno(bk, expected_bytes),
                  filesize_before_mmap,
                  bytes2pgno(bk, (size_t)filesize_before_mmap));
      if (filesize_before_mmap < used_bytes) {
        mdbx_error("last-page beyond end-of-file (last %" PRIaPGNO
                   ", have %" PRIaPGNO ")",
                   meta.mm_geo.next,
                   bytes2pgno(bk, (size_t)filesize_before_mmap));
        return MDBX_CORRUPTED;
      }

      if (bk->me_flags32 & MDBX_RDONLY) {
        mdbx_notice("ignore filesize mismatch in readonly-mode");
      } else {
        mdbx_info("resize datafile to %" PRIu64 " bytes, %" PRIaPGNO " pages",
                  expected_bytes, bytes2pgno(bk, expected_bytes));
        err = mdbx_ftruncate(bk->me_dxb_fd, expected_bytes);
        if (unlikely(err != MDBX_SUCCESS)) {
          mdbx_error("error %d, while resize datafile to %" PRIu64
                     " bytes, %" PRIaPGNO " pages",
                     rc, expected_bytes, bytes2pgno(bk, expected_bytes));
          return err;
        }
        filesize_before_mmap = expected_bytes;
      }
    }
  }

  err = mdbx_bk_map(bk, (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE)
                            ? 0
                            : expected_bytes);
  if (err != MDBX_SUCCESS)
    return err;

  const unsigned meta_clash_mask = meta_eq_mask(bk);
  if (meta_clash_mask) {
    mdbx_error("meta-pages are clashed: mask 0x%d", meta_clash_mask);
    return MDBX_WANNA_RECOVERY;
  }

  while (1) {
    meta_t *head = meta_head(bk);
    const txnid_t head_txnid = meta_txnid_fluid(bk, head);
    if (head_txnid == meta.mm_txnid_a)
      break;

    if (lck_rc == /* lck exclusive */ MDBX_RESULT_TRUE) {
      assert(META_IS_STEADY(&meta) && !META_IS_STEADY(head));
      if (bk->me_flags32 & MDBX_RDONLY) {
        mdbx_error("rollback needed: (from head %" PRIaTXN
                   " to steady %" PRIaTXN "), but unable in read-only mode",
                   head_txnid, meta.mm_txnid_a);
        return MDBX_WANNA_RECOVERY /* LY: could not recovery/rollback */;
      }

      /* LY: rollback weak checkpoint */
      mdbx_trace("rollback: from %" PRIaTXN ", to %" PRIaTXN, head_txnid,
                 meta.mm_txnid_a);
      mdbx_ensure(bk, head_txnid == meta_txnid_stable(bk, head));

      if (bk->me_flags32 & MDBX_WRITEMAP) {
        head->mm_txnid_a = 0;
        head->mm_datasync_sign = MDBX_DATASIGN_WEAK;
        head->mm_txnid_b = 0;
        const size_t offset = ((uint8_t *)container_of(head, page_t, mp_meta)) -
                              bk->me_dxb_mmap.dxb;
        const size_t paged_offset = offset & ~(bk->me_os_psize - 1);
        const size_t paged_length = mdbx_roundup2(
            bk->me_psize + offset - paged_offset, bk->me_os_psize);
        err = mdbx_msync(&bk->me_dxb_mmap, paged_offset, paged_length, false);
      } else {
        meta_t rollback = *head;
        meta_set_txnid(bk, &rollback, 0);
        rollback.mm_datasync_sign = MDBX_DATASIGN_WEAK;
        err = mdbx_pwrite(bk->me_dxb_fd, &rollback, sizeof(meta_t),
                          (uint8_t *)head - (uint8_t *)bk->me_map);
      }
      if (err)
        return err;

      mdbx_invalidate_cache(bk->me_map, pgno2bytes(bk, NUM_METAS));
      mdbx_ensure(bk, 0 == meta_txnid_fluid(bk, head));
      mdbx_ensure(bk, 0 == meta_eq_mask(bk));
      continue;
    }

    if (!bk->me_lck) {
      /* LY: without-lck (read-only) mode, so it is imposible that other
       * process made weak checkpoint. */
      mdbx_error("without-lck, unable recovery/rollback");
      return MDBX_WANNA_RECOVERY;
    }

    /* LY: assume just have a collision with other running process,
     *     or someone make a weak checkpoint */
    mdbx_info("assume collision or online weak checkpoint");
    break;
  }

  const meta_t *head = meta_head(bk);
  if (lck_rc == /* lck exclusive */ MDBX_RESULT_TRUE) {
    /* re-check file size after mmap */
    uint64_t filesize_after_mmap;
    err = mdbx_filesize(bk->me_dxb_fd, &filesize_after_mmap);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (filesize_after_mmap != expected_bytes) {
      if (filesize_after_mmap != filesize_before_mmap)
        mdbx_info("datafile resized by system to %" PRIu64 " bytes",
                  filesize_after_mmap);
      if (filesize_after_mmap % bk->me_os_psize ||
          filesize_after_mmap > bk->me_bookgeo.upper ||
          filesize_after_mmap < used_bytes) {
        mdbx_info("unacceptable/unexpected  datafile size %" PRIu64,
                  filesize_after_mmap);
        return MDBX_PROBLEM;
      }
      if ((bk->me_flags32 & MDBX_RDONLY) == 0) {
        meta.mm_geo.now =
            bytes2pgno(bk, bk->me_bookgeo.now = (size_t)filesize_after_mmap);
        mdbx_info("update meta-geo to filesize %" PRIuPTR " bytes, %" PRIaPGNO
                  " pages",
                  bk->me_bookgeo.now, meta.mm_geo.now);
      }
    }

    if (memcmp(&meta.mm_geo, &head->mm_geo, sizeof(meta.mm_geo))) {
      const txnid_t txnid = meta_txnid_stable(bk, head);
      mdbx_info("updating meta.geo: "
                "from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                "/s%u-g%u (txn#%" PRIaTXN "), "
                "to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                "/s%u-g%u (txn#%" PRIaTXN ")",
                head->mm_geo.lower, head->mm_geo.now, head->mm_geo.upper,
                head->mm_geo.shrink16, head->mm_geo.grow16, txnid,
                meta.mm_geo.lower, meta.mm_geo.now, meta.mm_geo.upper,
                meta.mm_geo.shrink16, meta.mm_geo.grow16, txnid + 1);

      mdbx_ensure(bk, meta_eq(bk, &meta, head));
      meta_set_txnid(bk, &meta, txnid + 1);
      bk->me_lck->li_dirty_volume += bk->me_psize;
      err = mdbx_sync_locked(bk, bk->me_flags32 | MDBX_SHRINK_ALLOWED, &meta);
      if (err) {
        mdbx_info("error %d, while updating meta.geo: "
                  "from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                  "/s%u-g%u (txn#%" PRIaTXN "), "
                  "to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                  "/s%u-g%u (txn#%" PRIaTXN ")",
                  err, head->mm_geo.lower, head->mm_geo.now, head->mm_geo.upper,
                  head->mm_geo.shrink16, head->mm_geo.grow16, txnid,
                  meta.mm_geo.lower, meta.mm_geo.now, meta.mm_geo.upper,
                  meta.mm_geo.shrink16, meta.mm_geo.grow16, txnid + 1);
        return err;
      }
    }
  }

  return rc;
}

/****************************************************************************/

/* Open and/or initialize the lock region for the databook. */
static int __cold setup_lck(MDBX_milieu *bk, const char *lck_pathname,
                            mode_t mode) {
  assert(bk->me_dxb_fd != MDBX_INVALID_FD);
  assert(bk->me_lck_fd == MDBX_INVALID_FD);

  int err = mdbx_openfile(lck_pathname, O_RDWR | O_CREAT, mode, &bk->me_lck_fd);
  if (err != MDBX_SUCCESS) {
    if (err != MDBX_EROFS || (bk->me_flags32 & MDBX_RDONLY) == 0)
      return err;
    /* LY: without-lck mode (e.g. on read-only filesystem) */
    bk->me_lck_fd = MDBX_INVALID_FD;
    bk->me_oldest = &bk->me_oldest_stub;
    bk->me_maxreaders = UINT_MAX;
    mdbx_debug("lck-setup: %s ", "lockless mode (readonly)");
    return MDBX_SUCCESS;
  }

  /* Try to get exclusive lock. If we succeed, then
   * nobody is using the lock region and we should initialize it. */
  const int rc = mdbx_lck_seize(bk);
  if (MDBX_IS_ERROR(rc))
    return rc;

  mdbx_debug("lck-setup: %s ",
             (rc == MDBX_RESULT_TRUE) ? "exclusive" : "shared");

  uint64_t size;
  err = mdbx_filesize(bk->me_lck_fd, &size);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (rc == MDBX_RESULT_TRUE) {
    uint64_t wanna = mdbx_roundup2(
        (bk->me_maxreaders - 1) * sizeof(MDBX_reader) + sizeof(MDBX_lockinfo),
        bk->me_os_psize);
#ifndef NDEBUG
    err = mdbx_ftruncate(bk->me_lck_fd, size = 0);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
#endif
    jitter4testing(false);

    if (size != wanna) {
      err = mdbx_ftruncate(bk->me_lck_fd, wanna);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      size = wanna;
    }
  } else if (size > SSIZE_MAX || (size & (bk->me_os_psize - 1)) ||
             size < bk->me_os_psize) {
    mdbx_notice("lck-file has invalid size %" PRIu64 " bytes", size);
    return MDBX_PROBLEM;
  }

  const size_t maxreaders =
      ((size_t)size - sizeof(MDBX_lockinfo)) / sizeof(MDBX_reader) + 1;
  if (maxreaders > UINT16_MAX) {
    mdbx_error("lck-size too big (up to %" PRIuPTR " readers)", maxreaders);
    return MDBX_PROBLEM;
  }
  bk->me_maxreaders = (unsigned)maxreaders;

  err = mdbx_mmap(MDBX_WRITEMAP, &bk->me_lck_mmap, (size_t)size, (size_t)size);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

#ifdef MADV_DODUMP
  (void)madvise(bk->me_lck, size, MADV_DODUMP);
#endif

#ifdef MADV_DONTFORK
  if (madvise(bk->me_lck, size, MADV_DONTFORK) < 0)
    return mdbx_get_errno();
#endif

#ifdef MADV_WILLNEED
  if (madvise(bk->me_lck, size, MADV_WILLNEED) < 0)
    return mdbx_get_errno();
#endif

#ifdef MADV_RANDOM
  if (madvise(bk->me_lck, size, MADV_RANDOM) < 0)
    return mdbx_get_errno();
#endif

  if (rc == MDBX_RESULT_TRUE) {
    /* LY: exlcusive mode, init lck */
    memset(bk->me_lck, 0, (size_t)size);
    err = mdbx_lck_init(bk);
    if (err)
      return err;

    bk->me_lck->li_magic_and_version = MDBX_LOCK_MAGIC;
    bk->me_lck->li_os_and_format = MDBX_LOCK_FORMAT;
  } else {
    if (bk->me_lck->li_magic_and_version != MDBX_LOCK_MAGIC) {
      mdbx_error("lock region has invalid magic/version");
      return ((bk->me_lck->li_magic_and_version >> 8) != MDBX_MAGIC)
                 ? MDBX_INVALID
                 : MDBX_VERSION_MISMATCH;
    }
    if (bk->me_lck->li_os_and_format != MDBX_LOCK_FORMAT) {
      mdbx_error("lock region has os/format 0x%" PRIx32 ", expected 0x%" PRIx32,
                 bk->me_lck->li_os_and_format, MDBX_LOCK_FORMAT);
      return MDBX_VERSION_MISMATCH;
    }
  }

  mdbx_assert(bk, !MDBX_IS_ERROR(rc));
  bk->me_oldest = &bk->me_lck->li_oldest;
  return rc;
}

int __cold mdbx_bk_open(MDBX_milieu *bk, const char *path, unsigned flags,
                        mode_t mode4create) {
  int rc = mdbx_is_directory(path);
  if (MDBX_IS_ERROR(rc)) {
    if (rc != MDBX_ENOENT || (flags & MDBX_RDONLY) != 0)
      return rc;
  }

  STATIC_ASSERT((MDBX_REGIME_CHANGEABLE & MDBX_REGIME_CHANGELESS) == 0);
  STATIC_ASSERT(((MDBX_AA_FLAGS | MDBX_DB_FLAGS | MDBX_OPEN_FLAGS) &
                 (MDBX_REGIME_CHANGEABLE | MDBX_REGIME_CHANGELESS)) == 0);

  return mdbx_bk_open_ex(
      bk, nullptr /* required base address */, path /* dxb pathname */,
      (rc != MDBX_RESULT_TRUE) ? "." : nullptr /* lck pathname */,
      nullptr /* ovf pathname */, flags /* regime flags */,
      MDBX_REGIME_PRINCIPAL_FLAGS /* regime check mask */,
      nullptr /* regime present */, mode4create);
}

int __cold mdbx_bk_open_ex(MDBX_milieu *bk, void *required_base_address,
                           const char *dxb_pathname, const char *lck_pathname,
                           const char *ovf_pathname, unsigned regime_flags,
                           unsigned regime_check_mask, unsigned *regime_present,
                           mode_t mode4create) {
  if (unlikely(!bk || !dxb_pathname))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;
  if (unlikely(bk->me_pid != mdbx_getpid()))
    return MDBX_PANIC;

  if (bk->me_dxb_fd != MDBX_INVALID_FD || (regime_flags & ~MDBX_ENV_FLAGS) != 0)
    return MDBX_EINVAL;

  if (ovf_pathname &&
      (ovf_pathname[0] == '\0' || strcmp(ovf_pathname, ".") == 0))
    ovf_pathname = nullptr;

  if (ovf_pathname && required_base_address)
    /* FIXME: This is correct (and could be implemented),
     * in the case fixed databook size */
    return MDBX_EINVAL;

  if (ovf_pathname || required_base_address)
    return MDBX_ENOSYS /* FIXME: TODO - implement both features */;

  char *molded_lck_filename = nullptr;
  char *molded_dxb_filename = nullptr;
  int rc = mdbx_is_directory(dxb_pathname);
  if (!lck_pathname) {
    /* subdir mode:
     *   - dxb and lck resides in directory given by dxb_pathname. */
    if (MDBX_IS_ERROR(rc))
      goto bailout;
    if (rc != MDBX_RESULT_TRUE) {
      rc = MDBX_EINVAL;
      goto bailout;
    }

    if (mdbx_asprintf(&molded_dxb_filename, "%s%s", dxb_pathname,
                      MDBX_PATH_SEPARATOR MDBX_DATANAME) < 0 ||
        mdbx_asprintf(&molded_lck_filename, "%s%s", dxb_pathname,
                      MDBX_PATH_SEPARATOR MDBX_LOCKNAME) < 0) {
      rc = mdbx_get_errno();
      goto bailout;
    }
    dxb_pathname = molded_dxb_filename;
    lck_pathname = molded_lck_filename;
  } else if (lck_pathname[0] == '\0' || strcmp(lck_pathname, ".") == 0) {
    /* no-subdir mode:
     *   - dxb given by dxb_pathname,
     *   - lck is same with MDBX_LOCK_SUFFIX appended. */
    if (rc == MDBX_RESULT_TRUE) {
      rc = MDBX_EINVAL;
      goto bailout;
    }
    if (mdbx_asprintf(&molded_lck_filename, "%s%s", dxb_pathname,
                      MDBX_LOCK_SUFFIX) < 0) {
      rc = mdbx_get_errno();
      goto bailout;
    }
    lck_pathname = molded_lck_filename;
  } else {
    /* separate mode: dxb given by dxb_pathname, lck given by lck_pathname */
    if (rc == MDBX_RESULT_TRUE ||
        mdbx_is_directory(lck_pathname) == MDBX_RESULT_TRUE) {
      rc = MDBX_EINVAL;
      goto bailout;
    }
  }

  if (regime_flags & MDBX_RDONLY) {
    /* LY: silently ignore irrelevant flags when
     * we're only getting read access */
    regime_flags &=
        ~(MDBX_WRITEMAP | MDBX_MAPASYNC | MDBX_NOSYNC | MDBX_NOMETASYNC |
          MDBX_COALESCE | MDBX_LIFORECLAIM | MDBX_NOMEMINIT);
  } else if (!((bk->me_free_pgs = mdbx_pnl_alloc(MDBX_PNL_UM_MAX)) &&
               (bk->me_dirtylist =
                    calloc(MDBX_PNL_UM_SIZE, sizeof(MDBX_ID2))))) {
    rc = MDBX_ENOMEM;
    goto bailout;
  }

  regime_check_mask &=
      MDBX_REGIME_PRINCIPAL_FLAGS /* silentry ignore all non-regime flags */;
  bk->me_flags32 = regime_flags | MDBX_ENV_ACTIVE;

  const size_t pathname_buflen = strlen(lck_pathname) + 1 +
                                 strlen(dxb_pathname) + 1 +
                                 (ovf_pathname ? strlen(ovf_pathname) : 0) + 1;
  bk->me_pathname_buf = malloc(pathname_buflen);
  bk->env_ahe_array = calloc(bk->env_ah_max, sizeof(ahe_t));
  if (!(bk->env_ahe_array && bk->me_pathname_buf)) {
    rc = MDBX_ENOMEM;
    goto bailout;
  }
  bk->env_ahe_array[MDBX_GACO_AAH].ax_kcmp =
      cmp_int_aligned; /* aligned MDBX_INTEGERKEY */

  char *append_ptr = bk->me_pathname_buf;
  append_ptr = mdbx_stpcpy(bk->me_pathname_dxb = append_ptr, dxb_pathname);
  append_ptr = mdbx_stpcpy(bk->me_pathname_lck = append_ptr, lck_pathname);
  if (ovf_pathname)
    append_ptr = mdbx_stpcpy(bk->me_pathname_ovf = append_ptr, ovf_pathname);
  else
    *append_ptr++ = '\0';
  mdbx_assert(bk, append_ptr == bk->me_pathname_buf + pathname_buflen);

  int oflags;
  if (F_ISSET(regime_flags, MDBX_RDONLY))
    oflags = O_RDONLY;
  else
    oflags = O_RDWR | O_CREAT;

  rc = mdbx_openfile(bk->me_pathname_dxb, oflags, mode4create, &bk->me_dxb_fd);
  if (rc != MDBX_SUCCESS)
    goto bailout;

  const int lck_rc = setup_lck(bk, bk->me_pathname_lck, mode4create);
  if (MDBX_IS_ERROR(lck_rc)) {
    rc = lck_rc;
    goto bailout;
  }

  const int dxb_rc = mdbx_setup_dxb(bk, lck_rc);
  if (MDBX_IS_ERROR(dxb_rc)) {
    rc = dxb_rc;
    goto bailout;
  }

  mdbx_debug("opened dbenv %p", (void *)bk);
  if (lck_rc == MDBX_RESULT_TRUE) {
    /* setup regime */
    bk->me_lck->li_regime =
        bk->me_flags32 & (MDBX_REGIME_PRINCIPAL_FLAGS | MDBX_RDONLY);
    if (regime_flags & MDBX_EXCLUSIVE) {
      rc = mdbx_lck_downgrade(bk, false);
      mdbx_debug("lck-downgrade-partial: rc %i ", rc);
      bk->me_flags32 |= MDBX_EXCLUSIVE;
    } else {
      /* LY: downgrade lock only if exclusive access not requested.
       *     in case exclusive==1, just leave value as is. */
      rc = mdbx_lck_downgrade(bk, true);
      mdbx_debug("lck-downgrade-full: rc %i ", rc);
    }
    if (rc != MDBX_SUCCESS)
      goto bailout;
  } else {
    if ((bk->me_flags32 & MDBX_RDONLY) == 0) {
      /* update */
      while (bk->me_lck->li_regime == MDBX_RDONLY) {
        if (mdbx_atomic_compare_and_swap32(&bk->me_lck->li_regime, MDBX_RDONLY,
                                           bk->me_flags32 &
                                               MDBX_REGIME_PRINCIPAL_FLAGS))
          break;
        /* TODO: yield/relax cpu */
      }
      if ((bk->me_lck->li_regime ^ bk->me_flags32) & regime_check_mask) {
        mdbx_error("current mode/flags incompatible with requested");
        rc = MDBX_INCOMPATIBLE;
        goto bailout;
      }
    }
  }

  if (bk->me_lck && (bk->me_flags32 & MDBX_NOTLS) == 0) {
    rc = rthc_alloc(&bk->me_txkey, &bk->me_lck->li_readers[0],
                    &bk->me_lck->li_readers[bk->me_maxreaders]);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
    bk->me_flags32 |= MDBX_ENV_TXKEY;
  }

  if ((regime_flags & MDBX_RDONLY) == 0) {
    MDBX_txn *txn;
    size_t tsize = sizeof(MDBX_txn),
           size =
               tsize + bk->env_ah_max * (sizeof(aht_t) + sizeof(MDBX_cursor *));
    if ((bk->me_pagebuf = calloc(1, bk->me_psize)) && (txn = calloc(1, size))) {
      txn->txn_aht_array = (aht_t *)((char *)txn + tsize);
      txn->mt_cursors = (MDBX_cursor **)(txn->txn_aht_array + bk->env_ah_max);
      txn->mt_book = bk;
      txn->mt_flags = MDBX_TXN_FINISHED;
      bk->me_wpa_txn = txn;
    } else {
      rc = MDBX_ENOMEM;
    }
  }

#if MDBX_DEBUG
  if (rc == MDBX_SUCCESS) {
    meta_t *meta = meta_head(bk);
    aatree_t *db = &meta->mm_aas[MAIN_AAH];

    mdbx_debug("opened database version %u, pagesize %u",
               (uint8_t)meta->mm_magic_and_version, bk->me_psize);
    mdbx_debug("using meta page %" PRIaPGNO ", txn %" PRIaTXN "",
               container_of(meta, page_t, mp_data)->mp_pgno,
               meta_txnid_fluid(bk, meta));
    mdbx_debug("depth: %u", db->aa_depth);
    mdbx_debug("entries: %" PRIu64 "", db->aa_entries);
    mdbx_debug("branch pages: %" PRIaPGNO "", db->aa_branch_pages);
    mdbx_debug("leaf pages: %" PRIaPGNO "", db->aa_leaf_pages);
    mdbx_debug("overflow pages: %" PRIaPGNO "", db->aa_overflow_pages);
    mdbx_debug("root: %" PRIaPGNO "", db->aa_root);
  }
#endif

bailout:
  if (regime_present)
    *regime_present = bk->me_lck ? bk->me_lck->li_regime | MDBX_ENV_ACTIVE : 0;
  if (rc)
    bk_release(bk);

  free(molded_lck_filename);
  free(molded_dxb_filename);
  return rc;
}

/* Destroy resources from mdbx_bk_open(), clear our readers & AAHs */
static void __cold bk_release(MDBX_milieu *bk) {
  if (!(bk->me_flags32 & MDBX_ENV_ACTIVE))
    return;
  bk->me_flags32 &= ~MDBX_ENV_ACTIVE;

  /* Doing this here since me_dbxs may not exist during mdbx_bk_close */
  if (bk->env_ahe_array) {
    for (unsigned i = bk->env_ah_max; --i >= CORE_AAH;)
      free(bk->env_ahe_array[i].ax_ident.iov_base);
    free(bk->env_ahe_array);
  }

  free(bk->me_pagebuf);
  free(bk->me_dirtylist);
  if (bk->me_wpa_txn) {
    mdbx_txl_free(bk->me_wpa_txn->mt_lifo_reclaimed);
    free(bk->me_wpa_txn);
  }
  mdbx_pnl_free(bk->me_free_pgs);

  if (bk->me_flags32 & MDBX_ENV_TXKEY) {
    rthc_remove(bk->me_txkey);
    bk->me_flags32 &= ~MDBX_ENV_TXKEY;
  }

  if (bk->me_map) {
    mdbx_munmap(&bk->me_dxb_mmap);
#ifdef USE_VALGRIND
    VALGRIND_DISCARD(bk->me_valgrind_handle);
    bk->me_valgrind_handle = -1;
#endif
  }
  if (bk->me_dxb_fd != MDBX_INVALID_FD) {
    (void)mdbx_closefile(bk->me_dxb_fd);
    bk->me_dxb_fd = MDBX_INVALID_FD;
  }

  if (bk->me_lck)
    mdbx_munmap(&bk->me_lck_mmap);
  bk->me_oldest = nullptr;

  mdbx_lck_destroy(bk);
  if (bk->me_lck_fd != MDBX_INVALID_FD) {
    (void)mdbx_closefile(bk->me_lck_fd);
    bk->me_lck_fd = MDBX_INVALID_FD;
  }

  bk->me_pathname_lck = nullptr;
  bk->me_pathname_dxb = nullptr;
  bk->me_pathname_ovf = nullptr;
  free(bk->me_pathname_buf);
}

int __cold mdbx_bk_shutdown(MDBX_milieu *bk, int dont_sync) {
  page_t *dp;
  int rc = MDBX_SUCCESS;

  if (unlikely(!bk))
    return MDBX_EINVAL;
  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;
  if (unlikely(bk->me_pid != mdbx_getpid()))
    return MDBX_EBADSIGN;

  if (!dont_sync && !(bk->me_flags32 & MDBX_RDONLY))
    rc = mdbx_bk_sync(bk, true);

  VALGRIND_DESTROY_MEMPOOL(bk);
  while ((dp = bk->me_dpages) != nullptr) {
    ASAN_UNPOISON_MEMORY_REGION(&dp->mp_next, sizeof(dp->mp_next));
    VALGRIND_MAKE_MEM_DEFINED(&dp->mp_next, sizeof(dp->mp_next));
    bk->me_dpages = dp->mp_next;
    free(dp);
  }

  bk_release(bk);
  mdbx_ensure(bk, mdbx_fastmutex_destroy(&bk->me_aah_lock) == MDBX_SUCCESS);
  bk->me_signature = 0;
  bk->me_pid = 0;
  free(bk);

  return rc;
}

static int cursor_put(cursor_t *mc, MDBX_iov *key, MDBX_iov *data,
                      unsigned flags) {
  page_t *fp, *sub_root = nullptr;
  uint16_t fp_flags;
  MDBX_iov xdata, *rdata, dkey, olddata;
  unsigned mcount = 0, dcount = 0;
  size_t nsize;
  unsigned nflags;
  DKBUF;
  aatree_t dummy;

  MDBX_milieu *bk = mc->mc_txn->mt_book;
  cursor_t *const subordinate = cursor_subordinate(mc);
  int rc = MDBX_SUCCESS;

  /* Check this first so counter will always be zero on any early failures. */
  if (flags & MDBX_IUD_MULTIPLE) {
    if (unlikely(!F_ISSET(mc->mc_aht->aa.flags16, MDBX_DUPFIXED)))
      return MDBX_INCOMPATIBLE;
    if (unlikely(data[1].iov_len >= INT_MAX))
      return MDBX_EINVAL;
    dcount = (unsigned)data[1].iov_len;
    data[1].iov_len = 0;
  }

  const unsigned nospill = flags & MDBX_IUD_NOSPILL;
  flags &= ~MDBX_IUD_NOSPILL;

  mdbx_debug("==> put db %d key [%s], size %" PRIuPTR
             ", data [%s] size %" PRIuPTR,
             DAAH(mc), DKEY(key), key ? key->iov_len : 0,
             DVAL((flags & MDBX_IUD_RESERVE) ? nullptr : data), data->iov_len);

  int dupdata_flag = 0;
  if (flags & MDBX_IUD_CURRENT) {
    /* Опция MDBX_IUD_CURRENT означает, что запрошено обновление текущей
     * записи, на которой сейчас стоит курсор. Проверяем что переданный ключ
     * совпадает  со значением в текущей позиции курсора.
     * Здесь проще вызвать mdbx_cursor_get(), так как для обслуживания таблиц
     * с MDBX_DUPSORT также требуется текущий размер данных. */
    MDBX_iov current_key, current_data;
    rc = cursor_get(mc, &current_key, &current_data, MDBX_GET_CURRENT);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    if (unlikely(cursor_compare_keys(mc, key, &current_key) != 0))
      return MDBX_EKEYMISMATCH;

    if (mc->mc_kind8 & S_HAVESUB) {
      assert(mc->mc_aht->aa.flags16 & MDBX_DUPSORT);
      node_t *leaf = node_ptr(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
      if (leaf->node_flags8 & NODE_DUP) {
        assert(subordinate != nullptr &&
               (subordinate->mc_state8 & C_INITIALIZED));
        /* Если за ключом более одного значения, либо если размер данных
         * отличается, то вместо inplace обновления требуется удаление и
         * последующая вставка. */
        if (subordinate_subcursor(subordinate)->mx_aht_body.aa.entries > 1 ||
            current_data.iov_len != data->iov_len) {
          rc = cursor_delete(mc, 0);
          if (unlikely(rc != MDBX_SUCCESS))
            return rc;
          flags -= MDBX_IUD_CURRENT;
        }
      }
    } else {
      assert(mc->mc_aht->aa.flags16 & MDBX_DUPSORT);
    }
  }

  if (mc->mc_aht->aa.root == P_INVALID) {
    /* new database, cursor has nothing to point to */
    mc->mc_snum = 0;
    mc->mc_top = 0;
    mc->mc_state8 &= ~C_INITIALIZED;
    rc = MDBX_NO_ROOT;
  } else if ((flags & MDBX_IUD_CURRENT) == 0) {
    int exact = 0;
    MDBX_iov d2;
    if (flags & MDBX_IUD_APPEND) {
      MDBX_iov k2;
      rc = cursor_last(mc, &k2, &d2);
      if (rc == MDBX_SUCCESS) {
        if (cursor_compare_keys(mc, key, &k2) > 0) {
          rc = MDBX_NOTFOUND;
          mc->mc_ki[mc->mc_top]++;
        } else {
          /* new key is <= last key */
          rc = MDBX_EKEYMISMATCH;
        }
      }
    } else {
      rc = cursor_set(mc, key, &d2, MDBX_SET, &exact);
    }
    if ((flags & MDBX_IUD_NOOVERWRITE) && rc == MDBX_SUCCESS) {
      mdbx_debug("duplicate key [%s]", DKEY(key));
      *data = d2;
      return MDBX_KEYEXIST;
    }
    if (rc != MDBX_SUCCESS && unlikely(rc != MDBX_NOTFOUND))
      return rc;
  }

  mc->mc_state8 &= ~C_AFTERDELETE;

  /* Cursor is positioned, check for room in the dirty list */
  if (!nospill) {
    if (unlikely(flags & MDBX_IUD_MULTIPLE)) {
      rdata = &xdata;
      xdata.iov_len = data->iov_len * dcount;
    } else {
      rdata = data;
    }
    int err = page_spill(mc, key, rdata);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }

  if (rc == MDBX_NO_ROOT) {
    page_t *np;
    /* new database, write a root leaf page */
    mdbx_debug("allocating new root leaf page");
    int err = page_new(mc, P_LEAF, 1, &np);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    assert(np->mp_flags16 & P_LEAF);
    err = cursor_push(mc, np);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    mc->mc_aht->aa.root = np->mp_pgno;
    mc->mc_aht->aa.depth16++;
    mc->mc_aht->ah.state8 |= MDBX_AAH_DIRTY;
    if (mc->mc_kind8 & S_DUPFIXED) {
      assert((mc->mc_aht->aa.flags16 & (MDBX_DUPSORT | MDBX_DUPFIXED)) ==
             MDBX_DUPFIXED);
      np->mp_flags16 |= P_DFL;
    } else {
      assert((mc->mc_aht->aa.flags16 & (MDBX_DUPSORT | MDBX_DUPFIXED)) !=
             MDBX_DUPFIXED);
    }
    mc->mc_state8 |= C_INITIALIZED;
  } else {
    /* make sure all cursor pages are writable */
    int err = cursor_touch(mc);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }

  bool insert_key, insert_data, do_sub = false;
  insert_key = insert_data = (rc != MDBX_SUCCESS);
  if (insert_key) {
    /* The key does not exist */
    mdbx_debug("inserting key at index %i", mc->mc_ki[mc->mc_top]);
    assert(((mc->mc_aht->aa.flags16 & MDBX_DUPSORT) != 0) ==
           ((mc->mc_kind8 & S_HAVESUB) != 0));
    if ((mc->mc_kind8 & S_HAVESUB) && LEAFSIZE(key, data) > bk->me_nodemax) {
      /* Too big for a node, insert in sub-AA.  Set up an empty
       * "old sub-page" for prep_subDB to expand to a full page. */
      fp_flags = P_LEAF | P_DIRTY;
      fp = bk->me_pagebuf;
      fp->mp_leaf2_ksize16 =
          (uint16_t)data->iov_len; /* used if MDBX_DUPFIXED */
      fp->mp_lower = fp->mp_upper = 0;
      olddata.iov_len = PAGEHDRSZ;
      goto prep_subDB;
    }
  } else {
    /* there's only a key anyway, so this is a no-op */
    if (IS_DFL(mc->mc_pg[mc->mc_top])) {
      const unsigned keysize = mc->mc_aht->aa.xsize32;
      if (key->iov_len != keysize)
        return MDBX_BAD_VALSIZE;
      else {
        void *ptr =
            DFLKEY(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top], keysize);
        memcpy(ptr, key->iov_base, keysize);
      }
    fix_parent:
      /* if overwriting slot 0 of leaf, need to
       * update branch key if there is a parent page */
      if (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
        unsigned dtop = 1;
        mc->mc_top--;
        /* slot 0 is always an empty key, find real slot */
        while (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
          mc->mc_top--;
          dtop++;
        }
        int err = (mc->mc_ki[mc->mc_top]) ? update_key(mc, key) : MDBX_SUCCESS;
        assert(mc->mc_top + dtop < UINT16_MAX);
        mc->mc_top += (uint16_t)dtop;
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      return MDBX_SUCCESS;
    }

  more:;
    node_t *leaf = node_ptr(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
    olddata.iov_len = node_get_datasize(leaf);
    olddata.iov_base = NODEDATA(leaf);

    /* AA has dups? */
    if (mc->mc_kind8 & S_HAVESUB) {
      assert(mc->mc_aht->aa.flags16 & MDBX_DUPSORT);
      /* Prepare (sub-)page/sub-AA to accept the new item, if needed.
       * fp: old sub-page or a header faking it.
       * mp: new (sub-)page.  offset: growth in page size.
       * xdata: node data with new page or AA. */
      unsigned i, offset = 0;
      page_t *mp = fp = xdata.iov_base = bk->me_pagebuf;
      mp->mp_pgno = mc->mc_pg[mc->mc_top]->mp_pgno;

      /* Was a single item before, must convert now */
      if (!F_ISSET(leaf->node_flags8, NODE_DUP)) {

        /* does data match? */
        if (!cursor_compare_data(mc, data, &olddata)) {
          if (unlikely(flags & (MDBX_IUD_NODUP | MDBX_IUD_APPENDDUP)))
            return MDBX_KEYEXIST;
          /* overwrite it */
          goto current;
        }

        /* Just overwrite the current item */
        if (flags & MDBX_IUD_CURRENT)
          goto current;

        /* Back up original data item */
        dupdata_flag = 1;
        dkey.iov_len = olddata.iov_len;
        dkey.iov_base = memcpy(fp + 1, olddata.iov_base, olddata.iov_len);

        /* Make sub-page header for the dup items, with dummy body */
        fp->mp_flags16 = P_LEAF | P_DIRTY | P_SUBP;
        fp->mp_lower = 0;
        xdata.iov_len = PAGEHDRSZ + dkey.iov_len + data->iov_len;
        if (mc->mc_kind8 & S_DUPFIXED) {
          assert(mc->mc_aht->aa.flags16 & MDBX_DUPFIXED);
          fp->mp_flags16 |= P_DFL;
          fp->mp_leaf2_ksize16 = (uint16_t)data->iov_len;
          xdata.iov_len += 2 * data->iov_len; /* leave space for 2 more */
        } else {
          assert(!(mc->mc_aht->aa.flags16 & MDBX_DUPFIXED));
          xdata.iov_len += 2 * (sizeof(indx_t) + NODESIZE) +
                           (dkey.iov_len & 1) + (data->iov_len & 1);
        }
        fp->mp_upper = (uint16_t)(xdata.iov_len - PAGEHDRSZ);
        olddata.iov_len = xdata.iov_len; /* pretend olddata is fp */
      } else if (leaf->node_flags8 & NODE_SUBTREE) {
        /* Data is on sub-AA, just store it */
        flags |= NODE_DUP | NODE_SUBTREE;
        goto put_sub;
      } else {
        /* Data is on sub-page */
        fp = olddata.iov_base;
        switch (flags) {
        default:
          if ((mc->mc_kind8 & S_DUPFIXED) == 0) {
            assert(!(mc->mc_aht->aa.flags16 & MDBX_DUPFIXED));
            offset = EVEN(NODESIZE + sizeof(indx_t) + data->iov_len);
            break;
          } else {
            assert(mc->mc_aht->aa.flags16 & MDBX_DUPFIXED);
          }
          offset = fp->mp_leaf2_ksize16;
          if (page_spaceleft(fp) < offset) {
            offset *= 4; /* space for 4 more */
            break;
          }
        /* FALLTHRU: Big enough MDBX_DUPFIXED sub-page */
        case MDBX_IUD_CURRENT | MDBX_IUD_NODUP:
        case MDBX_IUD_CURRENT:
          fp->mp_flags16 |= P_DIRTY;
          fp->mp_pgno = mp->mp_pgno;
          subordinate->mc_pg[0] = fp;
          flags |= NODE_DUP;
          goto put_sub;
        }
        xdata.iov_len = olddata.iov_len + offset;
      }

      fp_flags = fp->mp_flags16;
      if (NODESIZE + node_get_keysize(leaf) + xdata.iov_len > bk->me_nodemax) {
        /* Too big for a sub-page, convert to sub-tree */
        fp_flags &= ~P_SUBP;
      prep_subDB:;
        /* FIXME: формировать в subordinate */
        dummy.aa_xsize32 = 0;
        dummy.aa_flags16 = 0;
        if (mc->mc_aht->aa.flags16 & MDBX_DUPFIXED) {
          fp_flags |= P_DFL;
          dummy.aa_xsize32 = fp->mp_leaf2_ksize16;
          dummy.aa_flags16 = MDBX_DUPFIXED;
          if (mc->mc_aht->aa.flags16 & MDBX_INTEGERDUP)
            dummy.aa_flags16 |= MDBX_INTEGERKEY;
        }
        dummy.aa_depth16 = 1;
        dummy.aa_branch_pages = 0;
        dummy.aa_leaf_pages = 1;
        dummy.aa_overflow_pages = 0;
        dummy.aa_entries = page_numkeys(fp);
        xdata.iov_len = sizeof(aatree_t);
        xdata.iov_base = &dummy;
        rc = page_alloc(mc, 1, &mp, MDBX_ALLOC_ALL);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        assert(bk->me_psize > olddata.iov_len);
        offset = bk->me_psize - (unsigned)olddata.iov_len;
        flags |= NODE_DUP | NODE_SUBTREE;
        dummy.aa_root = mp->mp_pgno;
        sub_root = mp;
      }
      if (mp != fp) {
        mp->mp_flags16 = fp_flags | P_DIRTY;
        mp->mp_leaf2_ksize16 = fp->mp_leaf2_ksize16;
        mp->mp_lower = fp->mp_lower;
        assert(fp->mp_upper + offset <= UINT16_MAX);
        mp->mp_upper = (indx_t)(fp->mp_upper + offset);
        if (fp_flags & P_DFL) {
          memcpy(page_data(mp), page_data(fp),
                 page_numkeys(fp) * fp->mp_leaf2_ksize16);
        } else {
          memcpy((char *)mp + mp->mp_upper + PAGEHDRSZ,
                 (char *)fp + fp->mp_upper + PAGEHDRSZ,
                 olddata.iov_len - fp->mp_upper - PAGEHDRSZ);
          for (i = 0; i < page_numkeys(fp); i++) {
            assert(fp->mp_ptrs[i] + offset <= UINT16_MAX);
            mp->mp_ptrs[i] = (indx_t)(fp->mp_ptrs[i] + offset);
          }
        }
      }

      rdata = &xdata;
      flags |= NODE_DUP;
      do_sub = true;
      if (!insert_key)
        node_del(mc, 0);
      goto new_sub;
    } else {
      assert(!(mc->mc_aht->aa.flags16 & MDBX_DUPSORT));
    }
  current:
    /* MDBX passes NODE_SUBTREE in 'flags' to write a AA record */
    if (unlikely((leaf->node_flags8 ^ flags) & NODE_SUBTREE))
      return MDBX_INCOMPATIBLE;
    /* overflow page overwrites need special handling */
    if (unlikely(leaf->node_flags8 & NODE_BIG)) {
      int level, ovpages, dpages = OVPAGES(bk, data->iov_len);
      pgno_t pgno = get_pgno_lea16(olddata.iov_base);
      page_t *omp;
      int err = page_get(mc->mc_txn, pgno, &omp, &level);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      ovpages = omp->mp_pages;

      /* Is the ov page large enough? */
      if (ovpages >= dpages) {
        if (!(omp->mp_flags16 & P_DIRTY) &&
            (level || (bk->me_flags32 & MDBX_WRITEMAP))) {
          rc = page_unspill(mc->mc_txn, omp, &omp);
          if (unlikely(rc != MDBX_SUCCESS))
            return rc;
          level = 0; /* dirty in this txn or clean */
        }
        /* Is it dirty? */
        if (omp->mp_flags16 & P_DIRTY) {
          /* yes, overwrite it. Note in this case we don't
           * bother to try shrinking the page if the new data
           * is smaller than the overflow threshold. */
          if (unlikely(level > 1)) {
            /* It is writable only in a parent txn */
            page_t *np = page_malloc(mc->mc_txn, ovpages);
            if (unlikely(!np))
              return MDBX_ENOMEM;
            MDBX_ID2 id2 = {pgno, np};
            /* Note - this page is already counted in parent's dirtyroom */
            err = mdbx_mid2l_insert(mc->mc_txn->mt_rw_dirtylist, &id2);
            assert(err == 0);

            /* Currently we make the page look as with put() in the
             * parent txn, in case the user peeks at MDBX_RESERVEd
             * or unused parts. Some users treat ovpages specially. */
            const size_t whole = pgno2bytes(bk, ovpages);
            /* Skip the part where MDBX will put *data.
             * Copy end of page, adjusting alignment so
             * compiler may copy words instead of bytes. */
            const size_t off =
                (PAGEHDRSZ + data->iov_len) & -(intptr_t)sizeof(size_t);
            memcpy((size_t *)((char *)np + off), (size_t *)((char *)omp + off),
                   whole - off);
            memcpy(np, omp, PAGEHDRSZ); /* Copy header of page */
            omp = np;
          }
          node_set_datasize(leaf, data->iov_len);
          if (F_ISSET(flags, MDBX_IUD_RESERVE))
            data->iov_base = page_data(omp);
          else
            memcpy(page_data(omp), data->iov_base, data->iov_len);
          return MDBX_SUCCESS;
        }
      }
      err = ovpage_free(mc, omp);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    } else if (data->iov_len == olddata.iov_len) {
      assert(EVEN(key->iov_len) == EVEN(leaf->mn_ksize16));
      /* same size, just replace it. Note that we could
       * also reuse this node if the new data is smaller,
       * but instead we opt to shrink the node in that case. */
      if (F_ISSET(flags, MDBX_IUD_RESERVE))
        data->iov_base = olddata.iov_base;
      else if (!(mc->mc_kind8 & S_SUBCURSOR))
        memcpy(olddata.iov_base, data->iov_base, data->iov_len);
      else {
        assert(page_numkeys(mc->mc_pg[mc->mc_top]) == 1);
        assert(mc->mc_pg[mc->mc_top]->mp_upper ==
               mc->mc_pg[mc->mc_top]->mp_lower);
        assert(IS_LEAF(mc->mc_pg[mc->mc_top]) &&
               !IS_DFL(mc->mc_pg[mc->mc_top]));
        assert(node_get_datasize(leaf) == 0);
        assert(leaf->node_flags8 == 0);
        assert(key->iov_len < UINT16_MAX);
        leaf->mn_ksize16 = (uint16_t)key->iov_len;
        memcpy(NODEKEY(leaf), key->iov_base, key->iov_len);
        assert((char *)NODEDATA(leaf) + node_get_datasize(leaf) <
               (char *)(mc->mc_pg[mc->mc_top]) + bk->me_psize);
        goto fix_parent;
      }
      return MDBX_SUCCESS;
    }
    node_del(mc, 0);
  }

  rdata = data;

new_sub:
  nsize =
      IS_DFL(mc->mc_pg[mc->mc_top]) ? key->iov_len : leaf_size(bk, key, rdata);
  nflags = flags & NODE_ADD_FLAGS;
  if (page_spaceleft(mc->mc_pg[mc->mc_top]) < nsize) {
    if ((flags & (NODE_DUP | NODE_SUBTREE)) == NODE_DUP)
      nflags &= ~MDBX_IUD_APPEND; /* sub-page may need room to grow */
    if (!insert_key)
      nflags |= MDBX_SPLIT_REPLACE;
    rc = page_split(mc, key, rdata, P_INVALID, nflags);
  } else {
    /* There is room already in this leaf page. */
    rc = node_add(mc, mc->mc_ki[mc->mc_top], key, rdata, 0, nflags);
    if (likely(rc == MDBX_SUCCESS)) {
      /* Adjust other cursors pointing to mp */
      const unsigned top = mc->mc_top;
      page_t *const page = mc->mc_pg[top];
      assert((mc->mc_kind8 & (S_SUBCURSOR | S_HAVESUB)) == S_HAVESUB);
      for (MDBX_cursor *scan = *cursor_listhead(cursor_bundle(mc)); scan;
           scan = scan->mc_next) {
        if (&scan->primal == mc)
          continue;
        if (top >=
            scan->primal.mc_snum /* scan->primal.mc_snum < mc->mc_snum */)
          continue;
        if (!(scan->primal.mc_state8 & C_INITIALIZED))
          continue;
        if (scan->primal.mc_pg[top] != page)
          continue;
        if (insert_key && scan->primal.mc_ki[top] >= mc->mc_ki[top])
          scan->primal.mc_ki[top]++;

        assert(IS_LEAF(page));
        XCURSOR_REFRESH(scan, top, page);
      }
    }
  }

  if (likely(rc == MDBX_SUCCESS)) {
    /* Now store the actual data in the child AA. Note that we're
     * storing the user data in the keys field, so there are strict
     * size limits on dupdata. The actual data fields of the child
     * AA are all zero size. */
    if (do_sub) {
      int xflags;
    put_sub:
      xdata.iov_len = 0;
      xdata.iov_base = "";
      node_t *leaf = node_ptr(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
      if (flags & MDBX_IUD_CURRENT) {
        xflags =
            (flags & MDBX_IUD_NODUP)
                ? MDBX_IUD_CURRENT | MDBX_IUD_NOOVERWRITE | MDBX_IUD_NOSPILL
                : MDBX_IUD_CURRENT | MDBX_IUD_NOSPILL;
      } else {
        subordinate_setup(mc, leaf);
        xflags = (flags & MDBX_IUD_NODUP)
                     ? MDBX_IUD_NOOVERWRITE | MDBX_IUD_NOSPILL
                     : MDBX_IUD_NOSPILL;
      }
      if (sub_root)
        subordinate->mc_pg[0] = sub_root;
      /* converted, write the original data first */
      if (dupdata_flag) {
        rc = cursor_put(subordinate, &dkey, &xdata, xflags);
        if (unlikely(rc != MDBX_SUCCESS))
          goto bad_sub;
        /* we've done our job */
        dkey.iov_len = 0;
      }
      if (!(leaf->node_flags8 & NODE_SUBTREE) || sub_root) {
        /* Adjust other cursors pointing to mp */
        const unsigned top = mc->mc_top;
        page_t *const page = mc->mc_pg[top];
        assert((mc->mc_kind8 & (S_SUBCURSOR | S_HAVESUB)) == S_HAVESUB);
        for (MDBX_cursor *scan = *cursor_listhead(cursor_bundle(mc)); scan;
             scan = scan->mc_next) {
          if (&scan->primal == mc)
            continue;
          if (top >=
              scan->primal.mc_snum /* scan->primal.mc_snum < mc->mc_snum */)
            continue;
          if (!(scan->primal.mc_state8 & C_INITIALIZED))
            continue;
          if (scan->primal.mc_pg[top] != page)
            continue;

          if (scan->primal.mc_ki[top] == mc->mc_ki[top]) {
            subcursor_fixup(scan, subordinate, dupdata_flag);
          } else if (!insert_key) {
            XCURSOR_REFRESH(scan, top, page);
          }
        }
      }
      assert(subordinate_subcursor(subordinate)->mx_aht_body.aa.entries <
             SIZE_MAX);
      size_t entries_before_put =
          (size_t)subordinate_subcursor(subordinate)->mx_aht_body.aa.entries;
      if (flags & MDBX_IUD_APPENDDUP)
        xflags |= MDBX_IUD_APPEND;
      rc = cursor_put(subordinate, data, &xdata, xflags);
      if (flags & NODE_SUBTREE)
        aa_txn2db(&subordinate_subcursor(subordinate)->mx_aht_body,
                  (aatree_t *)NODEDATA(leaf));
      insert_data =
          (entries_before_put !=
           (size_t)subordinate_subcursor(subordinate)->mx_aht_body.aa.entries);
    }
    /* Increment count unless we just replaced an existing item. */
    if (insert_data)
      mc->mc_aht->aa.entries++;
    if (insert_key) {
      /* Invalidate txn if we created an empty sub-AA */
      if (unlikely(rc != MDBX_SUCCESS))
        goto bad_sub;
      /* If we succeeded and the key didn't exist before,
       * make sure the cursor is marked valid. */
      mc->mc_state8 |= C_INITIALIZED;
    }
    if (flags & MDBX_IUD_MULTIPLE) {
      if (!rc) {
        mcount++;
        /* let caller know how many succeeded, if any */
        data[1].iov_len = mcount;
        if (mcount < dcount) {
          data[0].iov_base = (char *)data[0].iov_base + data[0].iov_len;
          insert_key = insert_data = false;
          goto more;
        }
      }
    }
    return rc;
  bad_sub:
    if (unlikely(rc == MDBX_KEYEXIST))
      mdbx_error("unexpected %s", "MDBX_KEYEXIST");
    /* should not happen, we deleted that item */
    rc = MDBX_PROBLEM;
  }
  mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

static int cursor_delete(cursor_t *mc, unsigned flags) {
  if (unlikely(!(mc->mc_state8 & C_INITIALIZED)))
    return MDBX_EINVAL;

  if (unlikely(mc->mc_ki[mc->mc_top] >= page_numkeys(mc->mc_pg[mc->mc_top])))
    return MDBX_NOTFOUND;

  if (likely((flags & MDBX_IUD_NOSPILL) == 0)) {
    int rc = page_spill(mc, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  int rc = cursor_touch(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  page_t *page = mc->mc_pg[mc->mc_top];
  if (IS_DFL(page))
    goto del_key;

  node_t *leaf = node_ptr(page, mc->mc_ki[mc->mc_top]);
  if (F_ISSET(leaf->node_flags8, NODE_DUP)) {
    subcursor_t *const subcursor = cursor_subcursor(mc);
    if (flags & MDBX_IUD_NODUP) {
      /* mdbx_cr_del0() will subtract the final entry */
      mc->mc_aht->aa.entries -= subcursor->mx_aht_body.aa.entries - 1;
      subcursor->mx_cursor.mc_state8 &= ~C_INITIALIZED;
    } else {
      if (!F_ISSET(leaf->node_flags8, NODE_SUBTREE))
        subcursor->mx_cursor.mc_pg[0] = NODEDATA(leaf);

      rc = cursor_delete(&subcursor->mx_cursor, MDBX_IUD_NOSPILL);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;

      /* If sub-tree still has entries, we're done */
      if (likely(subcursor->mx_aht_body.aa.entries)) {
        if (likely(leaf->node_flags8 & NODE_SUBTREE)) {
          /* update sub-tree info */
          aa_txn2db(&subcursor->mx_aht_body, (aatree_t *)NODEDATA(leaf));
        } else {
          /* shrink fake page */
          node_shrink(page, mc->mc_ki[mc->mc_top]);
          leaf = node_ptr(page, mc->mc_ki[mc->mc_top]);
          subcursor->mx_cursor.mc_pg[0] = NODEDATA(leaf);
          /* fix other sub-tree cursors pointed at fake pages on this page */
          const unsigned top = mc->mc_top;
          page_t *const page = mc->mc_pg[top];
          assert((mc->mc_kind8 & (S_SUBCURSOR | S_HAVESUB)) == S_HAVESUB);
          for (MDBX_cursor *scan = *cursor_listhead(cursor_bundle(mc)); scan;
               scan = scan->mc_next) {
            if (&scan->subordinate == subcursor)
              continue;
            if (top >=
                scan->primal.mc_snum /* scan->primal.mc_snum < mc->mc_snum */)
              continue;
            if (!(scan->primal.mc_state8 & C_INITIALIZED))
              continue;
            if (scan->primal.mc_pg[top] != page)
              continue;
            XCURSOR_REFRESH(scan, top, page);
          }
        }
        mc->mc_aht->aa.entries--;
        return rc;
      } else {
        subcursor->mx_cursor.mc_state8 &= ~C_INITIALIZED;
      }
      /* otherwise fall thru and delete the sub-tree */
    }

    if (leaf->node_flags8 & NODE_SUBTREE) {
      /* add all the child AA's pages to the free list */
      rc = tree_drop(&subcursor->mx_cursor, 0);
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
    }
  }
  /* MDBX passes NODE_SUBTREE in 'flags' to delete a AA record */
  else if (unlikely((leaf->node_flags8 ^ flags) & NODE_SUBTREE)) {
    rc = MDBX_INCOMPATIBLE;
    goto fail;
  }

  /* add overflow pages to free list */
  if (unlikely(leaf->node_flags8 & NODE_BIG)) {
    page_t *omp;
    rc = page_get(mc->mc_txn, get_pgno_lea16(NODEDATA(leaf)), &omp, nullptr);
    if (unlikely((rc != MDBX_SUCCESS)))
      goto fail;
    rc = ovpage_free(mc, omp);
    if (unlikely((rc != MDBX_SUCCESS)))
      goto fail;
  }

del_key:
  return mdbx_cr_del0(mc);

fail:
  mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

/* Allocate and initialize new pages for a database.
 * Set MDBX_TXN_ERROR on failure.
 *
 * [in] mc a  cursor on the database being added to.
 * [in] flags flags defining what type of page is being allocated.
 * [in] num   the number of pages to allocate. This is usually 1,
 *            unless allocating overflow pages for a large record.
 * [out] mp   Address of a page, or nullptr on failure.
 *
 * Returns 0 on success, non-zero on failure. */
static int page_new(cursor_t *mc, unsigned flags, unsigned num, page_t **mp) {
  page_t *np;

  int rc = page_alloc(mc, num, &np, MDBX_ALLOC_ALL);
  if (unlikely((rc != MDBX_SUCCESS)))
    return rc;

  mdbx_debug("allocated new page #%" PRIaPGNO ", size %u", np->mp_pgno,
             mc->mc_txn->mt_book->me_psize);
  np->mp_flags16 = (uint16_t)(flags | P_DIRTY);
  np->mp_lower = 0;
  np->mp_upper = (indx_t)(mc->mc_txn->mt_book->me_psize - PAGEHDRSZ);

  if (IS_BRANCH(np))
    mc->mc_aht->aa.branch_pages++;
  else if (IS_LEAF(np))
    mc->mc_aht->aa.leaf_pages++;
  else if (IS_OVERFLOW(np)) {
    mc->mc_aht->aa.overflow_pages += num;
    np->mp_pages = num;
  }
  *mp = np;

  return MDBX_SUCCESS;
}

/* Calculate the size of a leaf node.
 *
 * The size depends on the databook's page size; if a data item
 * is too large it will be put onto an overflow page and the node
 * size will only include the key and not the data. Sizes are always
 * rounded up to an even number of bytes, to guarantee 2-byte alignment
 * of the node_t headers.
 *
 * [in] bk   The databook handle.
 * [in] key   The key for the node.
 * [in] data  The data for the node.
 *
 * Returns The number of bytes needed to store the node. */
static inline size_t leaf_size(MDBX_milieu *bk, MDBX_iov *key, MDBX_iov *data) {
  size_t sz;

  sz = LEAFSIZE(key, data);
  if (sz > bk->me_nodemax) {
    /* put on overflow page */
    sz -= data->iov_len - sizeof(pgno_t);
  }

  return EVEN(sz + sizeof(indx_t));
}

/* Calculate the size of a branch node.
 *
 * The size should depend on the databook's page size but since
 * we currently don't support spilling large keys onto overflow
 * pages, it's simply the size of the node_t header plus the
 * size of the key. Sizes are always rounded up to an even number
 * of bytes, to guarantee 2-byte alignment of the node_t headers.
 *
 * [in] bk The databook handle.
 * [in] key The key for the node.
 *
 * Returns The number of bytes needed to store the node. */
static inline size_t branch_size(MDBX_milieu *bk, MDBX_iov *key) {
  size_t sz;

  sz = INDXSIZE(key);
  if (unlikely(sz > bk->me_nodemax)) {
    /* put on overflow page */
    /* not implemented */
    mdbx_assert_fail(bk, "INDXSIZE(key) <= bk->me_nodemax", __FUNCTION__,
                     __LINE__);
    sz -= key->iov_len - sizeof(pgno_t);
  }

  return sz + sizeof(indx_t);
}

/* Add a node to the page pointed to by the cursor.
 * Set MDBX_TXN_ERROR on failure.
 *
 * [in] mc    The cursor for this operation.
 * [in] indx  The index on the page where the new node should be added.
 * [in] key   The key for the new node.
 * [in] data  The data for the new node, if any.
 * [in] pgno  The page number, if adding a branch node.
 * [in] flags Flags for the node.
 *
 * Returns 0 on success, non-zero on failure. Possible errors are:
 *
 * MDBX_ENOMEM    - failed to allocate overflow pages for the node.
 * MDBX_PAGE_FULL  - there is insufficient room in the page. This error
 *                  should never happen since all callers already calculate
 *                  the page's free space before calling this function. */
static int node_add(cursor_t *mc, unsigned indx, MDBX_iov *key, MDBX_iov *data,
                    pgno_t pgno, unsigned flags) {
  unsigned i;
  size_t node_size = NODESIZE;
  intptr_t room;
  node_t *node;
  page_t *mp = mc->mc_pg[mc->mc_top];
  page_t *ofp = nullptr; /* overflow page */
  void *ndata;
  DKBUF;

  assert(mp->mp_upper >= mp->mp_lower);

  mdbx_debug("add to %s %spage %" PRIaPGNO " index %i, data size %" PRIuPTR
             " key size %" PRIuPTR " [%s]",
             IS_LEAF(mp) ? "leaf" : "branch", IS_SUBP(mp) ? "sub-" : "",
             mp->mp_pgno, indx, data ? data->iov_len : 0,
             key ? key->iov_len : 0, DKEY(key));

  if (IS_DFL(mp)) {
    assert(key);
    /* Move higher keys up one slot. */
    const int keysize = mc->mc_aht->aa.xsize32;
    char *const ptr = DFLKEY(mp, indx, keysize);
    const int diff = page_numkeys(mp) - indx;
    if (diff > 0)
      memmove(ptr + keysize, ptr, diff * keysize);
    /* insert new key */
    memcpy(ptr, key->iov_base, keysize);

    /* Just using these for counting */
    assert(UINT16_MAX - mp->mp_lower >= (int)sizeof(indx_t));
    mp->mp_lower += sizeof(indx_t);
    assert(mp->mp_upper >= keysize - sizeof(indx_t));
    mp->mp_upper -= (indx_t)(keysize - sizeof(indx_t));
    return MDBX_SUCCESS;
  }

  room = (intptr_t)page_spaceleft(mp) - (intptr_t)sizeof(indx_t);
  if (key != nullptr)
    node_size += key->iov_len;
  if (IS_LEAF(mp)) {
    assert(key && data);
    if (unlikely(flags & NODE_BIG)) {
      /* Data already on overflow page. */
      node_size += sizeof(pgno_t);
    } else if (unlikely(node_size + data->iov_len >
                        mc->mc_txn->mt_book->me_nodemax)) {
      pgno_t ovpages = OVPAGES(mc->mc_txn->mt_book, data->iov_len);
      int rc;
      /* Put data on overflow page. */
      mdbx_debug("data size is %" PRIuPTR ", node would be %" PRIuPTR
                 ", put data on overflow page",
                 data->iov_len, node_size + data->iov_len);
      node_size = EVEN(node_size + sizeof(pgno_t));
      if ((intptr_t)node_size > room)
        goto full;
      rc = page_new(mc, P_OVERFLOW, ovpages, &ofp);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      mdbx_debug("allocated overflow page %" PRIaPGNO "", ofp->mp_pgno);
      flags |= NODE_BIG;
      goto update;
    } else {
      node_size += data->iov_len;
    }
  }
  node_size = EVEN(node_size);
  if (unlikely((intptr_t)node_size > room))
    goto full;

update:
  /* Move higher pointers up one slot. */
  for (i = page_numkeys(mp); i > indx; i--)
    mp->mp_ptrs[i] = mp->mp_ptrs[i - 1];

  /* Adjust free space offsets. */
  size_t ofs = mp->mp_upper - node_size;
  assert(ofs >= mp->mp_lower + sizeof(indx_t));
  assert(ofs <= UINT16_MAX);
  mp->mp_ptrs[indx] = (uint16_t)ofs;
  mp->mp_upper = (uint16_t)ofs;
  mp->mp_lower += sizeof(indx_t);

  /* Write the node data. */
  node = node_ptr(mp, indx);
  node->mn_ksize16 = (key == nullptr) ? 0 : (uint16_t)key->iov_len;
  node->node_flags8 = (uint16_t)flags;
  if (IS_LEAF(mp))
    node_set_datasize(node, data->iov_len);
  else
    node_set_pgno(node, pgno);

  if (key)
    memcpy(NODEKEY(node), key->iov_base, key->iov_len);

  if (IS_LEAF(mp)) {
    ndata = NODEDATA(node);
    if (unlikely(ofp == nullptr)) {
      if (unlikely(flags & NODE_BIG))
        memcpy(ndata, data->iov_base, sizeof(pgno_t));
      else if (F_ISSET(flags, MDBX_IUD_RESERVE))
        data->iov_base = ndata;
      else if (likely(ndata != data->iov_base))
        memcpy(ndata, data->iov_base, data->iov_len);
    } else {
      memcpy(ndata, &ofp->mp_pgno, sizeof(pgno_t));
      ndata = page_data(ofp);
      if (F_ISSET(flags, MDBX_IUD_RESERVE))
        data->iov_base = ndata;
      else if (likely(ndata != data->iov_base))
        memcpy(ndata, data->iov_base, data->iov_len);
    }
  }

  return MDBX_SUCCESS;

full:
  mdbx_debug("not enough room in page %" PRIaPGNO ", got %u ptrs", mp->mp_pgno,
             page_numkeys(mp));
  mdbx_debug("upper-lower = %u - %u = %" PRIiPTR "", mp->mp_upper, mp->mp_lower,
             room);
  mdbx_debug("node size = %" PRIuPTR "", node_size);
  mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return MDBX_PAGE_FULL;
}

/* Move a node from csrc to cdst. */
static int node_move(cursor_t *csrc, cursor_t *cdst, int fromleft) {
  node_t *srcnode;
  MDBX_iov key, data;
  pgno_t srcpg;
  int rc;
  unsigned flags;

  DKBUF;

  /* Mark src and dst as dirty. */
  if (unlikely((rc = page_touch(csrc)) || (rc = page_touch(cdst))))
    return rc;

  if (IS_DFL(csrc->mc_pg[csrc->mc_top])) {
    key.iov_len = csrc->mc_aht->aa.xsize32;
    key.iov_base = DFLKEY(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top],
                          key.iov_len);
    data.iov_len = 0;
    data.iov_base = nullptr;
    srcpg = 0;
    flags = 0;
  } else {
    srcnode = node_ptr(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top]);
    assert(!((size_t)srcnode & 1));
    srcpg = node_get_pgno(srcnode);
    flags = srcnode->node_flags8;
    if (csrc->mc_ki[csrc->mc_top] == 0 &&
        IS_BRANCH(csrc->mc_pg[csrc->mc_top])) {
      unsigned snum = csrc->mc_snum;
      node_t *s2;
      /* must find the lowest key below src */
      rc = page_search_lowest(csrc);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      if (IS_DFL(csrc->mc_pg[csrc->mc_top])) {
        key.iov_len = csrc->mc_aht->aa.xsize32;
        key.iov_base = DFLKEY(csrc->mc_pg[csrc->mc_top], 0, key.iov_len);
      } else {
        s2 = node_ptr(csrc->mc_pg[csrc->mc_top], 0);
        key.iov_len = node_get_keysize(s2);
        key.iov_base = NODEKEY(s2);
      }
      assert(snum >= 1 && snum <= UINT16_MAX);
      csrc->mc_snum = (uint16_t)snum--;
      csrc->mc_top = (uint16_t)snum;
    } else {
      key.iov_len = node_get_keysize(srcnode);
      key.iov_base = NODEKEY(srcnode);
    }
    data.iov_len = node_get_datasize(srcnode);
    data.iov_base = NODEDATA(srcnode);
  }

  MDBX_cursor mn;
  mn.subordinate = nullptr;
  if (IS_BRANCH(cdst->mc_pg[cdst->mc_top]) && cdst->mc_ki[cdst->mc_top] == 0) {
    unsigned snum = cdst->mc_snum;
    node_t *s2;
    MDBX_iov bkey;
    /* must find the lowest key below dst */
    cursor_copy(cdst, &mn);
    rc = page_search_lowest(&mn);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    if (IS_DFL(mn.mc_pg[mn.mc_top])) {
      bkey.iov_len = mn.mc_aht->aa.xsize32;
      bkey.iov_base = DFLKEY(mn.mc_pg[mn.mc_top], 0, bkey.iov_len);
    } else {
      s2 = node_ptr(mn.mc_pg[mn.mc_top], 0);
      bkey.iov_len = node_get_keysize(s2);
      bkey.iov_base = NODEKEY(s2);
    }
    assert(snum >= 1 && snum <= UINT16_MAX);
    mn.mc_snum = (uint16_t)snum--;
    mn.mc_top = (uint16_t)snum;
    mn.mc_ki[snum] = 0;
    rc = update_key(&mn, &bkey);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  mdbx_debug("moving %s node %u [%s] on page %" PRIaPGNO
             " to node %u on page %" PRIaPGNO "",
             IS_LEAF(csrc->mc_pg[csrc->mc_top]) ? "leaf" : "branch",
             csrc->mc_ki[csrc->mc_top], DKEY(&key),
             csrc->mc_pg[csrc->mc_top]->mp_pgno, cdst->mc_ki[cdst->mc_top],
             cdst->mc_pg[cdst->mc_top]->mp_pgno);

  /* Add the node to the destination page. */
  rc = node_add(cdst, cdst->mc_ki[cdst->mc_top], &key, &data, srcpg, flags);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Delete the node from the source page. */
  node_del(csrc, key.iov_len);

  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    MDBX_aah aah = csrc->mc_ah;
    page_t *mpd, *mps;

    mps = csrc->mc_pg[csrc->mc_top];
    /* If we're adding on the left, bump others up */
    if (fromleft) {
      mpd = cdst->mc_pg[csrc->mc_top];
      for (m2 = csrc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
        if (csrc->mc_state8 & C_SUBCURSOR)
          m3 = &m2->subordinate.mx_cursor;
        else
          m3 = m2;
        if (!(m3->mc_state8 & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
          continue;
        if (m3 != cdst && m3->mc_pg[csrc->mc_top] == mpd &&
            m3->mc_ki[csrc->mc_top] >= cdst->mc_ki[csrc->mc_top]) {
          m3->mc_ki[csrc->mc_top]++;
        }
        if (m3 != csrc && m3->mc_pg[csrc->mc_top] == mps &&
            m3->mc_ki[csrc->mc_top] == csrc->mc_ki[csrc->mc_top]) {
          m3->mc_pg[csrc->mc_top] = cdst->mc_pg[cdst->mc_top];
          m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
          m3->mc_ki[csrc->mc_top - 1]++;
        }
        if (IS_LEAF(mps))
          XCURSOR_REFRESH(m3, csrc->mc_top, m3->mc_pg[csrc->mc_top]);
      }
    } else
    /* Adding on the right, bump others down */
    {
      for (m2 = csrc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
        if (csrc->mc_state8 & C_SUBCURSOR)
          m3 = &m2->subordinate.mx_cursor;
        else
          m3 = m2;
        if (m3 == csrc)
          continue;
        if (!(m3->mc_state8 & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
          continue;
        if (m3->mc_pg[csrc->mc_top] == mps) {
          if (!m3->mc_ki[csrc->mc_top]) {
            m3->mc_pg[csrc->mc_top] = cdst->mc_pg[cdst->mc_top];
            m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
            m3->mc_ki[csrc->mc_top - 1]--;
          } else {
            m3->mc_ki[csrc->mc_top]--;
          }
          if (IS_LEAF(mps))
            XCURSOR_REFRESH(m3, csrc->mc_top, m3->mc_pg[csrc->mc_top]);
        }
      }
    }
  }

  /* Update the parent separators. */
  if (csrc->mc_ki[csrc->mc_top] == 0) {
    if (csrc->mc_ki[csrc->mc_top - 1] != 0) {
      if (IS_DFL(csrc->mc_pg[csrc->mc_top])) {
        key.iov_base = DFLKEY(csrc->mc_pg[csrc->mc_top], 0, key.iov_len);
      } else {
        srcnode = node_ptr(csrc->mc_pg[csrc->mc_top], 0);
        key.iov_len = node_get_keysize(srcnode);
        key.iov_base = NODEKEY(srcnode);
      }
      mdbx_debug("update separator for source page %" PRIaPGNO " to [%s]",
                 csrc->mc_pg[csrc->mc_top]->mp_pgno, DKEY(&key));
      cursor_copy(csrc, &mn);
      mn.mc_snum--;
      mn.mc_top--;
      /* We want mdbx_rebalance to find mn when doing fixups */
      WITH_CURSOR_TRACKING(mn, rc = update_key(&mn, &key));
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    if (IS_BRANCH(csrc->mc_pg[csrc->mc_top])) {
      MDBX_iov nullkey;
      indx_t ix = csrc->mc_ki[csrc->mc_top];
      nullkey.iov_len = 0;
      csrc->mc_ki[csrc->mc_top] = 0;
      rc = update_key(csrc, &nullkey);
      csrc->mc_ki[csrc->mc_top] = ix;
      assert(rc == MDBX_SUCCESS);
    }
  }

  if (cdst->mc_ki[cdst->mc_top] == 0) {
    if (cdst->mc_ki[cdst->mc_top - 1] != 0) {
      if (IS_DFL(csrc->mc_pg[csrc->mc_top])) {
        key.iov_base = DFLKEY(cdst->mc_pg[cdst->mc_top], 0, key.iov_len);
      } else {
        srcnode = node_ptr(cdst->mc_pg[cdst->mc_top], 0);
        key.iov_len = node_get_keysize(srcnode);
        key.iov_base = NODEKEY(srcnode);
      }
      mdbx_debug("update separator for destination page %" PRIaPGNO " to [%s]",
                 cdst->mc_pg[cdst->mc_top]->mp_pgno, DKEY(&key));
      cursor_copy(cdst, &mn);
      mn.mc_snum--;
      mn.mc_top--;
      /* We want mdbx_rebalance to find mn when doing fixups */
      WITH_CURSOR_TRACKING(mn, rc = update_key(&mn, &key));
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    if (IS_BRANCH(cdst->mc_pg[cdst->mc_top])) {
      MDBX_iov nullkey;
      indx_t ix = cdst->mc_ki[cdst->mc_top];
      nullkey.iov_len = 0;
      cdst->mc_ki[cdst->mc_top] = 0;
      rc = update_key(cdst, &nullkey);
      cdst->mc_ki[cdst->mc_top] = ix;
      assert(rc == MDBX_SUCCESS);
    }
  }

  return MDBX_SUCCESS;
}

/* Merge one page into another.
 *
 * The nodes from the page pointed to by csrc will be copied to the page
 * pointed to by cdst and then the csrc page will be freed.
 *
 * [in] csrc Cursor pointing to the source page.
 * [in] cdst Cursor pointing to the destination page.
 *
 * Returns 0 on success, non-zero on failure. */
static int page_merge(MDBX_cursor *csrc, MDBX_cursor *cdst) {
  page_t *psrc, *pdst;
  node_t *srcnode;
  MDBX_iov key, data;
  unsigned nkeys;
  int rc;
  unsigned i, j;

  psrc = csrc->mc_pg[csrc->mc_top];
  pdst = cdst->mc_pg[cdst->mc_top];

  mdbx_debug("merging page %" PRIaPGNO " into %" PRIaPGNO "", psrc->mp_pgno,
             pdst->mp_pgno);

  assert(csrc->mc_snum > 1); /* can't merge root page */
  assert(cdst->mc_snum > 1);

  /* Mark dst as dirty. */
  if (unlikely(rc = page_touch(cdst)))
    return rc;

  /* get dst page again now that we've touched it. */
  pdst = cdst->mc_pg[cdst->mc_top];

  /* Move all nodes from src to dst. */
  j = nkeys = page_numkeys(pdst);
  if (IS_DFL(psrc)) {
    key.iov_len = csrc->mc_aht->aa.xsize32;
    key.iov_base = page_data(psrc);
    for (i = 0; i < page_numkeys(psrc); i++, j++) {
      rc = node_add(cdst, j, &key, nullptr, 0, 0);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      key.iov_base = (char *)key.iov_base + key.iov_len;
    }
  } else {
    for (i = 0; i < page_numkeys(psrc); i++, j++) {
      srcnode = node_ptr(psrc, i);
      if (i == 0 && IS_BRANCH(psrc)) {
        MDBX_cursor mn;
        node_t *s2;
        cursor_copy(csrc, &mn);
        mn.subordinate = nullptr;
        /* must find the lowest key below src */
        rc = page_search_lowest(&mn);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        if (IS_DFL(mn.mc_pg[mn.mc_top])) {
          key.iov_len = mn.mc_aht->aa.xsize32;
          key.iov_base = DFLKEY(mn.mc_pg[mn.mc_top], 0, key.iov_len);
        } else {
          s2 = node_ptr(mn.mc_pg[mn.mc_top], 0);
          key.iov_len = node_get_keysize(s2);
          key.iov_base = NODEKEY(s2);
        }
      } else {
        key.iov_len = srcnode->mn_ksize16;
        key.iov_base = NODEKEY(srcnode);
      }

      data.iov_len = node_get_datasize(srcnode);
      data.iov_base = NODEDATA(srcnode);
      rc = node_add(cdst, j, &key, &data, node_get_pgno(srcnode),
                    srcnode->node_flags8);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
  }

  mdbx_debug("dst page %" PRIaPGNO " now has %u keys (%.1f%% filled)",
             pdst->mp_pgno, page_numkeys(pdst),
             (float)PAGEFILL(cdst->mc_txn->mt_book, pdst) / 10);

  /* Unlink the src page from parent and add to free list. */
  csrc->mc_top--;
  node_del(csrc, 0);
  if (csrc->mc_ki[csrc->mc_top] == 0) {
    key.iov_len = 0;
    rc = update_key(csrc, &key);
    if (unlikely(rc != MDBX_SUCCESS)) {
      csrc->mc_top++;
      return rc;
    }
  }
  csrc->mc_top++;

  psrc = csrc->mc_pg[csrc->mc_top];
  /* If not operating on GACO, allow this page to be reused
   * in this txn. Otherwise just add to free list. */
  rc = page_loose(csrc, psrc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if (IS_LEAF(psrc))
    csrc->mc_aht->aa.leaf_pages--;
  else
    csrc->mc_aht->aa.branch_pages--;
  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    MDBX_aah aah = csrc->mc_ah;
    unsigned top = csrc->mc_top;

    for (m2 = csrc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
      if (csrc->mc_state8 & C_SUBCURSOR)
        m3 = &m2->subordinate.mx_cursor;
      else
        m3 = m2;
      if (m3 == csrc)
        continue;
      if (m3->mc_snum < csrc->mc_snum)
        continue;
      if (m3->mc_pg[top] == psrc) {
        m3->mc_pg[top] = pdst;
        assert(nkeys + m3->mc_ki[top] <= UINT16_MAX);
        m3->mc_ki[top] += (indx_t)nkeys;
        m3->mc_ki[top - 1] = cdst->mc_ki[top - 1];
      } else if (m3->mc_pg[top - 1] == csrc->mc_pg[top - 1] &&
                 m3->mc_ki[top - 1] > csrc->mc_ki[top - 1]) {
        m3->mc_ki[top - 1]--;
      }
      if (IS_LEAF(psrc))
        XCURSOR_REFRESH(m3, top, m3->mc_pg[top]);
    }
  }
  {
    unsigned snum = cdst->mc_snum;
    uint16_t depth = cdst->mc_aht->aa.depth16;
    cursor_pop(cdst);
    rc = tree_rebalance(cdst);
    /* Did the tree height change? */
    if (depth != cdst->mc_aht->aa.depth16)
      snum += cdst->mc_aht->aa.depth16 - depth;
    assert(snum >= 1 && snum <= UINT16_MAX);
    cdst->mc_snum = (uint16_t)snum;
    cdst->mc_top = (uint16_t)(snum - 1);
  }
  return rc;
}

/* Rebalance the tree after a delete operation.
 * [in] mc Cursor pointing to the page where rebalancing should begin.
 * Returns 0 on success, non-zero on failure. */
static int tree_rebalance(MDBX_cursor *mc) {
  node_t *node;
  int rc, fromleft;
  unsigned ptop, minkeys, thresh;
  MDBX_cursor mn;
  indx_t oldki;

  if (IS_BRANCH(mc->mc_pg[mc->mc_top])) {
    minkeys = 2;
    thresh = 1;
  } else {
    minkeys = 1;
    thresh = FILL_THRESHOLD;
  }
  mdbx_debug("rebalancing %s page %" PRIaPGNO " (has %u keys, %.1f%% full)",
             IS_LEAF(mc->mc_pg[mc->mc_top]) ? "leaf" : "branch",
             mc->mc_pg[mc->mc_top]->mp_pgno,
             page_numkeys(mc->mc_pg[mc->mc_top]),
             (float)PAGEFILL(mc->mc_txn->mt_book, mc->mc_pg[mc->mc_top]) / 10);

  if (PAGEFILL(mc->mc_txn->mt_book, mc->mc_pg[mc->mc_top]) >= thresh &&
      page_numkeys(mc->mc_pg[mc->mc_top]) >= minkeys) {
    mdbx_debug("no need to rebalance page %" PRIaPGNO ", above fill threshold",
               mc->mc_pg[mc->mc_top]->mp_pgno);
    return MDBX_SUCCESS;
  }

  if (mc->mc_snum < 2) {
    page_t *mp = mc->mc_pg[0];
    unsigned nkeys = page_numkeys(mp);
    if (IS_SUBP(mp)) {
      mdbx_debug("Can't rebalance a subpage, ignoring");
      return MDBX_SUCCESS;
    }
    if (nkeys == 0) {
      mdbx_debug("tree is completely empty");
      mc->mc_aht->aa.root = P_INVALID;
      mc->mc_aht->aa.depth16 = 0;
      mc->mc_aht->aa.leaf_pages = 0;
      rc = mdbx_pnl_append(&mc->mc_txn->mt_befree_pages, mp->mp_pgno);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      /* Adjust cursors pointing to mp */
      mc->mc_snum = 0;
      mc->mc_top = 0;
      mc->mc_state8 &= ~C_INITIALIZED;
      {
        MDBX_cursor *m2, *m3;
        MDBX_aah aah = mc->mc_ah;

        for (m2 = mc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
          if (mc->mc_state8 & C_SUBCURSOR)
            m3 = &m2->subordinate.mx_cursor;
          else
            m3 = m2;
          if (!(m3->mc_state8 & C_INITIALIZED) || (m3->mc_snum < mc->mc_snum))
            continue;
          if (m3->mc_pg[0] == mp) {
            m3->mc_snum = 0;
            m3->mc_top = 0;
            m3->mc_state8 &= ~C_INITIALIZED;
          }
        }
      }
    } else if (IS_BRANCH(mp) && page_numkeys(mp) == 1) {
      int i;
      mdbx_debug("collapsing root page!");
      rc = mdbx_pnl_append(&mc->mc_txn->mt_befree_pages, mp->mp_pgno);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      mc->mc_aht->aa.root = node_get_pgno(node_ptr(mp, 0));
      rc = page_get(mc->mc_txn, mc->mc_aht->aa.root, &mc->mc_pg[0], nullptr);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      mc->mc_aht->aa.depth16--;
      mc->mc_aht->aa.branch_pages--;
      mc->mc_ki[0] = mc->mc_ki[1];
      for (i = 1; i < mc->mc_aht->aa.depth16; i++) {
        mc->mc_pg[i] = mc->mc_pg[i + 1];
        mc->mc_ki[i] = mc->mc_ki[i + 1];
      }
      {
        /* Adjust other cursors pointing to mp */
        MDBX_cursor *m2, *m3;
        MDBX_aah aah = mc->mc_ah;

        for (m2 = mc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
          if (mc->mc_state8 & C_SUBCURSOR)
            m3 = &m2->subordinate.mx_cursor;
          else
            m3 = m2;
          if (m3 == mc)
            continue;
          if (!(m3->mc_state8 & C_INITIALIZED))
            continue;
          if (m3->mc_pg[0] == mp) {
            for (i = 0; i < mc->mc_aht->aa.depth16; i++) {
              m3->mc_pg[i] = m3->mc_pg[i + 1];
              m3->mc_ki[i] = m3->mc_ki[i + 1];
            }
            m3->mc_snum--;
            m3->mc_top--;
          }
        }
      }
    } else {
      mdbx_debug("root page %" PRIaPGNO
                 " doesn't need rebalancing (flags 0x%x)",
                 mp->mp_pgno, mp->mp_flags16);
    }
    return MDBX_SUCCESS;
  }

  /* The parent (branch page) must have at least 2 pointers,
   * otherwise the tree is invalid. */
  ptop = mc->mc_top - 1;
  assert(page_numkeys(mc->mc_pg[ptop]) > 1);

  /* Leaf page fill factor is below the threshold.
   * Try to move keys from left or right neighbor, or
   * merge with a neighbor page. */

  /* Find neighbors. */
  cursor_copy(mc, &mn);
  mn.subordinate = nullptr;

  oldki = mc->mc_ki[mc->mc_top];
  if (mc->mc_ki[ptop] == 0) {
    /* We're the leftmost leaf in our parent. */
    mdbx_debug("reading right neighbor");
    mn.mc_ki[ptop]++;
    node = node_ptr(mc->mc_pg[ptop], mn.mc_ki[ptop]);
    rc = page_get(mc->mc_txn, node_get_pgno(node), &mn.mc_pg[mn.mc_top],
                  nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mn.mc_ki[mn.mc_top] = 0;
    mc->mc_ki[mc->mc_top] = page_numkeys(mc->mc_pg[mc->mc_top]);
    fromleft = 0;
  } else {
    /* There is at least one neighbor to the left. */
    mdbx_debug("reading left neighbor");
    mn.mc_ki[ptop]--;
    node = node_ptr(mc->mc_pg[ptop], mn.mc_ki[ptop]);
    rc = page_get(mc->mc_txn, node_get_pgno(node), &mn.mc_pg[mn.mc_top],
                  nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mn.mc_ki[mn.mc_top] = page_numkeys(mn.mc_pg[mn.mc_top]) - 1;
    mc->mc_ki[mc->mc_top] = 0;
    fromleft = 1;
  }

  mdbx_debug("found neighbor page %" PRIaPGNO " (%u keys, %.1f%% full)",
             mn.mc_pg[mn.mc_top]->mp_pgno, page_numkeys(mn.mc_pg[mn.mc_top]),
             (float)PAGEFILL(mc->mc_txn->mt_book, mn.mc_pg[mn.mc_top]) / 10);

  /* If the neighbor page is above threshold and has enough keys,
   * move one key from it. Otherwise we should try to merge them.
   * (A branch page must never have less than 2 keys.) */
  if (PAGEFILL(mc->mc_txn->mt_book, mn.mc_pg[mn.mc_top]) >= thresh &&
      page_numkeys(mn.mc_pg[mn.mc_top]) > minkeys) {
    rc = node_move(&mn, mc, fromleft);
    if (fromleft) {
      /* if we inserted on left, bump position up */
      oldki++;
    }
  } else {
    if (!fromleft) {
      rc = page_merge(&mn, mc);
    } else {
      oldki += page_numkeys(mn.mc_pg[mn.mc_top]);
      mn.mc_ki[mn.mc_top] += mc->mc_ki[mn.mc_top] + 1;
      /* We want mdbx_rebalance to find mn when doing fixups */
      WITH_CURSOR_TRACKING(mn, rc = page_merge(mc, &mn));
      cursor_copy(&mn, mc);
    }
    mc->mc_state8 &= ~C_EOF;
  }
  mc->mc_ki[mc->mc_top] = oldki;
  return rc;
}

/* Complete a delete operation started by mdbx_cursor_delete(). */
static int mdbx_cr_del0(MDBX_cursor *mc) {
  int rc;
  page_t *mp;
  indx_t ki;
  unsigned nkeys;
  MDBX_cursor *m2, *m3;
  MDBX_aah aah = mc->mc_ah;

  ki = mc->mc_ki[mc->mc_top];
  mp = mc->mc_pg[mc->mc_top];
  node_del(mc, mc->mc_aht->aa.xsize32);
  mc->mc_aht->aa.entries--;
  {
    /* Adjust other cursors pointing to mp */
    for (m2 = mc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
      m3 = (mc->mc_state8 & C_SUBCURSOR) ? &m2->subordinate.mx_cursor : m2;
      if (!(m2->mc_state8 & m3->mc_state8 & C_INITIALIZED))
        continue;
      if (m3 == mc || m3->mc_snum < mc->mc_snum)
        continue;
      if (m3->mc_pg[mc->mc_top] == mp) {
        if (m3->mc_ki[mc->mc_top] == ki) {
          m3->mc_state8 |= C_AFTERDELETE;
          if (mc->mc_aht->aa.flags16 & MDBX_DUPSORT) {
            /* Sub-cursor referred into dataset which is gone */
            m3->subordinate.mx_cursor.mc_state8 &= ~(C_INITIALIZED | C_EOF);
          }
          continue;
        } else if (m3->mc_ki[mc->mc_top] > ki) {
          m3->mc_ki[mc->mc_top]--;
        }
        XCURSOR_REFRESH(m3, mc->mc_top, m3->mc_pg[mc->mc_top]);
      }
    }
  }
  rc = tree_rebalance(mc);

  if (likely(rc == MDBX_SUCCESS)) {
    /* AA is totally empty now, just bail out.
     * Other cursors adjustments were already done
     * by mdbx_rebalance and aren't needed here. */
    if (!mc->mc_snum) {
      mc->mc_state8 |= C_AFTERDELETE | C_EOF;
      return rc;
    }

    mp = mc->mc_pg[mc->mc_top];
    nkeys = page_numkeys(mp);

    /* Adjust other cursors pointing to mp */
    for (m2 = mc->mc_txn->mt_cursors[aah]; !rc && m2; m2 = m2->mc_next) {
      m3 = (mc->mc_state8 & C_SUBCURSOR) ? &m2->subordinate.mx_cursor : m2;
      if (!(m2->mc_state8 & m3->mc_state8 & C_INITIALIZED))
        continue;
      if (m3->mc_snum < mc->mc_snum)
        continue;
      if (m3->mc_pg[mc->mc_top] == mp) {
        /* if m3 points past last node in page, find next sibling */
        if (m3->mc_ki[mc->mc_top] >= mc->mc_ki[mc->mc_top]) {
          if (m3->mc_ki[mc->mc_top] >= nkeys) {
            rc = cursor_sibling(m3, 1);
            if (rc == MDBX_NOTFOUND) {
              m3->mc_state8 |= C_EOF;
              rc = MDBX_SUCCESS;
              continue;
            }
          }
          if (mc->mc_aht->aa.flags16 & MDBX_DUPSORT) {
            node_t *node =
                node_ptr(m3->mc_pg[m3->mc_top], m3->mc_ki[m3->mc_top]);
            /* If this node has dupdata, it may need to be reinited
             * because its data has moved.
             * If the xcursor was not initd it must be reinited.
             * Else if node points to a subDB, nothing is needed. */
            if (node->node_flags8 & NODE_DUP) {
              if (m3->subordinate.mx_cursor.mc_state8 & C_INITIALIZED) {
                if (!(node->node_flags8 & NODE_SUBTREE))
                  m3->subordinate.mx_cursor.mc_pg[0] = NODEDATA(node);
              } else {
                subordinate_setup(m3, node);
                m3->subordinate.mx_cursor.mc_state8 |= C_AFTERDELETE;
              }
            }
          }
        }
      }
    }
    mc->mc_state8 |= C_AFTERDELETE;
  }

  if (unlikely(rc != MDBX_SUCCESS))
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

int mdbx_del(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data) {
  if (unlikely(!key || !txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  if (unlikely(!TXN_AAH_EXIST(txn, aah, MDBX_AAH_USER)))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_flags & (MDBX_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->mt_flags & MDBX_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  return mdbx_del0(txn, aah, key, data, 0);
}

static int mdbx_del0(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
                     unsigned flags) {
  MDBX_cursor mc;
  enum MDBX_cursor_op op;
  MDBX_iov rdata;
  int exact = 0;
  DKBUF;

  mdbx_debug("====> delete AA %" PRIuFAST32 " key [%s], data [%s]", aah,
             DKEY(key), DVAL(data));

  int rc = cursor_init(&mc, txn, aah);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (data) {
    op = MDBX_GET_BOTH;
    rdata = *data;
    data = &rdata;
  } else {
    op = MDBX_SET;
    flags |= MDBX_IUD_NODUP;
  }
  rc = cursor_set(&mc, key, data, op, &exact);
  if (likely(rc == MDBX_SUCCESS)) {
    /* let page_split know about this cursor if needed:
     * delete will trigger a rebalance; if it needs to move
     * a node from one page to another, it will have to
     * update the parent's separator key(s). If the new sepkey
     * is larger than the current one, the parent page may
     * run out of space, triggering a split. We need this
     * cursor to be consistent until the end of the rebalance. */
    mc.mc_next = txn->mt_cursors[aah];
    txn->mt_cursors[aah] = &mc;
    rc = mdbx_cursor_delete(&mc, flags);
    txn->mt_cursors[aah] = mc.mc_next;
  }
  return rc;
}

/* Split a page and insert a new node.
 * Set MDBX_TXN_ERROR on failure.
 * [in,out] mc Cursor pointing to the page and desired insertion index.
 * The cursor will be updated to point to the actual page and index where
 * the node got inserted after the split.
 * [in] newkey The key for the newly inserted node.
 * [in] newdata The data for the newly inserted node.
 * [in] newpgno The page number, if the new node is a branch node.
 * [in] nflags The NODE_ADD_FLAGS for the new node.
 * Returns 0 on success, non-zero on failure. */
static int page_split(MDBX_cursor *mc, MDBX_iov *newkey, MDBX_iov *newdata,
                      pgno_t newpgno, unsigned nflags) {
  unsigned flags;
  int rc = MDBX_SUCCESS, new_root = 0, did_split = 0;
  pgno_t pgno = 0;
  unsigned i, ptop;
  MDBX_milieu *bk = mc->mc_txn->mt_book;
  node_t *node;
  MDBX_iov sepkey, rkey, xdata, *rdata = &xdata;
  page_t *copy = nullptr;
  page_t *rp, *pp;
  MDBX_cursor mn;
  DKBUF;

  page_t *mp = mc->mc_pg[mc->mc_top];
  unsigned newindx = mc->mc_ki[mc->mc_top];
  unsigned nkeys = page_numkeys(mp);

  mdbx_debug("-----> splitting %s page %" PRIaPGNO
             " and adding [%s] at index %i/%i",
             IS_LEAF(mp) ? "leaf" : "branch", mp->mp_pgno, DKEY(newkey),
             mc->mc_ki[mc->mc_top], nkeys);

  /* Create a right sibling. */
  if ((rc = page_new(mc, mp->mp_flags16, 1, &rp)))
    return rc;
  rp->mp_leaf2_ksize16 = mp->mp_leaf2_ksize16;
  mdbx_debug("new right sibling: page %" PRIaPGNO "", rp->mp_pgno);

  /* Usually when splitting the root page, the cursor
   * height is 1. But when called from mdbx_update_key,
   * the cursor height may be greater because it walks
   * up the stack while finding the branch slot to update. */
  if (mc->mc_top < 1) {
    if ((rc = page_new(mc, P_BRANCH, 1, &pp)))
      goto done;
    /* shift current top to make room for new parent */
    for (i = mc->mc_snum; i > 0; i--) {
      mc->mc_pg[i] = mc->mc_pg[i - 1];
      mc->mc_ki[i] = mc->mc_ki[i - 1];
    }
    mc->mc_pg[0] = pp;
    mc->mc_ki[0] = 0;
    mc->mc_aht->aa.root = pp->mp_pgno;
    mdbx_debug("root split! new root = %" PRIaPGNO "", pp->mp_pgno);
    new_root = mc->mc_aht->aa.depth16++;

    /* Add left (implicit) pointer. */
    if (unlikely((rc = node_add(mc, 0, nullptr, nullptr, mp->mp_pgno, 0)) !=
                 MDBX_SUCCESS)) {
      /* undo the pre-push */
      mc->mc_pg[0] = mc->mc_pg[1];
      mc->mc_ki[0] = mc->mc_ki[1];
      mc->mc_aht->aa.root = mp->mp_pgno;
      mc->mc_aht->aa.depth16--;
      goto done;
    }
    mc->mc_snum++;
    mc->mc_top++;
    ptop = 0;
  } else {
    ptop = mc->mc_top - 1;
    mdbx_debug("parent branch page is %" PRIaPGNO "", mc->mc_pg[ptop]->mp_pgno);
  }

  cursor_copy(mc, &mn);
  mn.subordinate = nullptr;
  mn.mc_pg[mn.mc_top] = rp;
  mn.mc_ki[ptop] = mc->mc_ki[ptop] + 1;

  unsigned split_indx;
  if (nflags & MDBX_IUD_APPEND) {
    mn.mc_ki[mn.mc_top] = 0;
    sepkey = *newkey;
    split_indx = newindx;
    nkeys = 0;
  } else {
    split_indx = (nkeys + 1) / 2;

    if (IS_DFL(rp)) {
      char *split, *ins;
      int x;
      unsigned lsize, rsize, keysize;
      /* Move half of the keys to the right sibling */
      x = mc->mc_ki[mc->mc_top] - split_indx;
      keysize = mc->mc_aht->aa.xsize32;
      split = DFLKEY(mp, split_indx, keysize);
      rsize = (nkeys - split_indx) * keysize;
      lsize = (nkeys - split_indx) * sizeof(indx_t);
      assert(mp->mp_lower >= lsize);
      mp->mp_lower -= (indx_t)lsize;
      assert(rp->mp_lower + lsize <= UINT16_MAX);
      rp->mp_lower += (indx_t)lsize;
      assert(mp->mp_upper + rsize - lsize <= UINT16_MAX);
      mp->mp_upper += (indx_t)(rsize - lsize);
      assert(rp->mp_upper >= rsize - lsize);
      rp->mp_upper -= (indx_t)(rsize - lsize);
      sepkey.iov_len = keysize;
      if (newindx == split_indx) {
        sepkey.iov_base = newkey->iov_base;
      } else {
        sepkey.iov_base = split;
      }
      if (x < 0) {
        assert(keysize >= sizeof(indx_t));
        ins = DFLKEY(mp, mc->mc_ki[mc->mc_top], keysize);
        memcpy(rp->mp_ptrs, split, rsize);
        sepkey.iov_base = rp->mp_ptrs;
        memmove(ins + keysize, ins,
                (split_indx - mc->mc_ki[mc->mc_top]) * keysize);
        memcpy(ins, newkey->iov_base, keysize);
        assert(UINT16_MAX - mp->mp_lower >= (int)sizeof(indx_t));
        mp->mp_lower += sizeof(indx_t);
        assert(mp->mp_upper >= keysize - sizeof(indx_t));
        mp->mp_upper -= (indx_t)(keysize - sizeof(indx_t));
      } else {
        if (x)
          memcpy(rp->mp_ptrs, split, x * keysize);
        ins = DFLKEY(rp, x, keysize);
        memcpy(ins, newkey->iov_base, keysize);
        memcpy(ins + keysize, split + x * keysize, rsize - x * keysize);
        assert(UINT16_MAX - rp->mp_lower >= (int)sizeof(indx_t));
        rp->mp_lower += sizeof(indx_t);
        assert(rp->mp_upper >= keysize - sizeof(indx_t));
        rp->mp_upper -= (indx_t)(keysize - sizeof(indx_t));
        assert(x <= UINT16_MAX);
        mc->mc_ki[mc->mc_top] = (indx_t)x;
      }
    } else {
      size_t psize, nsize, k;
      /* Maximum free space in an empty page */
      unsigned pmax = bk->me_psize - PAGEHDRSZ;
      if (IS_LEAF(mp))
        nsize = leaf_size(bk, newkey, newdata);
      else
        nsize = branch_size(bk, newkey);
      nsize = EVEN(nsize);

      /* grab a page to hold a temporary copy */
      copy = page_malloc(mc->mc_txn, 1);
      if (unlikely(copy == nullptr)) {
        rc = MDBX_ENOMEM;
        goto done;
      }
      copy->mp_pgno = mp->mp_pgno;
      copy->mp_flags16 = mp->mp_flags16;
      copy->mp_lower = 0;
      assert(bk->me_psize - PAGEHDRSZ <= UINT16_MAX);
      copy->mp_upper = (indx_t)(bk->me_psize - PAGEHDRSZ);

      /* prepare to insert */
      for (unsigned j = i = 0; i < nkeys; i++) {
        if (i == newindx)
          copy->mp_ptrs[j++] = 0;
        copy->mp_ptrs[j++] = mp->mp_ptrs[i];
      }

      /* When items are relatively large the split point needs
       * to be checked, because being off-by-one will make the
       * difference between success or failure in mdbx_node_add.
       *
       * It's also relevant if a page happens to be laid out
       * such that one half of its nodes are all "small" and
       * the other half of its nodes are "large." If the new
       * item is also "large" and falls on the half with
       * "large" nodes, it also may not fit.
       *
       * As a final tweak, if the new item goes on the last
       * spot on the page (and thus, onto the new page), bias
       * the split so the new page is emptier than the old page.
       * This yields better packing during sequential inserts. */
      int dir;
      if (nkeys < 20 || nsize > pmax / 16 || newindx >= nkeys) {
        /* Find split point */
        psize = 0;
        if (newindx <= split_indx || newindx >= nkeys) {
          i = 0;
          dir = 1;
          k = (newindx >= nkeys) ? nkeys : split_indx + 1 + IS_LEAF(mp);
        } else {
          i = nkeys;
          dir = -1;
          k = split_indx - 1;
        }
        for (; i != k; i += dir) {
          if (i == newindx) {
            psize += nsize;
            node = nullptr;
          } else {
            node = (node_t *)((char *)mp + copy->mp_ptrs[i] + PAGEHDRSZ);
            psize += NODESIZE + node_get_keysize(node) + sizeof(indx_t);
            if (IS_LEAF(mp)) {
              if (unlikely(node->node_flags8 & NODE_BIG))
                psize += sizeof(pgno_t);
              else
                psize += node_get_datasize(node);
            }
            psize = EVEN(psize);
          }
          if (psize > pmax || i == k - dir) {
            split_indx = i + (dir < 0);
            break;
          }
        }
      }
      if (split_indx == newindx) {
        sepkey.iov_len = newkey->iov_len;
        sepkey.iov_base = newkey->iov_base;
      } else {
        node = (node_t *)((char *)mp + copy->mp_ptrs[split_indx] + PAGEHDRSZ);
        sepkey.iov_len = node->mn_ksize16;
        sepkey.iov_base = NODEKEY(node);
      }
    }
  }

  mdbx_debug("separator is %d [%s]", split_indx, DKEY(&sepkey));

  /* Copy separator key to the parent. */
  if (page_spaceleft(mn.mc_pg[ptop]) < branch_size(bk, &sepkey)) {
    int snum = mc->mc_snum;
    mn.mc_snum--;
    mn.mc_top--;
    did_split = 1;
    /* We want other splits to find mn when doing fixups */
    WITH_CURSOR_TRACKING(
        mn, rc = page_split(&mn, &sepkey, nullptr, rp->mp_pgno, 0));
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;

    /* root split? */
    if (snum < (int)mc->mc_snum)
      ptop++;

    /* Right page might now have changed parent.
     * Check if left page also changed parent. */
    if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
        mc->mc_ki[ptop] >= page_numkeys(mc->mc_pg[ptop])) {
      for (i = 0; i < ptop; i++) {
        mc->mc_pg[i] = mn.mc_pg[i];
        mc->mc_ki[i] = mn.mc_ki[i];
      }
      mc->mc_pg[ptop] = mn.mc_pg[ptop];
      if (mn.mc_ki[ptop]) {
        mc->mc_ki[ptop] = mn.mc_ki[ptop] - 1;
      } else {
        /* find right page's left sibling */
        mc->mc_ki[ptop] = mn.mc_ki[ptop];
        rc = cursor_sibling(mc, 0);
      }
    }
  } else {
    mn.mc_top--;
    rc = node_add(&mn, mn.mc_ki[ptop], &sepkey, nullptr, rp->mp_pgno, 0);
    mn.mc_top++;
  }
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc == MDBX_NOTFOUND) /* improper mdbx_cr_sibling() result */ {
      mdbx_error("unexpected %s", "MDBX_NOTFOUND");
      rc = MDBX_PROBLEM;
    }
    goto done;
  }
  if (nflags & MDBX_IUD_APPEND) {
    mc->mc_pg[mc->mc_top] = rp;
    mc->mc_ki[mc->mc_top] = 0;
    rc = node_add(mc, 0, newkey, newdata, newpgno, nflags);
    if (rc)
      goto done;
    for (i = 0; i < mc->mc_top; i++)
      mc->mc_ki[i] = mn.mc_ki[i];
  } else if (!IS_DFL(mp)) {
    /* Move nodes */
    mc->mc_pg[mc->mc_top] = rp;
    i = split_indx;
    indx_t n = 0;
    do {
      if (i == newindx) {
        rkey.iov_base = newkey->iov_base;
        rkey.iov_len = newkey->iov_len;
        if (IS_LEAF(mp)) {
          rdata = newdata;
        } else
          pgno = newpgno;
        flags = nflags;
        /* Update index for the new key. */
        mc->mc_ki[mc->mc_top] = n;
      } else {
        node = (node_t *)((char *)mp + copy->mp_ptrs[i] + PAGEHDRSZ);
        rkey.iov_base = NODEKEY(node);
        rkey.iov_len = node->mn_ksize16;
        if (IS_LEAF(mp)) {
          xdata.iov_base = NODEDATA(node);
          xdata.iov_len = node_get_datasize(node);
          rdata = &xdata;
        } else
          pgno = node_get_pgno(node);
        flags = node->node_flags8;
      }

      if (!IS_LEAF(mp) && n == 0) {
        /* First branch index doesn't need key data. */
        rkey.iov_len = 0;
      }

      rc = node_add(mc, n, &rkey, rdata, pgno, flags);
      if (rc)
        goto done;
      if (i == nkeys) {
        i = 0;
        n = 0;
        mc->mc_pg[mc->mc_top] = copy;
      } else {
        i++;
        n++;
      }
    } while (i != split_indx);

    nkeys = page_numkeys(copy);
    for (i = 0; i < nkeys; i++)
      mp->mp_ptrs[i] = copy->mp_ptrs[i];
    mp->mp_lower = copy->mp_lower;
    mp->mp_upper = copy->mp_upper;
    memcpy(node_ptr(mp, nkeys - 1), node_ptr(copy, nkeys - 1),
           bk->me_psize - copy->mp_upper - PAGEHDRSZ);

    /* reset back to original page */
    if (newindx < split_indx) {
      mc->mc_pg[mc->mc_top] = mp;
    } else {
      mc->mc_pg[mc->mc_top] = rp;
      mc->mc_ki[ptop]++;
      /* Make sure mc_ki is still valid. */
      if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
          mc->mc_ki[ptop] >= page_numkeys(mc->mc_pg[ptop])) {
        for (i = 0; i <= ptop; i++) {
          mc->mc_pg[i] = mn.mc_pg[i];
          mc->mc_ki[i] = mn.mc_ki[i];
        }
      }
    }
    if (nflags & MDBX_IUD_RESERVE) {
      node = node_ptr(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
      if (likely(!(node->node_flags8 & NODE_BIG)))
        newdata->iov_base = NODEDATA(node);
    }
  } else {
    if (newindx >= split_indx) {
      mc->mc_pg[mc->mc_top] = rp;
      mc->mc_ki[ptop]++;
      /* Make sure mc_ki is still valid. */
      if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
          mc->mc_ki[ptop] >= page_numkeys(mc->mc_pg[ptop])) {
        for (i = 0; i <= ptop; i++) {
          mc->mc_pg[i] = mn.mc_pg[i];
          mc->mc_ki[i] = mn.mc_ki[i];
        }
      }
    }
  }

  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    MDBX_aah aah = mc->mc_ah;
    nkeys = page_numkeys(mp);

    for (m2 = mc->mc_txn->mt_cursors[aah]; m2; m2 = m2->mc_next) {
      if (mc->mc_state8 & C_SUBCURSOR)
        m3 = &m2->subordinate.mx_cursor;
      else
        m3 = m2;
      if (m3 == mc)
        continue;
      if (!(m2->mc_state8 & m3->mc_state8 & C_INITIALIZED))
        continue;
      if (new_root) {
        int k;
        /* sub cursors may be on different AA */
        if (m3->mc_pg[0] != mp)
          continue;
        /* root split */
        for (k = new_root; k >= 0; k--) {
          m3->mc_ki[k + 1] = m3->mc_ki[k];
          m3->mc_pg[k + 1] = m3->mc_pg[k];
        }
        m3->mc_ki[0] = (m3->mc_ki[0] >= nkeys) ? 1 : 0;
        m3->mc_pg[0] = mc->mc_pg[0];
        m3->mc_snum++;
        m3->mc_top++;
      }
      if (m3->mc_top >= mc->mc_top && m3->mc_pg[mc->mc_top] == mp) {
        if (m3->mc_ki[mc->mc_top] >= newindx && !(nflags & MDBX_SPLIT_REPLACE))
          m3->mc_ki[mc->mc_top]++;
        if (m3->mc_ki[mc->mc_top] >= nkeys) {
          m3->mc_pg[mc->mc_top] = rp;
          assert(m3->mc_ki[mc->mc_top] >= nkeys);
          m3->mc_ki[mc->mc_top] -= (indx_t)nkeys;
          for (i = 0; i < mc->mc_top; i++) {
            m3->mc_ki[i] = mn.mc_ki[i];
            m3->mc_pg[i] = mn.mc_pg[i];
          }
        }
      } else if (!did_split && m3->mc_top >= ptop &&
                 m3->mc_pg[ptop] == mc->mc_pg[ptop] &&
                 m3->mc_ki[ptop] >= mc->mc_ki[ptop]) {
        m3->mc_ki[ptop]++;
      }
      if (IS_LEAF(mp))
        XCURSOR_REFRESH(m3, mc->mc_top, m3->mc_pg[mc->mc_top]);
    }
  }
  mdbx_debug("mp left: %d, rp left: %d", page_spaceleft(mp),
             page_spaceleft(rp));

done:
  if (copy) /* tmp page */
    dpage_free(bk, copy);
  if (unlikely(rc != MDBX_SUCCESS))
    mc->mc_txn->mt_flags |= MDBX_TXN_ERROR;
  return rc;
}

int mdbx_put(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
             unsigned flags) {
  MDBX_cursor mc;

  if (unlikely(!key || !data || !txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  if (unlikely(!TXN_AAH_EXIST(txn, aah, MDBX_AAH_USER)))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_flags & (MDBX_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->mt_flags & MDBX_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  int rc = cursor_init(&mc, txn, aah);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  mc.mc_next = txn->mt_cursors[aah];
  txn->mt_cursors[aah] = &mc;

  /* LY: support for update (explicit overwrite) */
  if (flags & MDBX_IUD_CURRENT) {
    rc = mdbx_cursor_get(&mc, key, nullptr, MDBX_SET);
    if (likely(rc == MDBX_SUCCESS) &&
        (txn->txn_aht_array[aah].aa.flags16 & MDBX_DUPSORT)) {
      /* LY: allows update (explicit overwrite) only for unique keys */
      node_t *leaf = node_ptr(mc.mc_pg[mc.mc_top], mc.mc_ki[mc.mc_top]);
      if (F_ISSET(leaf->node_flags8, NODE_DUP)) {
        assert(XCURSOR_INITED(&mc) && mc.subordinate->mx_aht.aa.entries > 1);
        rc = MDBX_EMULTIVAL;
      }
    }
  }

  if (likely(rc == MDBX_SUCCESS))
    rc = cursor_put(&mc, key, data, flags);
  txn->mt_cursors[aah] = mc.mc_next;

  return rc;
}

#ifndef MDBX_WBUF
#define MDBX_WBUF (1024 * 1024)
#endif
#define MDBX_EOF 0x10 /* mdbx_bk_copyfd1() is done reading */

/* State needed for a double-buffering compacting copy. */
struct copy_ctx_ {
  MDBX_milieu *mc_book;
  MDBX_txn *mc_txn;
  mdbx_condmutex_t mc_condmutex;
  char *mc_wbuf[2];
  char *mc_over[2];
  size_t mc_wlen[2];
  size_t mc_olen[2];
  MDBX_filehandle_t mc_fd;
  volatile int mc_error;
  pgno_t mc_next_pgno;
  short mc_toggle; /* Buffer number in provider */
  short mc_new;    /* (0-2 buffers to write) | (MDBX_EOF at end) */
  /* Error code.  Never cleared if set.  Both threads can set nonzero
   * to fail the copy.  Not mutex-protected, MDBX expects atomic int. */
};

/* Dedicated writer thread for compacting copy. */
static THREAD_RESULT __cold THREAD_CALL mdbx_bk_copythr(void *arg) {
  copy_ctx_t *my = arg;
  char *ptr;
  int toggle = 0;
  int rc;

#if defined(F_SETNOSIGPIPE)
  /* OS X delivers SIGPIPE to the whole process, not the thread that caused
   * it.
   * Disable SIGPIPE using platform specific fcntl. */
  int enabled = 1;
  if (fcntl(my->mc_fd, F_SETNOSIGPIPE, &enabled))
    my->mc_error = mdbx_get_errno();
#endif

#if defined(SIGPIPE) && !defined(_WIN32) && !defined(_WIN64)
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  rc = pthread_sigmask(SIG_BLOCK, &set, nullptr);
  if (rc != 0)
    my->mc_error = rc;
#endif

  mdbx_condmutex_lock(&my->mc_condmutex);
  while (!my->mc_error) {
    while (!my->mc_new)
      mdbx_condmutex_wait(&my->mc_condmutex);
    if (my->mc_new == 0 + MDBX_EOF) /* 0 buffers, just EOF */
      break;
    size_t wsize = my->mc_wlen[toggle];
    ptr = my->mc_wbuf[toggle];
  again:
    if (wsize > 0 && !my->mc_error) {
      rc = mdbx_write(my->mc_fd, ptr, wsize);
      if (rc != MDBX_SUCCESS) {
#if defined(SIGPIPE) && !defined(_WIN32) && !defined(_WIN64)
        if (rc == EPIPE) {
          /* Collect the pending SIGPIPE, otherwise (at least OS X)
           * gives it to the process on thread-exit (ITS#8504). */
          int tmp;
          sigwait(&set, &tmp);
        }
#endif
        my->mc_error = rc;
      }
    }

    /* If there's an overflow page tail, write it too */
    if (my->mc_olen[toggle]) {
      wsize = my->mc_olen[toggle];
      ptr = my->mc_over[toggle];
      my->mc_olen[toggle] = 0;
      goto again;
    }
    my->mc_wlen[toggle] = 0;
    toggle ^= 1;
    /* Return the empty buffer to provider */
    my->mc_new--;
    mdbx_condmutex_signal(&my->mc_condmutex);
  }
  mdbx_condmutex_unlock(&my->mc_condmutex);
  return (THREAD_RESULT)0;
}

/* Give buffer and/or MDBX_EOF to writer thread, await unused buffer.
 *
 * [in] my control structure.
 * [in] adjust (1 to hand off 1 buffer) | (MDBX_EOF when ending). */
static int __cold mdbx_bk_cthr_toggle(copy_ctx_t *my, int adjust) {
  mdbx_condmutex_lock(&my->mc_condmutex);
  my->mc_new += (short)adjust;
  mdbx_condmutex_signal(&my->mc_condmutex);
  while (my->mc_new & 2) /* both buffers in use */
    mdbx_condmutex_wait(&my->mc_condmutex);
  mdbx_condmutex_unlock(&my->mc_condmutex);

  my->mc_toggle ^= (adjust & 1);
  /* Both threads reset mc_wlen, to be safe from threading errors */
  my->mc_wlen[my->mc_toggle] = 0;
  return my->mc_error;
}

/* Depth-first tree traversal for compacting copy.
 * [in] my control structure.
 * [in,out] pg database root.
 * [in] flags includes NODE_DUP if it is a sorted-duplicate sub-AA. */
static int __cold bk_copy_walk(copy_ctx_t *my, pgno_t *pg, int flags) {
  MDBX_cursor mc;
  node_t *ni;
  page_t *mo, *mp, *leaf;
  char *buf, *ptr;
  int rc, toggle;
  unsigned i;

  /* Empty AA, nothing to do */
  if (*pg == P_INVALID)
    return MDBX_SUCCESS;

  memset(&mc, 0, sizeof(mc));
  mc.primal.mc_snum = 1;
  mc.primal.mc_txn = my->mc_txn;

  rc = page_get(my->mc_txn, *pg, &mc.primal.mc_pg[0], nullptr);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  rc = page_search_root(&mc.primal, nullptr, MDBX_PS_FIRST);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Make cursor pages writable */
  buf = ptr = malloc(pgno2bytes(my->mc_book, mc.primal.mc_snum));
  if (buf == nullptr)
    return MDBX_ENOMEM;

  for (i = 0; i < mc.primal.mc_top; i++) {
    page_copy((page_t *)ptr, mc.primal.mc_pg[i], my->mc_book->me_psize);
    mc.primal.mc_pg[i] = (page_t *)ptr;
    ptr += my->mc_book->me_psize;
  }

  /* This is writable space for a leaf page. Usually not needed. */
  leaf = (page_t *)ptr;

  toggle = my->mc_toggle;
  while (mc.primal.mc_snum > 0) {
    unsigned n;
    mp = mc.primal.mc_pg[mc.primal.mc_top];
    n = page_numkeys(mp);

    if (IS_LEAF(mp)) {
      if (!IS_DFL(mp) && !(flags & NODE_DUP)) {
        for (i = 0; i < n; i++) {
          ni = node_ptr(mp, i);
          if (unlikly(ni->node_flags8 & NODE_BIG)) {
            page_t *omp;

            /* Need writable leaf */
            if (mp != leaf) {
              mc.primal.mc_pg[mc.primal.mc_top] = leaf;
              page_copy(leaf, mp, my->mc_book->me_psize);
              mp = leaf;
              ni = node_ptr(mp, i);
            }

            pgno_t pgno = get_pgno_lea16(NODEDATA(ni));
            set_pgno_lea16(NODEDATA(ni), my->mc_next_pgno);
            rc = page_get(my->mc_txn, pgno, &omp, nullptr);
            if (unlikely(rc != MDBX_SUCCESS))
              goto done;
            if (my->mc_wlen[toggle] >= MDBX_WBUF) {
              rc = mdbx_bk_cthr_toggle(my, 1);
              if (unlikely(rc != MDBX_SUCCESS))
                goto done;
              toggle = my->mc_toggle;
            }
            mo = (page_t *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
            memcpy(mo, omp, my->mc_book->me_psize);
            mo->mp_pgno = my->mc_next_pgno;
            my->mc_next_pgno += omp->mp_pages;
            my->mc_wlen[toggle] += my->mc_book->me_psize;
            if (omp->mp_pages > 1) {
              my->mc_olen[toggle] = pgno2bytes(my->mc_book, omp->mp_pages - 1);
              my->mc_over[toggle] = (char *)omp + my->mc_book->me_psize;
              rc = mdbx_bk_cthr_toggle(my, 1);
              if (unlikely(rc != MDBX_SUCCESS))
                goto done;
              toggle = my->mc_toggle;
            }
          } else if (ni->node_flags8 & NODE_SUBTREE) {
            aatree_t db;

            /* Need writable leaf */
            if (mp != leaf) {
              mc.primal.mc_pg[mc.primal.mc_top] = leaf;
              page_copy(leaf, mp, my->mc_book->me_psize);
              mp = leaf;
              ni = node_ptr(mp, i);
            }

            memcpy(&db, NODEDATA(ni), sizeof(db));
            my->mc_toggle = (short)toggle;
            rc = bk_copy_walk(my, &db.aa_root, ni->node_flags8 & NODE_DUP);
            if (rc)
              goto done;
            toggle = my->mc_toggle;
            memcpy(NODEDATA(ni), &db, sizeof(db));
          }
        }
      }
    } else {
      mc.primal.mc_ki[mc.primal.mc_top]++;
      if (mc.primal.mc_ki[mc.primal.mc_top] < n) {
        pgno_t pgno;
      again:
        ni = node_ptr(mp, mc.primal.mc_ki[mc.primal.mc_top]);
        pgno = node_get_pgno(ni);
        rc = page_get(my->mc_txn, pgno, &mp, nullptr);
        if (unlikely(rc != MDBX_SUCCESS))
          goto done;
        mc.primal.mc_top++;
        mc.primal.mc_snum++;
        mc.primal.mc_ki[mc.primal.mc_top] = 0;
        if (IS_BRANCH(mp)) {
          /* Whenever we advance to a sibling branch page,
           * we must proceed all the way down to its first leaf. */
          page_copy(mc.primal.mc_pg[mc.primal.mc_top], mp,
                    my->mc_book->me_psize);
          goto again;
        } else
          mc.primal.mc_pg[mc.primal.mc_top] = mp;
        continue;
      }
    }
    if (my->mc_wlen[toggle] >= MDBX_WBUF) {
      rc = mdbx_bk_cthr_toggle(my, 1);
      if (unlikely(rc != MDBX_SUCCESS))
        goto done;
      toggle = my->mc_toggle;
    }
    mo = (page_t *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
    page_copy(mo, mp, my->mc_book->me_psize);
    mo->mp_pgno = my->mc_next_pgno++;
    my->mc_wlen[toggle] += my->mc_book->me_psize;
    if (mc.primal.mc_top) {
      /* Update parent if there is one */
      ni = node_ptr(mc.primal.mc_pg[mc.primal.mc_top - 1],
                    mc.primal.mc_ki[mc.primal.mc_top - 1]);
      node_set_pgno(ni, mo->mp_pgno);
      cursor_pop(&mc.primal);
    } else {
      /* Otherwise we're done */
      *pg = mo->mp_pgno;
      break;
    }
  }
done:
  free(buf);
  return rc;
}

/* Copy databook with compaction. */
static int __cold bk_copy_compact(MDBX_milieu *bk, MDBX_filehandle_t fd) {
  MDBX_txn *txn = nullptr;
  mdbx_thread_t thr;
  copy_ctx_t my;
  memset(&my, 0, sizeof(my));

  int rc = mdbx_condmutex_init(&my.mc_condmutex);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  rc = mdbx_memalign_alloc(bk->me_os_psize, MDBX_WBUF * 2,
                           (void **)&my.mc_wbuf[0]);
  if (unlikely(rc != MDBX_SUCCESS))
    goto done;

  memset(my.mc_wbuf[0], 0, MDBX_WBUF * 2);
  my.mc_wbuf[1] = my.mc_wbuf[0] + MDBX_WBUF;
  my.mc_next_pgno = NUM_METAS;
  my.mc_book = bk;
  my.mc_fd = fd;
  rc = mdbx_thread_create(&thr, mdbx_bk_copythr, &my);
  if (unlikely(rc != MDBX_SUCCESS))
    goto done;

  rc = mdbx_tn_begin(bk, nullptr, MDBX_RDONLY, &txn);
  if (unlikely(rc != MDBX_SUCCESS))
    goto finish;

  page_t *meta = init_metas(bk, my.mc_wbuf[0]);

  /* Set metapage 1 with current main AA */
  pgno_t new_root, root = aht_main(txn)->aa.root;
  if ((new_root = root) != P_INVALID) {
    /* Count free pages + GACO pages.  Subtract from last_pg
     * to find the new last_pg, which also becomes the new root. */
    pgno_t freecount = 0;
    MDBX_cursor mc;
    MDBX_iov key, data;

    rc = cursor_init(&mc, txn, aht_gaco(txn));
    if (unlikely(rc != MDBX_SUCCESS))
      goto finish;
    while ((rc = mdbx_cursor_get(&mc, &key, &data, MDBX_NEXT)) == 0)
      freecount += *(pgno_t *)data.iov_base;
    if (unlikely(rc != MDBX_NOTFOUND))
      goto finish;

    freecount += txn->txn_aht_array[MDBX_GACO_AAH].aa.branch_pages +
                 txn->txn_aht_array[MDBX_GACO_AAH].aa.leaf_pages +
                 txn->txn_aht_array[MDBX_GACO_AAH].aa.overflow_pages;

    new_root = txn->mt_next_pgno - 1 - freecount;
    meta->mp_meta.mm_geo.next = meta->mp_meta.mm_geo.now = new_root + 1;
    aa_txn2db(&txn->txn_aht_array[MDBX_MAIN_AAH],
              &meta->mp_meta.mm_aas[MDBX_MAIN_AAH]);
    meta->mp_meta.mm_aas[MDBX_MAIN_AAH].aa_root = new_root;
  } else {
    /* When the AA is empty, handle it specially to
     * fix any breakage like page leaks from ITS#8174. */
    meta->mp_meta.mm_aas[MDBX_MAIN_AAH].aa_flags16 =
        txn->txn_aht_array[MDBX_MAIN_AAH].aa.flags16;
  }

  /* copy canary sequenses if present */
  if (txn->mt_canary.v) {
    meta->mp_meta.mm_canary = txn->mt_canary;
    meta->mp_meta.mm_canary.v = meta_txnid_stable(bk, &meta->mp_meta);
  }

  /* update signature */
  meta->mp_meta.mm_datasync_sign = meta_sign(&meta->mp_meta);

  my.mc_wlen[0] = pgno2bytes(bk, NUM_METAS);
  my.mc_txn = txn;
  rc = bk_copy_walk(&my, &root, 0);
  if (rc == MDBX_SUCCESS && root != new_root) {
    mdbx_error("unexpected root %" PRIaPGNO " (%" PRIaPGNO ")", root, new_root);
    rc = MDBX_PROBLEM; /* page leak or corrupt databook */
  }

finish:
  if (rc != MDBX_SUCCESS)
    my.mc_error = rc;
  mdbx_bk_cthr_toggle(&my, 1 | MDBX_EOF);
  rc = mdbx_thread_join(thr);
  mdbx_tn_abort(txn);

done:
  mdbx_memalign_free(my.mc_wbuf[0]);
  mdbx_condmutex_destroy(&my.mc_condmutex);
  return rc ? rc : my.mc_error;
}

/* Copy databook as-is. */
static int __cold bk_copy_asis(MDBX_milieu *bk, MDBX_filehandle_t fd) {
  MDBX_txn *txn = nullptr;

  /* Do the lock/unlock of the reader mutex before starting the
   * write txn.  Otherwise other read txns could block writers. */
  int rc = mdbx_tn_begin(bk, nullptr, MDBX_RDONLY, &txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* We must start the actual read txn after blocking writers */
  rc = txn_end(txn, MDBX_END_RESET_TMP);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout; /* FIXME: or just return? */

  /* Temporarily block writers until we snapshot the meta pages */
  rc = mdbx_tn_lock(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  rc = txn_renew0(txn, MDBX_RDONLY);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_tn_unlock(bk);
    goto bailout;
  }

  rc = mdbx_write(fd, bk->me_map, pgno2bytes(bk, NUM_METAS));
  meta_t *const head = meta_head(bk);
  const uint64_t size =
      mdbx_roundup2(pgno2bytes(bk, head->mm_geo.now), bk->me_os_psize);
  mdbx_tn_unlock(bk);

  if (likely(rc == MDBX_SUCCESS))
    rc = mdbx_write(fd, bk->me_map + pgno2bytes(bk, NUM_METAS),
                    pgno2bytes(bk, txn->mt_next_pgno - NUM_METAS));

  if (likely(rc == MDBX_SUCCESS))
    rc = mdbx_ftruncate(fd, size);

bailout:
  mdbx_tn_abort(txn);
  return rc;
}

int __cold mdbx_bk_set_flags(MDBX_milieu *bk, unsigned flags, int onoff) {
  if (unlikely(flags & ~MDBX_REGIME_CHANGEABLE))
    return MDBX_EINVAL;

  int rc = mdbx_tn_lock(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (onoff)
    bk->me_flags32 |= flags;
  else
    bk->me_flags32 &= ~flags;

  mdbx_tn_unlock(bk);
  return MDBX_SUCCESS;
}

/* Depth-first tree traversal. */
static int __cold do_walk(walk_ctx_t *ctx, const char *aah, pgno_t pg,
                          int deep) {
  page_t *mp;
  int rc, i, nkeys;
  size_t header_size, unused_size, payload_size, align_bytes;
  const char *type;

  if (pg == P_INVALID)
    return MDBX_SUCCESS; /* empty db */

  MDBX_cursor mc;
  memset(&mc, 0, sizeof(mc));
  mc.primal.mc_snum = 1;
  mc.primal.mc_txn = ctx->mw_txn;

  rc = page_get(ctx->mw_txn, pg, &mp, nullptr);
  if (rc)
    return rc;
  if (pg != mp->mp_pgno)
    return MDBX_CORRUPTED;

  nkeys = page_numkeys(mp);
  header_size = IS_DFL(mp) ? PAGEHDRSZ : PAGEHDRSZ + mp->mp_lower;
  unused_size = page_spaceleft(mp);
  payload_size = 0;

  /* LY: Don't use mask here, e.g bitwise
   * (P_BRANCH|P_LEAF|P_DFL|P_META|P_OVERFLOW|P_SUBP).
   * Pages should not me marked dirty/loose or otherwise. */
  switch (mp->mp_flags16) {
  case P_BRANCH:
    type = "branch";
    if (nkeys < 1)
      return MDBX_CORRUPTED;
    break;
  case P_LEAF:
    type = "leaf";
    break;
  case P_LEAF | P_SUBP:
    type = "dupsort-subleaf";
    break;
  case P_LEAF | P_DFL:
    type = "dupfixed-leaf";
    break;
  case P_LEAF | P_DFL | P_SUBP:
    type = "dupsort-dupfixed-subleaf";
    break;
  case P_META:
  case P_OVERFLOW:
  default:
    return MDBX_CORRUPTED;
  }

  for (align_bytes = i = 0; i < nkeys;
       align_bytes += ((payload_size + align_bytes) & 1), i++) {
    node_t *node;

    if (IS_DFL(mp)) {
      /* DFL pages have no mp_ptrs[] or node headers */
      payload_size += mp->mp_leaf2_ksize16;
      continue;
    }

    node = node_ptr(mp, i);
    payload_size += NODESIZE + node->mn_ksize16;

    if (IS_BRANCH(mp)) {
      rc = do_walk(ctx, aah, node_get_pgno(node), deep);
      if (rc)
        return rc;
      continue;
    }

    assert(IS_LEAF(mp));
    if (unllikely(node->node_flags8 & NODE_BIG)) {
      page_t *omp;
      pgno_t *opg;
      size_t over_header, over_payload, over_unused;

      payload_size += sizeof(pgno_t);
      opg = NODEDATA(node);
      rc = page_get(ctx->mw_txn, *opg, &omp, nullptr);
      if (rc)
        return rc;
      if (*opg != omp->mp_pgno)
        return MDBX_CORRUPTED;
      /* LY: Don't use mask here, e.g bitwise
       * (P_BRANCH|P_LEAF|P_DFL|P_META|P_OVERFLOW|P_SUBP).
       * Pages should not me marked dirty/loose or otherwise. */
      if (P_OVERFLOW != omp->mp_flags16)
        return MDBX_CORRUPTED;

      over_header = PAGEHDRSZ;
      over_payload = node_get_datasize(node);
      over_unused = pgno2bytes(ctx->mw_txn->mt_book, omp->mp_pages) -
                    over_payload - over_header;

      rc = ctx->mw_visitor(*opg, omp->mp_pages, ctx->mw_user, aah,
                           "overflow-data", 1, over_payload, over_header,
                           over_unused);
      if (rc)
        return rc;
      continue;
    }

    payload_size += node_get_datasize(node);
    if (node->node_flags8 & NODE_SUBTREE) {
      aatree_t *db = NODEDATA(node);
      char *name = nullptr;

      if (!(node->node_flags8 & NODE_DUP)) {
        name = NODEKEY(node);
        ptrdiff_t namelen = (char *)db - name;
        name = memcpy(alloca(namelen + 1), name, namelen);
        name[namelen] = 0;
      }
      rc = do_walk(ctx, (name && name[0]) ? name : aah, db->aa_root, deep + 1);
      if (rc)
        return rc;
    }
  }

  return ctx->mw_visitor(mp->mp_pgno, 1, ctx->mw_user, aah, type, nkeys,
                         payload_size, header_size, unused_size + align_bytes);
}
