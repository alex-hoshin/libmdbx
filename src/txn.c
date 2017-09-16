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

static inline int validate_txn(const MDBX_txn *txn) {
  if (unlikely(!txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  if (unlikely(txn->mt_flags & MDBX_TXN_BLOCKED))
    return MDBX_BAD_TXN;

  return validate_bk(txn->mt_book);
}

static inline int validate_txn_more4rw(MDBX_txn *txn) {
  if (unlikely(txn->mt_flags & MDBX_RDONLY))
    return MDBX_EACCESS;

  return MDBX_SUCCESS;
}

static inline int validate_txn_rw(MDBX_txn *txn) {
  int err = validate_txn(txn);
  if (likely(err == MDBX_SUCCESS))
    err = validate_txn_more4rw(txn);

  return err;
}

//-----------------------------------------------------------------------------

/* Common code for mdbx_tn_begin() and mdbx_tn_renew(). */
static int txn_renew0(MDBX_txn *txn, unsigned flags) {
  MDBX_milieu *bk = txn->mt_book;
  int rc;

  if (unlikely(bk->me_pid != mdbx_getpid())) {
    bk->me_flags32 |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }

  pgno_t upper_pgno = 0;
  if (flags & MDBX_RDONLY) {
    txn->mt_flags = MDBX_RDONLY;
    MDBX_reader *r = txn->mt_ro_reader;
    if (likely(bk->me_flags32 & MDBX_ENV_TXKEY)) {
      mdbx_assert(bk, !(bk->me_flags32 & MDBX_NOTLS));
      r = mdbx_thread_rthc_get(bk->me_txkey);
      if (likely(r)) {
        mdbx_assert(bk, r->mr_pid == bk->me_pid);
        mdbx_assert(bk, r->mr_tid == mdbx_thread_self());
      }
    } else {
      mdbx_assert(bk, !bk->me_lck || (bk->me_flags32 & MDBX_NOTLS));
    }

    if (likely(r)) {
      if (unlikely(r->mr_pid != bk->me_pid || r->mr_txnid != ~(txnid_t)0))
        return MDBX_BAD_RSLOT;
    } else if (bk->me_lck) {
      unsigned slot, nreaders;
      const MDBX_pid_t pid = bk->me_pid;
      const MDBX_tid_t tid = mdbx_thread_self();
      mdbx_assert(bk, bk->me_lck->li_magic_and_version == MDBX_LOCK_MAGIC);
      mdbx_assert(bk, bk->me_lck->li_os_and_format == MDBX_LOCK_FORMAT);

      rc = mdbx_rdt_lock(bk);
      if (unlikely(MDBX_IS_ERROR(rc)))
        return rc;
      rc = MDBX_SUCCESS;

      if (unlikely(bk->me_live_reader != pid)) {
        rc = mdbx_rpid_set(bk);
        if (unlikely(rc != MDBX_SUCCESS)) {
          mdbx_rdt_unlock(bk);
          return rc;
        }
        bk->me_live_reader = pid;
      }

      while (1) {
        nreaders = bk->me_lck->li_numreaders;
        for (slot = 0; slot < nreaders; slot++)
          if (bk->me_lck->li_readers[slot].mr_pid == 0)
            break;

        if (likely(slot < bk->me_maxreaders))
          break;

        rc = reader_check(bk, true, nullptr);
        if (rc != MDBX_RESULT_TRUE) {
          mdbx_rdt_unlock(bk);
          return (rc == MDBX_SUCCESS) ? MDBX_READERS_FULL : rc;
        }
      }

      STATIC_ASSERT(sizeof(MDBX_reader) == MDBX_CACHELINE_SIZE);
#ifdef MDBX_OSAL_LOCK
      STATIC_ASSERT(offsetof(MDBX_lockinfo, li_wmutex) % MDBX_CACHELINE_SIZE ==
                    0);
#else
      STATIC_ASSERT(offsetof(MDBX_lockinfo, li_oldest) % MDBX_CACHELINE_SIZE ==
                    0);
#endif
#ifdef MDBX_OSAL_LOCK
      STATIC_ASSERT(offsetof(MDBX_lockinfo, li_rmutex) % MDBX_CACHELINE_SIZE ==
                    0);
#else
      STATIC_ASSERT(
          offsetof(MDBX_lockinfo, li_numreaders) % MDBX_CACHELINE_SIZE == 0);
#endif
      STATIC_ASSERT(offsetof(MDBX_lockinfo, li_readers) % MDBX_CACHELINE_SIZE ==
                    0);
      r = &bk->me_lck->li_readers[slot];
      /* Claim the reader slot, carefully since other code
       * uses the reader table un-mutexed: First reset the
       * slot, next publish it in mtb.li_numreaders.  After
       * that, it is safe for mdbx_bk_close() to touch it.
       * When it will be closed, we can finally claim it. */
      r->mr_pid = 0;
      r->mr_txnid = ~(txnid_t)0;
      r->mr_tid = tid;
      mdbx_coherent_barrier();
      if (slot == nreaders)
        bk->me_lck->li_numreaders = ++nreaders;
      if (bk->me_close_readers < nreaders)
        bk->me_close_readers = nreaders;
      r->mr_pid = pid;
      mdbx_rdt_unlock(bk);

      if (likely(bk->me_flags32 & MDBX_ENV_TXKEY))
        mdbx_thread_rthc_set(bk->me_txkey, r);
    }

    while (1) {
      meta_t *const meta = meta_head(bk);
      jitter4testing(false);
      const txnid_t snap = meta_txnid_fluid(bk, meta);
      jitter4testing(false);
      if (r) {
        r->mr_txnid = snap;
        jitter4testing(false);
        mdbx_assert(bk, r->mr_pid == mdbx_getpid());
        mdbx_assert(bk, r->mr_tid == mdbx_thread_self());
        mdbx_assert(bk, r->mr_txnid == snap);
        mdbx_coherent_barrier();
      }
      jitter4testing(true);

      /* Snap the state from current meta-head */
      txn->mt_txnid = snap;
      txn->mt_next_pgno = meta->mm_geo.next;
      txn->mt_end_pgno = meta->mm_geo.now;
      upper_pgno = meta->mm_geo.upper;
      rc = aa_db2txn(&meta->mm_aas[MDBX_GACO_AAH],
                     &txn->txn_aht_array[MDBX_GACO_AAH]);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
      rc = aa_db2txn(&meta->mm_aas[MDBX_MAIN_AAH],
                     &txn->txn_aht_array[MDBX_MAIN_AAH]);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
      txn->mt_canary = meta->mm_canary;

      /* LY: Retry on a race, ITS#7970. */
      mdbx_compiler_barrier();
      if (likely(meta == meta_head(bk) && snap == meta_txnid_fluid(bk, meta) &&
                 snap >= bk->me_oldest[0])) {
        jitter4testing(false);
        break;
      }
      if (bk->me_lck)
        bk->me_lck->li_reader_finished_flag = true;
    }

    if (unlikely(txn->mt_txnid == 0)) {
      mdbx_error("databook corrupted by died writer, must shutdown!");
      rc = MDBX_WANNA_RECOVERY;
      goto bailout;
    }
    mdbx_assert(bk, txn->mt_txnid >= *bk->me_oldest);
    txn->mt_ro_reader = r;
  } else {
    /* Not yet touching txn == bk->me_txn0, it may be active */
    jitter4testing(false);
    rc = mdbx_tn_lock(bk);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;

    jitter4testing(false);
    meta_t *meta = meta_head(bk);
    jitter4testing(false);
    txn->mt_canary = meta->mm_canary;
    const txnid_t snap = meta_txnid_stable(bk, meta);
    txn->mt_txnid = snap + 1;
    if (unlikely(txn->mt_txnid < snap)) {
      mdbx_debug("txnid overflow!");
      rc = MDBX_TXN_FULL;
      goto bailout;
    }

    txn->mt_flags = flags;
    txn->mt_child = nullptr;
    txn->mt_loose_pages = nullptr;
    txn->mt_loose_count = 0;
    txn->mt_dirtyroom = MDBX_PNL_UM_MAX;
    txn->mt_rw_dirtylist = bk->me_dirtylist;
    txn->mt_rw_dirtylist[0].mid = 0;
    txn->mt_befree_pages = bk->me_free_pgs;
    txn->mt_befree_pages[0] = 0;
    txn->mt_spill_pages = nullptr;
    if (txn->mt_lifo_reclaimed)
      txn->mt_lifo_reclaimed[0] = 0;
    bk->me_current_txn = txn;

    /* Snap core AA records */
    rc = aa_db2txn(&meta->mm_aas[MDBX_GACO_AAH],
                   &txn->txn_aht_array[MDBX_GACO_AAH]);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
    rc = aa_db2txn(&meta->mm_aas[MDBX_MAIN_AAH],
                   &txn->txn_aht_array[MDBX_MAIN_AAH]);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
    txn->mt_next_pgno = meta->mm_geo.next;
    txn->mt_end_pgno = meta->mm_geo.now;
    upper_pgno = meta->mm_geo.upper;
  }

  /* setup basic AA-handles */
  txn->txn_aht_array[MDBX_GACO_AAH].ahe = &bk->env_ahe_array[MDBX_GACO_AAH];
  txn->txn_aht_array[MDBX_GACO_AAH].ah.seq16 =
      0 /* bk->env_ahe_array[MDBX_GACO_AAH].ax_seqaah16 */;
  txn->txn_aht_array[MDBX_GACO_AAH].ah.kind_and_state16 =
      MDBX_AAH_VALID | MDBX_AAH_GACO;
  txn->txn_aht_array[MDBX_MAIN_AAH].ahe = &bk->env_ahe_array[MDBX_MAIN_AAH];
  txn->txn_aht_array[MDBX_MAIN_AAH].ah.seq16 =
      0 /* bk->env_ahe_array[MDBX_MAIN_AAH].ax_seqaah16 */;
  txn->txn_aht_array[MDBX_MAIN_AAH].ah.kind_and_state16 =
      MDBX_AAH_VALID | MDBX_AAH_MAIN;
  txn->txn_ah_num = CORE_AAH;

  if (unlikely(bk->me_flags32 & MDBX_FATAL_ERROR)) {
    mdbx_warning("databook had fatal error, must shutdown!");
    rc = MDBX_PANIC;
  } else {
    const size_t size = pgno2bytes(bk, txn->mt_end_pgno);
    if (unlikely(size > bk->me_mapsize)) {
      if (upper_pgno > MAX_PAGENO ||
          bytes2pgno(bk, pgno2bytes(bk, upper_pgno)) != upper_pgno) {
        rc = MDBX_MAP_RESIZED;
        goto bailout;
      }
      rc = mdbx_mapresize(bk, txn->mt_end_pgno, upper_pgno);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    }
    return MDBX_SUCCESS;
  }
bailout:
  assert(rc != MDBX_SUCCESS);
  txn_end(txn, MDBX_END_SLOT | MDBX_END_FAIL_BEGIN);
  return rc;
}

int mdbx_tn_renew(MDBX_txn *txn) {
  int rc;

  if (unlikely(!txn))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  if (unlikely(!F_ISSET(txn->mt_flags, MDBX_RDONLY | MDBX_TXN_FINISHED)))
    return MDBX_EINVAL;

  rc = txn_renew0(txn, MDBX_RDONLY);
  if (rc == MDBX_SUCCESS) {
    mdbx_debug("renew txn %" PRIaTXN "%c %p on bk %p, root page %" PRIaPGNO
               "/%" PRIaPGNO,
               txn->mt_txnid, (txn->mt_flags & MDBX_RDONLY) ? 'r' : 'w',
               (void *)txn, (void *)txn->mt_book, aht_main(txn)->aa.root,
               aht_gaco(txn)->aa.root);
  }
  return rc;
}

int mdbx_tn_begin(MDBX_milieu *bk, MDBX_txn *parent, unsigned flags,
                  MDBX_txn **ret) {
  MDBX_txn *txn;
  int rc, txn_extra_size, txn_body_size;

  if (unlikely(!bk || !ret))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(bk->me_pid != mdbx_getpid())) {
    bk->me_flags32 |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }

  if (unlikely(!bk->me_map))
    return MDBX_EPERM;

  flags &= MDBX_TXN_BEGIN_FLAGS;
  flags |= bk->me_flags32 & MDBX_WRITEMAP;

  if (unlikely(bk->me_flags32 & MDBX_RDONLY &
               ~flags)) /* write txn in RDONLY bk */
    return MDBX_EACCESS;

  if (parent) {
    if (unlikely(parent->mt_signature != MDBX_MT_SIGNATURE))
      return MDBX_EINVAL;

    if (unlikely(parent->mt_owner != mdbx_thread_self()))
      return MDBX_THREAD_MISMATCH;

    /* Nested transactions: Max 1 child, write txns only, no writemap */
    flags |= parent->mt_flags;
    if (unlikely(flags & (MDBX_RDONLY | MDBX_WRITEMAP | MDBX_TXN_BLOCKED))) {
      return (parent->mt_flags & MDBX_RDONLY) ? MDBX_EINVAL : MDBX_BAD_TXN;
    }
    txn_body_size = sizeof(nested_txn);
    /* Child txns save pgstate_t and use own copy of cursors */
    txn_extra_size = bk->env_ah_max * (sizeof(aht_t) + sizeof(MDBX_cursor *));
  } else if (flags & MDBX_RDONLY) {
    txn_body_size = sizeof(MDBX_txn);
    txn_extra_size = bk->env_ah_max * sizeof(aht_t);
  } else {
    /* Reuse preallocated write txn. However, do not touch it until
     * txn_renew0() succeeds, since it currently may be active. */
    txn = bk->me_wpa_txn;
    goto renew;
  }

  txn = calloc(1, txn_extra_size + txn_body_size);
  if (unlikely(txn == nullptr)) {
    mdbx_debug("calloc: %s", "failed");
    return MDBX_ENOMEM;
  }
  txn->txn_aht_array = (aht_t *)((char *)txn + txn_body_size);
  txn->mt_flags = flags;
  txn->mt_book = bk;

  if (parent) {
    unsigned i;
    txn->mt_cursors = (MDBX_cursor **)(txn->txn_aht_array + bk->env_ah_max);
    txn->mt_rw_dirtylist = malloc(sizeof(MDBX_ID2) * MDBX_PNL_UM_SIZE);
    if (!txn->mt_rw_dirtylist ||
        !(txn->mt_befree_pages = mdbx_pnl_alloc(MDBX_PNL_UM_MAX))) {
      free(txn->mt_rw_dirtylist);
      free(txn);
      return MDBX_ENOMEM;
    }
    txn->mt_txnid = parent->mt_txnid;
    txn->mt_dirtyroom = parent->mt_dirtyroom;
    txn->mt_rw_dirtylist[0].mid = 0;
    txn->mt_spill_pages = nullptr;
    txn->mt_next_pgno = parent->mt_next_pgno;
    txn->mt_end_pgno = parent->mt_end_pgno;
    parent->mt_flags |= MDBX_TXN_HAS_CHILD;
    parent->mt_child = txn;
    txn->mt_parent = parent;
    txn->txn_ah_num = parent->txn_ah_num;
    memcpy(txn->txn_aht_array, parent->txn_aht_array,
           txn->txn_ah_num * sizeof(aht_t));
    /* Copy parent's mt_aah_flags, but clear MDBX_AAH_FRESH */
    for (i = 0; i < txn->txn_ah_num; ++i)
      txn->txn_aht_array[i].ah.state8 &= ~MDBX_AAH_CREATED;

    nested_txn *ntxn = (nested_txn *)txn;
    ntxn->mnt_pgstate = bk->me_pgstate /* save parent reclaimed state */;
    rc = MDBX_SUCCESS;
    if (bk->me_reclaimed_pglist) {
      MDBX_PNL list_copy = mdbx_pnl_alloc(bk->me_reclaimed_pglist[0]);
      if (unlikely(!list_copy))
        rc = MDBX_ENOMEM;
      else {
        size_t bytes = MDBX_PNL_SIZEOF(bk->me_reclaimed_pglist);
        bk->me_reclaimed_pglist =
            memcpy(list_copy, ntxn->mnt_pgstate.mf_reclaimed_pglist, bytes);
      }
    }
    if (likely(rc == MDBX_SUCCESS))
      rc = txn_shadow_cursors(parent, txn);
    if (unlikely(rc != MDBX_SUCCESS))
      txn_end(txn, MDBX_END_FAIL_BEGINCHILD);
  } else { /* MDBX_RDONLY */
  renew:
    rc = txn_renew0(txn, flags);
  }

  if (unlikely(rc != MDBX_SUCCESS)) {
    if (txn != bk->me_wpa_txn)
      free(txn);
  } else {
    txn->mt_owner = mdbx_thread_self();
    txn->mt_signature = MDBX_MT_SIGNATURE;
    *ret = txn;
    mdbx_debug("begin txn %" PRIaTXN "%c %p on bk %p, root page %" PRIaPGNO
               "/%" PRIaPGNO,
               txn->mt_txnid, (flags & MDBX_RDONLY) ? 'r' : 'w', (void *)txn,
               (void *)bk, aht_main(txn)->aa.root, aht_gaco(txn)->aa.root);
  }

  return rc;
}

/* End a transaction, except successful commit of a nested transaction.
 * May be called twice for readonly txns: First reset it, then abort.
 * [in] txn   the transaction handle to end
 * [in] mode  why and how to end the transaction */
static int txn_end(MDBX_txn *txn, unsigned mode) {
  MDBX_milieu *bk = txn->mt_book;
  static const char *const names[] = MDBX_END_NAMES;

  if (unlikely(txn->mt_book->me_pid != mdbx_getpid())) {
    bk->me_flags32 |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }

  /* Export or close AAH handles opened in this txn */
  int rc = aa_return(txn, mode);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  mdbx_debug("%s txn %" PRIaTXN "%c %p on mdbenv %p, root page %" PRIaPGNO
             "/%" PRIaPGNO,
             names[mode & MDBX_END_OPMASK], txn->mt_txnid,
             (txn->mt_flags & MDBX_RDONLY) ? 'r' : 'w', (void *)txn, (void *)bk,
             aht_main(txn)->aa.root, aht_gaco(txn)->aa.root);

  if (F_ISSET(txn->mt_flags, MDBX_RDONLY)) {
    if (txn->mt_ro_reader) {
      txn->mt_ro_reader->mr_txnid = ~(txnid_t)0;
      bk->me_lck->li_reader_finished_flag = true;
      if (mode & MDBX_END_SLOT) {
        if ((bk->me_flags32 & MDBX_ENV_TXKEY) == 0)
          txn->mt_ro_reader->mr_pid = 0;
        txn->mt_ro_reader = nullptr;
      }
    }
    mdbx_coherent_barrier();
    txn->txn_ah_num = 0; /* prevent further AAH activity */
    txn->mt_flags |= MDBX_TXN_FINISHED;
    txn->mt_owner = 0;
  } else if (!F_ISSET(txn->mt_flags, MDBX_TXN_FINISHED)) {
    pgno_t *pghead = bk->me_reclaimed_pglist;

    if (!(mode & MDBX_END_EOTDONE)) /* !(already closed cursors) */
      cursors_eot(txn, 0);
    if (!(bk->me_flags32 & MDBX_WRITEMAP)) {
      dlist_free(txn);
    }

    if (txn->mt_lifo_reclaimed) {
      txn->mt_lifo_reclaimed[0] = 0;
      if (txn != bk->me_wpa_txn) {
        mdbx_txl_free(txn->mt_lifo_reclaimed);
        txn->mt_lifo_reclaimed = nullptr;
      }
    }
    txn->txn_ah_num = 0;
    txn->mt_flags = MDBX_TXN_FINISHED;

    if (!txn->mt_parent) {
      mdbx_pnl_shrink(&txn->mt_befree_pages);
      bk->me_free_pgs = txn->mt_befree_pages;
      /* me_pgstate: */
      bk->me_reclaimed_pglist = nullptr;
      bk->me_last_reclaimed = 0;

      bk->me_current_txn = nullptr;
      txn->mt_owner = 0;
      txn->mt_signature = 0;
      mode = 0; /* txn == bk->me_txn0, do not free() it */

      /* The writer mutex was locked in mdbx_tn_begin. */
      mdbx_tn_unlock(bk);
    } else {
      txn->mt_parent->mt_child = nullptr;
      txn->mt_parent->mt_flags &= ~MDBX_TXN_HAS_CHILD;
      bk->me_pgstate = ((nested_txn *)txn)->mnt_pgstate;
      mdbx_pnl_free(txn->mt_befree_pages);
      mdbx_pnl_free(txn->mt_spill_pages);
      free(txn->mt_rw_dirtylist);
    }

    mdbx_pnl_free(pghead);
  }

  if (mode & MDBX_END_FREE) {
    mdbx_ensure(bk, txn != bk->me_wpa_txn);
    txn->mt_owner = 0;
    txn->mt_signature = 0;
    free(txn);
  }

  return MDBX_SUCCESS;
}

int mdbx_tn_commit(MDBX_txn *txn) {
  int rc;

  if (unlikely(txn == nullptr))
    return MDBX_EINVAL;

  if (unlikely(txn->mt_signature != MDBX_MT_SIGNATURE))
    return MDBX_EBADSIGN;

  if (unlikely(txn->mt_owner != mdbx_thread_self()))
    return MDBX_THREAD_MISMATCH;

  MDBX_milieu *bk = txn->mt_book;
  if (unlikely(bk->me_pid != mdbx_getpid())) {
    bk->me_flags32 |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }

  if (txn->mt_child) {
    rc = mdbx_tn_commit(txn->mt_child);
    txn->mt_child = nullptr;
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
  }

  /* txn_end() mode for a commit which writes nothing */
  unsigned end_mode = MDBX_END_EMPTY_COMMIT | MDBX_END_SLOT | MDBX_END_FREE;
  if (unlikely(F_ISSET(txn->mt_flags, MDBX_RDONLY)))
    goto done;

  if (unlikely(txn->mt_flags & (MDBX_TXN_FINISHED | MDBX_TXN_ERROR))) {
    mdbx_debug("error flag is set, can't commit");
    if (txn->mt_parent)
      txn->mt_parent->mt_flags |= MDBX_TXN_ERROR;
    rc = MDBX_BAD_TXN;
    goto fail;
  }

  if (txn->mt_parent) {
    MDBX_txn *parent = txn->mt_parent;
    page_t **lp;
    MDBX_ID2L dst, src;
    MDBX_PNL pspill;
    unsigned i, x, y, len, ps_len;

    /* Append our reclaim list to parent's */
    if (txn->mt_lifo_reclaimed) {
      if (parent->mt_lifo_reclaimed) {
        rc = mdbx_txl_append_list(&parent->mt_lifo_reclaimed,
                                  txn->mt_lifo_reclaimed);
        if (unlikely(rc != MDBX_SUCCESS))
          goto fail;
        mdbx_txl_free(txn->mt_lifo_reclaimed);
      } else
        parent->mt_lifo_reclaimed = txn->mt_lifo_reclaimed;
      txn->mt_lifo_reclaimed = nullptr;
    }

    /* Append our free list to parent's */
    rc = mdbx_pnl_append_list(&parent->mt_befree_pages, txn->mt_befree_pages);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
    mdbx_pnl_free(txn->mt_befree_pages);
    /* Failures after this must either undo the changes
     * to the parent or set MDBX_TXN_ERROR in the parent. */

    parent->mt_next_pgno = txn->mt_next_pgno;
    parent->mt_end_pgno = txn->mt_end_pgno;
    parent->mt_flags = txn->mt_flags;

    /* Merge our cursors into parent's and close them */
    cursors_eot(txn, 1);

    /* Update parent's AA table. */
    memcpy(parent->txn_aht_array, txn->txn_aht_array,
           txn->txn_ah_num * sizeof(aht_t));
    parent->txn_ah_num = txn->txn_ah_num;

    aht_t *const end = &parent->txn_aht_array[parent->txn_ah_num];
    const ptrdiff_t parent2child = txn->txn_aht_array - parent->txn_aht_array;
    for (aht_t *aht = &parent->txn_aht_array[CORE_AAH]; aht < end; ++aht) {
      /* preserve parent's MDBX_AAH_CREATED status */
      aht->ah.state8 =
          (aht->ah.state8 & MDBX_AAH_CREATED) | aht[parent2child].ah.state8;
    }
    rc = aa_return(txn, end_mode);

    dst = parent->mt_rw_dirtylist;
    src = txn->mt_rw_dirtylist;
    /* Remove anything in our dirty list from parent's spill list */
    if ((pspill = parent->mt_spill_pages) && (ps_len = pspill[0])) {
      x = y = ps_len;
      pspill[0] = (pgno_t)-1;
      /* Mark our dirty pages as deleted in parent spill list */
      for (i = 0, len = src[0].mid; ++i <= len;) {
        pgno_t pn = src[i].mid << 1;
        while (pn > pspill[x])
          x--;
        if (pn == pspill[x]) {
          pspill[x] = 1;
          y = --x;
        }
      }
      /* Squash deleted pagenums if we deleted any */
      for (x = y; ++x <= ps_len;)
        if (!(pspill[x] & 1))
          pspill[++y] = pspill[x];
      pspill[0] = y;
    }

    /* Remove anything in our spill list from parent's dirty list */
    if (txn->mt_spill_pages && txn->mt_spill_pages[0]) {
      for (i = 1; i <= txn->mt_spill_pages[0]; i++) {
        pgno_t pn = txn->mt_spill_pages[i];
        if (pn & 1)
          continue; /* deleted spillpg */
        pn >>= 1;
        y = mdbx_mid2l_search(dst, pn);
        if (y <= dst[0].mid && dst[y].mid == pn) {
          free(dst[y].mptr);
          while (y < dst[0].mid) {
            dst[y] = dst[y + 1];
            y++;
          }
          dst[0].mid--;
        }
      }
    }

    /* Find len = length of merging our dirty list with parent's */
    x = dst[0].mid;
    dst[0].mid = 0; /* simplify loops */
    if (parent->mt_parent) {
      len = x + src[0].mid;
      y = mdbx_mid2l_search(src, dst[x].mid + 1) - 1;
      for (i = x; y && i; y--) {
        pgno_t yp = src[y].mid;
        while (yp < dst[i].mid)
          i--;
        if (yp == dst[i].mid) {
          i--;
          len--;
        }
      }
    } else { /* Simplify the above for single-ancestor case */
      len = MDBX_PNL_UM_MAX - txn->mt_dirtyroom;
    }
    /* Merge our dirty list with parent's */
    y = src[0].mid;
    for (i = len; y; dst[i--] = src[y--]) {
      pgno_t yp = src[y].mid;
      while (yp < dst[x].mid)
        dst[i--] = dst[x--];
      if (yp == dst[x].mid)
        free(dst[x--].mptr);
    }
    assert(i == x);
    dst[0].mid = len;
    free(txn->mt_rw_dirtylist);
    parent->mt_dirtyroom = txn->mt_dirtyroom;
    if (txn->mt_spill_pages) {
      if (parent->mt_spill_pages) {
        /* TODO: Prevent failure here, so parent does not fail */
        rc = mdbx_pnl_append_list(&parent->mt_spill_pages, txn->mt_spill_pages);
        if (unlikely(rc != MDBX_SUCCESS))
          parent->mt_flags |= MDBX_TXN_ERROR;
        mdbx_pnl_free(txn->mt_spill_pages);
        mdbx_pnl_sort(parent->mt_spill_pages);
      } else {
        parent->mt_spill_pages = txn->mt_spill_pages;
      }
    }

    /* Append our loose page list to parent's */
    for (lp = &parent->mt_loose_pages; *lp; lp = &NEXT_LOOSE_PAGE(*lp))
      ;
    *lp = txn->mt_loose_pages;
    parent->mt_loose_count += txn->mt_loose_count;

    parent->mt_child = nullptr;
    mdbx_pnl_free(((nested_txn *)txn)->mnt_pgstate.mf_reclaimed_pglist);
    txn->mt_signature = 0;
    free(txn);
    return rc;
  }

  if (unlikely(txn != bk->me_current_txn)) {
    mdbx_debug("attempt to commit unknown transaction");
    rc = MDBX_EINVAL;
    goto fail;
  }

  cursors_eot(txn, 0);
  end_mode |= MDBX_END_EOTDONE | MDBX_END_FIXUPAAH;

  if (!txn->mt_rw_dirtylist[0].mid &&
      !(txn->mt_flags & (MDBX_TXN_DIRTY | MDBX_TXN_SPILLS)))
    goto done;

  mdbx_debug("committing txn %" PRIaTXN " %p on mdbenv %p, root page %" PRIaPGNO
             "/%" PRIaPGNO,
             txn->mt_txnid, (void *)txn, (void *)bk, aht_main(txn)->aa.root,
             aht_gaco(txn)->aa.root);

  /* Update AA root pointers */
  if (txn->txn_ah_num > CORE_AAH) {
    MDBX_cursor mc;
    rc = cursor_init(&mc, txn, aht_main(txn));
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;

    aht_t *const end = &txn->txn_aht_array[txn->txn_ah_num];
    for (aht_t *aht = &txn->txn_aht_array[CORE_AAH]; aht < end; ++aht) {
      if (aht->ah.state8 & MDBX_AAH_DIRTY) {
        ahe_t *ahe = aht->ahe;
        if (unlikely(ahe->ax_seqaah16 != aht->ah.seq16 ||
                     ahe->ax_refcounter16 < 1)) {
          rc = MDBX_BAD_AAH;
          goto fail;
        }

        MDBX_iov data = {nullptr, sizeof(aatree_t)};
        rc = cursor_put(&mc.primal, &ahe->ax_ident, &data,
                        MDBX_IUD_RESERVE | NODE_SUBTREE);
        if (unlikely(rc != MDBX_SUCCESS))
          goto fail;
        aa_txn2db(aht, (aatree_t *)data.iov_base);
      }
    }
  }

  rc = freelist_save(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    goto fail;

  mdbx_pnl_free(bk->me_reclaimed_pglist);
  bk->me_reclaimed_pglist = nullptr;
  mdbx_pnl_shrink(&txn->mt_befree_pages);

  if (audit_enabled())
    audit(txn);

  rc = page_flush(txn, 0);
  if (likely(rc == MDBX_SUCCESS)) {
    meta_t meta, *head = meta_head(bk);

    meta.mm_magic_and_version = head->mm_magic_and_version;
    meta.mm_extra_flags16 = head->mm_extra_flags16;
    meta.mm_validator_id8 = head->mm_validator_id8;
    meta.mm_extra_pagehdr8 = head->mm_extra_pagehdr8;

    meta.mm_geo = head->mm_geo;
    meta.mm_geo.next = txn->mt_next_pgno;
    meta.mm_geo.now = txn->mt_end_pgno;
    aa_txn2db(&txn->txn_aht_array[MDBX_GACO_AAH], &meta.mm_aas[MDBX_GACO_AAH]);
    aa_txn2db(&txn->txn_aht_array[MDBX_MAIN_AAH], &meta.mm_aas[MDBX_MAIN_AAH]);
    meta.mm_canary = txn->mt_canary;
    meta_set_txnid(bk, &meta, txn->mt_txnid);

    rc = mdbx_sync_locked(
        bk, bk->me_flags32 | txn->mt_flags | MDBX_SHRINK_ALLOWED, &meta);
  }
  if (unlikely(rc != MDBX_SUCCESS))
    goto fail;
  end_mode = MDBX_END_COMMITTED | MDBX_END_FIXUPAAH | MDBX_END_EOTDONE;

done:
  return txn_end(txn, end_mode);

fail:
  mdbx_tn_abort(txn);
  return rc;
}
