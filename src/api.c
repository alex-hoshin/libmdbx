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

//-----------------------------------------------------------------------------

int mdbx_cmp(MDBX_txn *txn, MDBX_aah aah, const MDBX_iov *a,
             const MDBX_iov *b) {
  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  if (unlikely(!a || !b))
    return MDBX_EINVAL;
  if (unlikely(a->iov_len && !a->iov_base))
    return MDBX_EINVAL;
  if (unlikely(b->iov_len && !b->iov_base))
    return MDBX_EINVAL;

  rc = rp.aht->ahe->ax_kcmp(*a, *b);
  if (rc < 0)
    return MDBX_RESULT_LT;
  if (rc > 0)
    return MDBX_RESULT_GT;
  return MDBX_RESULT_EQ;
}

int mdbx_dcmp(MDBX_txn *txn, MDBX_aah aah, const MDBX_iov *a,
              const MDBX_iov *b) {
  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  if (unlikely(!a || !b))
    return MDBX_EINVAL;
  if (unlikely(a->iov_len && !a->iov_base))
    return MDBX_EINVAL;
  if (unlikely(b->iov_len && !b->iov_base))
    return MDBX_EINVAL;

  rc = rp.aht->ahe->ax_dcmp(*a, *b);
  if (rc < 0)
    return MDBX_RESULT_LT;
  if (rc > 0)
    return MDBX_RESULT_GT;
  return MDBX_RESULT_EQ;
}

MDBX_iov mdbx_str2iov(const char *str) {
  MDBX_iov iov = {0, 0};
  if (likely(str)) {
    iov.iov_base = (void *)str;
    iov.iov_len = strlen(str);
  }
  return iov;
}

//-----------------------------------------------------------------------------

int __cold mdbx_aa_preopen(MDBX_milieu *bk, const MDBX_iov aa_ident,
                           unsigned flags, MDBX_aah *aah, MDBX_comparer *keycmp,
                           MDBX_comparer *datacmp) {
  if (unlikely(flags & ~MDBX_AA_FLAGS))
    return MDBX_EINVAL;

  if (unlikely(aa_ident.iov_base == nullptr && aa_ident.iov_len != 0))
    return MDBX_EINVAL;

  int rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_fastmutex_acquire(&bk->me_aah_lock, flags);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const ahe_rc_t rp = aa_open(bk, nullptr, aa_ident, flags, keycmp, datacmp);
  rc = rp.err;
  if (aah)
    *aah = rp.ahe ? bk_ahe2aah(bk, rp.ahe) : MDBX_INVALID_AAH;

  mdbx_ensure(bk, mdbx_fastmutex_release(&bk->me_aah_lock) == MDBX_SUCCESS);
  return rc;
}

int __cold mdbx_aa_open(MDBX_txn *txn, const MDBX_iov aa_ident, unsigned flags,
                        MDBX_aah *aah, MDBX_comparer *keycmp,
                        MDBX_comparer *datacmp) {
  if (unlikely(!aah || (flags & ~MDBX_OPEN_FLAGS) != 0))
    return MDBX_EINVAL;

  if (unlikely(aa_ident.iov_base == nullptr || aa_ident.iov_len == 0 ||
               aah == nullptr))
    return MDBX_EINVAL;

  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (flags & MDBX_CREATE) {
    rc = validate_txn_more4rw(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  MDBX_milieu *bk = txn->mt_book;
  rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_fastmutex_acquire(&bk->me_aah_lock, flags);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const ahe_rc_t rp = aa_open(bk, txn, aa_ident, flags, keycmp, datacmp);
  rc = rp.err;
  if (aah)
    *aah = rp.ahe ? bk_ahe2aah(bk, rp.ahe) : MDBX_INVALID_AAH;

  mdbx_ensure(bk, mdbx_fastmutex_release(&bk->me_aah_lock) == MDBX_SUCCESS);
  return rc;
}

int mdbx_aa_addref(MDBX_milieu *bk, MDBX_aah aah) {
  int rc = validate_bk_aah(bk, aah);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_fastmutex_acquire(&bk->me_aah_lock, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = aa_addref(bk, aah);
  mdbx_ensure(bk, mdbx_fastmutex_release(&bk->me_aah_lock) == MDBX_SUCCESS);
  return rc;
}

int mdbx_aa_close(MDBX_milieu *bk, MDBX_aah aah) {
  int rc = validate_bk_aah(bk, aah);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = mdbx_fastmutex_acquire(&bk->me_aah_lock, 0);
  if (likely(rc == MDBX_SUCCESS)) {
    rc = aa_close(bk, aah);
    mdbx_ensure(bk, mdbx_fastmutex_release(&bk->me_aah_lock) == MDBX_SUCCESS);
  }
  return rc;
}

int mdbx_aa_drop(MDBX_txn *txn, MDBX_aah aah, enum mdbx_drop_flags_t flags) {
  if (unlikely((MDBX_PURGE_AA | MDBX_DELETE_AA | MDBX_CLOSE_HANDLE) <
               (unsigned)flags))
    return MDBX_EINVAL;

  int rc = validate_txn_rw(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  rc = aa_drop(txn, flags, rp.aht);
  if (likely(rc == MDBX_SUCCESS) && (flags & MDBX_CLOSE_HANDLE)) {
    rp.aht->ah.kind_and_state16 &= ~MDBX_AAH_INTERIM;
    rc = mdbx_fastmutex_acquire(&txn->mt_book->me_aah_lock, 0);
    if (likely(rc == MDBX_SUCCESS)) {
      rc = aa_close(txn->mt_book, aah);
      mdbx_ensure(txn->mt_book,
                  mdbx_fastmutex_release(&txn->mt_book->me_aah_lock) ==
                      MDBX_SUCCESS);
    }
  }
  return rc;
}

int mdbx_aa_info(MDBX_txn *txn, MDBX_aah aah, MDBX_aa_info *info,
                 size_t info_size) {
  if (unlikely(!info || info_size != sizeof(MDBX_aa_info)))
    return MDBX_EINVAL;

  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  info->ai_tree_depth = rp.aht->aa.depth16;
  info->ai_branch_pages = rp.aht->aa.branch_pages;
  info->ai_leaf_pages = rp.aht->aa.leaf_pages;
  info->ai_overflow_pages = rp.aht->aa.overflow_pages;
  info->ai_entries = rp.aht->aa.entries;
  info->ai_flags = rp.aht->aa.flags16;
  info->ai_sequence = rp.aht->aa.genseq;
  info->ai_ident = rp.aht->ahe->ax_ident;
  return MDBX_SUCCESS;
}

int mdbx_aa_state(MDBX_txn *txn, MDBX_aah aah, enum MDBX_aah_flags_t *state) {
  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  ahe_t *ahe = bk_aah2ahe(txn->mt_book, aah);
  if (unlikely(!ahe))
    return MDBX_BAD_AAH;

  if (likely(state))
    *state = aa_state(txn, ahe);

  return MDBX_SUCCESS;
}

int mdbx_aa_sequence(MDBX_txn *txn, MDBX_aah aah, uint64_t *result,
                     uint64_t increment) {
  if (unlikely(!result && !increment))
    return MDBX_EINVAL;

  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (increment != 0) {
    rc = validate_txn_more4rw(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  return aa_sequence(txn, rp.aht, result, increment);
}

//-----------------------------------------------------------------------------

static inline MDBX_cursor_result_t cursor_result(int err, MDBX_cursor *cursor) {
  MDBX_cursor_result_t result = {err, cursor};
  return result;
}

MDBX_cursor_result_t mdbx_cursor_open(MDBX_txn *txn, MDBX_aah aah) {
  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return cursor_result(rc, nullptr);

  aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return cursor_result(rp.err, nullptr);

  if (unlikely(rp.aht == aht_gaco(txn) && !F_ISSET(txn->mt_flags, MDBX_RDONLY)))
    return cursor_result(MDBX_EPERM, nullptr);

  MDBX_cursor *cursor = malloc(cursor_size(rp.aht));
  if (unlikely(cursor != nullptr))
    return cursor_result(MDBX_ENOMEM, nullptr);

  rc = cursor_open(txn, rp.aht, cursor);
  if (unlikely(rc != MDBX_SUCCESS)) {
    free(cursor);
    return cursor_result(rc, nullptr);
  }

  return cursor_result(MDBX_SUCCESS, cursor);
}

int mdbx_cursor_close(MDBX_cursor *cursor) {
  int rc = validate_cursor4close(cursor);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_cursor *bc = container_of(cursor, MDBX_cursor, primal);
  rc = cursor_close(bc);
  switch (rc) {
  case MDBX_RESULT_FALSE:
    free(bc);
  /* fall through */
  case MDBX_RESULT_TRUE:
    return MDBX_SUCCESS;
  default:
    return rc;
  }
}

int mdbx_cursor_renew(MDBX_txn *txn, MDBX_cursor *bundle) {
  int rc = validate_cursor4close(bundle);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS)) {
    /* allow untracking for finished txn */
    if (rc != MDBX_BAD_TXN)
      return rc;
  }

  if (unlikely(bundle->mc_backup))
    return MDBX_EPERM;

  cursor_untrack(bundle);
  if (unlikely(rc != MDBX_SUCCESS))
    /* return error after untracking for finished txn */
    return rc;

  return cursor_init(bundle, txn, bundle->primal.mc_aht);
}

MDBX_txn_result_t mdbx_cursor_txn(const MDBX_cursor *cursor) {
  MDBX_txn_result_t result = {validate_cursor4operation_ro(cursor), nullptr};
  if (likely(result.err == MDBX_SUCCESS))
    result.txn = cursor->primal.mc_txn;
  return result;
}

MDBX_aah_result_t mdbx_cursor_aah(const MDBX_cursor *cursor) {
  MDBX_aah_result_t result = {validate_cursor4operation_ro(cursor),
                              MDBX_INVALID_AAH};
  if (likely(result.err == MDBX_SUCCESS)) {
    if (likely(cursor_is_aah_valid(cursor)))
      result.aah = bk_ahe2aah(cursor->primal.mc_txn->mt_book,
                              cursor->primal.mc_aht->ahe);
    else
      result.err = MDBX_BAD_AAH;
  }
  return result;
}

int mdbx_cursor_get(MDBX_cursor *mc, MDBX_iov *key, MDBX_iov *data,
                    enum MDBX_cursor_op op) {
  int rc = validate_cursor4operation_ro(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return cursor_get(&mc->primal, key, data, op);
}

int mdbx_cursor_put(MDBX_cursor *mc, MDBX_iov *key, MDBX_iov *data,
                    unsigned flags) {
  STATIC_ASSERT(((MDBX_IUD_FLAGS & NODE_ADD_FLAGS) & MDBX_NODE_FLAGS) == 0);

  if (unlikely(mc == nullptr || key == nullptr))
    return MDBX_EINVAL;

  if (unlikely(flags & ~MDBX_IUD_FLAGS))
    return MDBX_EINVAL;

  int rc = validate_cursor4operation_rw(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_milieu *const bk = mc->primal.mc_txn->mt_book;
  if (unlikely(key->iov_len > bk->me_keymax))
    return MDBX_BAD_VALSIZE;

  aht_t *const aht = mc->primal.mc_aht;
  if (unlikely(data->iov_len > ((aht->aa.flags16 & MDBX_DUPSORT)
                                    ? bk->me_keymax
                                    : MDBX_MAXDATASIZE)))
    return MDBX_BAD_VALSIZE;

  if (flags & MDBX_IUD_RESERVE) {
    if (unlikely(aht->aa.flags16 & (MDBX_DUPSORT | MDBX_REVERSEDUP)))
      return MDBX_INCOMPATIBLE;
    data->iov_base = nullptr;
  }

  if ((aht->aa.flags16 & MDBX_INTEGERKEY) &&
      unlikely(key->iov_len != sizeof(uint32_t) &&
               key->iov_len != sizeof(uint64_t))) {
    assert(!"key-size is invalid for MDBX_INTEGERKEY");
    return MDBX_BAD_VALSIZE;
  }

  if ((aht->aa.flags16 & MDBX_INTEGERDUP) &&
      unlikely(data->iov_len != sizeof(uint32_t) &&
               data->iov_len != sizeof(uint64_t))) {
    assert(!"data-size is invalid MDBX_INTEGERDUP");
    return MDBX_BAD_VALSIZE;
  }

  return cursor_put(&mc->primal, key, data, flags);
}

// int mdbx_cursor_put_multiple(MDBX_cursor *mc, MDBX_iov *key, MDBX_iov *data,
//                unsigned flags) {
//}

int mdbx_cursor_delete(MDBX_cursor *mc, unsigned flags) {
  if (flags & ~MDBX_IUD_NODUP)
    return MDBX_EINVAL;

  int rc = validate_cursor4operation_rw(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return cursor_delete(&mc->primal, flags);
}

//-----------------------------------------------------------------------------

int __cold mdbx_bk_copy2fd(MDBX_milieu *bk, MDBX_filehandle_t fd,
                           enum MDBX_copy_flags_t flags) {
  int rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (flags & MDBX_CP_COMPACT)
    return bk_copy_compact(bk, fd);

  return bk_copy_asis(bk, fd);
}

int __cold mdbx_bk_copy(MDBX_milieu *bk, const char *pathname,
                        enum MDBX_copy_flags_t flags) {
  int rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  char *molded_dxb_filename = nullptr;
  MDBX_filehandle_t newfd = MDBX_INVALID_FD;

  if (flags & MDBX_CP_NOSUBDIR) {
    molded_dxb_filename = (char *)pathname;
  } else {
    if (mdbx_asprintf(&molded_dxb_filename, "%s%s", pathname,
                      MDBX_PATH_SEPARATOR MDBX_DATANAME) < 0) {
      rc = mdbx_get_errno();
      goto bailout;
    }
    pathname = molded_dxb_filename;
  }

  /* The destination path must exist, but the destination file must not.
   * We don't want the OS to cache the writes, since the source data is
   * already in the OS cache. */
  rc = mdbx_openfile(pathname, O_WRONLY | O_CREAT | O_EXCL, 0666, &newfd);
  if (rc == MDBX_SUCCESS) {
    if (bk->me_psize >= bk->me_os_psize) {
#ifdef F_NOCACHE /* __APPLE__ */
      (void)fcntl(newfd, F_NOCACHE, 1);
#elif defined(O_DIRECT) && defined(F_GETFL)
      /* Set O_DIRECT if the file system supports it */
      if ((rc = fcntl(newfd, F_GETFL)) != -1)
        (void)fcntl(newfd, F_SETFL, rc | O_DIRECT);
#endif
    }
    rc = mdbx_bk_copy2fd(bk, newfd, flags);
  }

bailout:
  free(molded_dxb_filename);

  if (newfd != MDBX_INVALID_FD) {
    int err = mdbx_closefile(newfd);
    if (rc == MDBX_SUCCESS && rc != err)
      rc = err;
  }

  return rc;
}

//-----------------------------------------------------------------------------

int __cold mdbx_bk_regime(MDBX_milieu *bk, unsigned *arg) {
  if (unlikely(!arg))
    return MDBX_EINVAL;

  int rc = validate_bk(bk);
  if (likely(rc == MDBX_SUCCESS))
    *arg = bk->me_flags32;
  return rc;
}

int __cold mdbx_get_maxkeysize(MDBX_milieu *bk) {
  if (!bk || bk->me_signature != MDBX_ME_SIGNATURE || !bk->me_keymax)
    return -MDBX_EINVAL;
  return bk->me_keymax;
}

int __cold mdbx_set_userctx(MDBX_milieu *bk, void *ctx) {
  int rc = validate_bk(bk);
  if (likely(rc == MDBX_SUCCESS))
    bk->me_userctx = ctx;
  return rc;
}

void *__cold mdbx_get_userctx(MDBX_milieu *bk) {
  return likely(validate_bk(bk) == MDBX_SUCCESS) ? bk->me_userctx : nullptr;
}

int __cold mdbx_set_assert(MDBX_milieu *bk, MDBX_assert_func *func) {
  int rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

#if MDBX_DEBUG
  bk->me_assert_func = func;
  return MDBX_SUCCESS;
#else
  (void)func;
  return MDBX_ENOSYS;
#endif
}

int __cold mdbx_bk_info(MDBX_milieu *bk, MDBX_milieu_info *info,
                        size_t info_size, MDBX_env_info *env, size_t env_size) {
  if (info_size != sizeof(MDBX_milieu_info) &&
      !(info == nullptr && info_size == 0))
    return MDBX_EINVAL;

  if (env_size != sizeof(MDBX_env_info) && !(env == nullptr && env_size == 0))
    return MDBX_EINVAL;

  int rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (info_size) {
    const meta_t *const meta0 = metapage(bk, 0);
    const meta_t *const meta1 = metapage(bk, 1);
    const meta_t *const meta2 = metapage(bk, 2);
    const meta_t *meta;
    do {
      meta = meta_head(bk);
      info->bi_recent_txnid = meta_txnid_fluid(bk, meta);
      info->bi_meta.txnid0 = meta_txnid_fluid(bk, meta0);
      info->bi_meta.sign0 = meta0->mm_datasync_sign;
      info->bi_meta.txnid1 = meta_txnid_fluid(bk, meta1);
      info->bi_meta.sign1 = meta1->mm_datasync_sign;
      info->bi_meta.txnid2 = meta_txnid_fluid(bk, meta2);
      info->bi_meta.sign2 = meta2->mm_datasync_sign;
      info->bi_last_pgno = meta->mm_geo.next - 1;
      info->bi_geo.lower = pgno2bytes(bk, meta->mm_geo.lower);
      info->bi_geo.upper = pgno2bytes(bk, meta->mm_geo.upper);
      info->bi_geo.current = pgno2bytes(bk, meta->mm_geo.now);
      info->bi_geo.shrink = pgno2bytes(bk, meta->mm_geo.shrink16);
      info->bi_geo.grow = pgno2bytes(bk, meta->mm_geo.grow16);
      info->bi_mapsize = bk->me_mapsize;
      info->bi_dirty_volume = bk->me_lck ? bk->me_lck->li_dirty_volume : 0;
      mdbx_compiler_barrier();
    } while (unlikely(info->bi_meta.txnid0 != meta_txnid_fluid(bk, meta0) ||
                      info->bi_meta.sign0 != meta0->mm_datasync_sign ||
                      info->bi_meta.txnid1 != meta_txnid_fluid(bk, meta1) ||
                      info->bi_meta.sign1 != meta1->mm_datasync_sign ||
                      info->bi_meta.txnid2 != meta_txnid_fluid(bk, meta2) ||
                      info->bi_meta.sign2 != meta2->mm_datasync_sign ||
                      meta != meta_head(bk) ||
                      info->bi_recent_txnid != meta_txnid_fluid(bk, meta)));

    info->bi_pagesize = bk->me_psize;
    info->bi_sys_pagesize = bk->me_os_psize;
    info->bi_dxb_ioblk = 0 /* FIXMRE: TODO */;
    info->bi_ovf_ioblk = 0 /* FIXMRE: TODO */;

    info->bi_readers_max = bk->me_maxreaders;
    if (bk->me_lck) {
      MDBX_reader *r = bk->me_lck->li_readers;
      info->bi_latter_reader_txnid = info->bi_recent_txnid;
      for (unsigned i = 0; i < info->bi_readers_num; ++i) {
        if (r[i].mr_pid) {
          const txnid_t txnid = r[i].mr_txnid;
          if (info->bi_latter_reader_txnid > txnid)
            info->bi_latter_reader_txnid = txnid;
        }
      }
      info->bi_readers_num = bk->me_lck->li_numreaders;
      info->bi_autosync_threshold = bk->me_lck->li_autosync_threshold;
      info->bi_regime = bk->me_lck->li_regime;
    } else {
      info->bi_readers_num = INT32_MAX;
      info->bi_latter_reader_txnid = 0;
      info->bi_autosync_threshold = 0;
      info->bi_dirty_volume = 0;
      info->bi_regime = MDBX_RDONLY;
    }
  }

  if (env) {
    env->ei_aah_max = bk->env_ah_max;
    env->ei_aah_num = bk->env_ah_num;
    env->ei_debugflags = mdbx_runtime_flags;
    env->ei_userctx = bk->me_userctx;
    env->ei_debug_callback = mdbx_debug_logger;
#if MDBX_DEBUG
    env->ei_assert_callback = bk->me_assert_func;
#else
    env->ei_assert_callback = nullptr;
#endif
    env->ei_rbr_callback = bk->me_callback_rbr;
    env->ei_dxb_fd = bk->me_dxb_fd;
    env->ei_clk_fd = bk->me_lck_fd;
    env->ei_ovf_fd = MDBX_INVALID_FD /* FIXME: TODO */;
    env->ei_pathnames = bk->me_pathname_buf;
  }

  return MDBX_SUCCESS;
}

#ifdef __SANITIZE_THREAD__
/* LY: avoid tsan-trap by me_txn, mm_last_pg and mt_next_pgno */
__attribute__((no_sanitize_thread, noinline))
#endif
int mdbx_tn_lag(MDBX_txn *txn, int *percent)
{
  int rc = validate_txn_rw(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_milieu *bk = txn->mt_book;
  if (unlikely((txn->mt_flags & MDBX_RDONLY) == 0)) {
    if (percent)
      *percent =
          (int)((txn->mt_next_pgno * UINT64_C(100) + txn->mt_end_pgno / 2) /
                txn->mt_end_pgno);
    return -1;
  }

  txnid_t recent;
  meta_t *meta;
  do {
    meta = meta_head(bk);
    recent = meta_txnid_fluid(bk, meta);
    if (percent) {
      const pgno_t maxpg = meta->mm_geo.now;
      *percent = (int)((meta->mm_geo.next * UINT64_C(100) + maxpg / 2) / maxpg);
    }
  } while (unlikely(recent != meta_txnid_fluid(bk, meta)));

  txnid_t lag = recent - txn->mt_ro_reader->mr_txnid;
  return (lag > INT_MAX) ? INT_MAX : (int)lag;
}

int __cold mdbx_walk(MDBX_txn *txn, MDBX_walk_func *visitor, void *user) {
  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  walk_ctx_t ctx;
  ctx.mw_txn = txn;
  ctx.mw_user = user;
  ctx.mw_visitor = visitor;

  rc = visitor(0, NUM_METAS, user, "meta", "meta", NUM_METAS,
               sizeof(meta_t) * NUM_METAS, PAGEHDRSZ * NUM_METAS,
               (txn->mt_book->me_psize - sizeof(meta_t) - PAGEHDRSZ) *
                   NUM_METAS);
  if (!rc)
    rc = do_walk(&ctx, "gaco", txn->txn_aht_array[MDBX_GACO_AAH].aa.root, 0);
  if (!rc)
    rc = do_walk(&ctx, "main", txn->txn_aht_array[MDBX_MAIN_AAH].aa.root, 0);
  if (!rc)
    rc = visitor(P_INVALID, 0, user, nullptr, nullptr, 0, 0, 0, 0);
  return rc;
}

int mdbx_canary_put(MDBX_txn *txn, const MDBX_canary_t *canary) {
  int rc = validate_txn_rw(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (likely(canary)) {
    if (txn->mt_canary.x == canary->x && txn->mt_canary.y == canary->y &&
        txn->mt_canary.z == canary->z)
      return MDBX_SUCCESS;
    txn->mt_canary.x = canary->x;
    txn->mt_canary.y = canary->y;
    txn->mt_canary.z = canary->z;
  }
  txn->mt_canary.v = txn->mt_txnid;

  if ((txn->mt_flags & MDBX_TXN_DIRTY) == 0) {
    MDBX_milieu *bk = txn->mt_book;
    txn->mt_flags |= MDBX_TXN_DIRTY;
    bk->me_lck->li_dirty_volume += bk->me_psize;
  }

  return MDBX_SUCCESS;
}

int mdbx_canary_get(MDBX_txn *txn, MDBX_canary_t *canary) {
  if (unlikely(!canary))
    return MDBX_EINVAL;

  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  *canary = txn->mt_canary;
  return MDBX_SUCCESS;
}

int mdbx_cursor_at_first(MDBX_cursor *cursor) {
  int rc = validate_cursor4operation_ro(cursor);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (!(cursor->primal.mc_state8 & C_INITIALIZED))
    return MDBX_RESULT_FALSE;

  for (unsigned i = 0; i < cursor->primal.mc_snum; ++i) {
    if (cursor->primal.mc_ki[i])
      return MDBX_RESULT_FALSE;
  }

  /* if (cursor->subordinate.mx_cursor.mc_state8 & C_INITIALIZED) {
    for (unsigned i = 0; i < cursor->subordinate.mx_cursor.mc_snum; ++i) {
      if (cursor->subordinate.mx_cursor.mc_ki[i])
        return MDBX_RESULT_FALSE;
    }
  } */

  return MDBX_RESULT_TRUE;
}

int mdbx_cursor_at_last(MDBX_cursor *cursor) {
  int rc = validate_cursor4operation_ro(cursor);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (!(cursor->primal.mc_state8 & C_INITIALIZED))
    return MDBX_RESULT_FALSE;

  for (unsigned i = 0; i < cursor->primal.mc_snum; ++i) {
    unsigned nkeys = page_numkeys(cursor->primal.mc_pg[i]);
    if (cursor->primal.mc_ki[i] < nkeys - 1)
      return MDBX_RESULT_FALSE;
  }

  /* if (cursor->subordinate.mx_cursor.mc_state8 & C_INITIALIZED) {
    for (unsigned i = 0; i < cursor->subordinate.mx_cursor.mc_snum; ++i) {
      unsigned nkeys = page_numkeys(cursor->subordinate.mx_cursor.mc_pg[i]);
      if (cursor->subordinate.mx_cursor.mc_ki[i] < nkeys - 1)
        return MDBX_RESULT_FALSE;
    }
  } */

  return MDBX_RESULT_TRUE;
}

int mdbx_cursor_eof(MDBX_cursor *cursor) {
  int rc = validate_cursor4operation_ro(cursor);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (!(cursor->primal.mc_state8 & C_INITIALIZED) ||
      cursor->primal.mc_snum == 0)
    return MDBX_RESULT_TRUE;

  if ((cursor->primal.mc_state8 & C_EOF) &&
      cursor->primal.mc_ki[cursor->primal.mc_top] >=
          page_numkeys(cursor->primal.mc_pg[cursor->primal.mc_top]))
    return MDBX_RESULT_TRUE;

  return MDBX_RESULT_FALSE;
}

/* Позволяет обновить или удалить существующую запись с получением
 * в old_data предыдущего значения данных. При этом если new_data равен
 * нулю, то выполняется удаление, иначе обновление/вставка.
 *
 * Текущее значение может находиться в уже измененной (грязной) странице.
 * В этом случае страница будет перезаписана при обновлении, а само старое
 * значение утрачено. Поэтому исходно в old_data должен быть передан
 * дополнительный буфер для копирования старого значения.
 * Если переданный буфер слишком мал, то функция вернет -1, установив
 * old_data->iov_len в соответствующее значение.
 *
 * Для не-уникальных ключей также возможен второй сценарий использования,
 * когда посредством old_data из записей с одинаковым ключом для
 * удаления/обновления выбирается конкретная. Для выбора этого сценария
 * во flags следует одновременно указать MDBX_IUD_CURRENT и
 * MDBX_IUD_NOOVERWRITE.
 * Именно эта комбинация выбрана, так как она лишена смысла, и этим позволяет
 * идентифицировать запрос такого сценария.
 *
 * Функция может быть замещена соответствующими операциями с курсорами
 * после двух доработок (TODO):
 *  - внешняя аллокация курсоров, в том числе на стеке (без malloc).
 *  - получения статуса страницы по адресу (знать о P_DIRTY).
 */
int mdbx_replace(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *new_data,
                 MDBX_iov *old_data, unsigned flags) {
  if (unlikely(!key || !old_data || old_data == new_data))
    return MDBX_EINVAL;

  if (unlikely(old_data->iov_base == nullptr && old_data->iov_len))
    return MDBX_EINVAL;

  if (unlikely(new_data == nullptr && !(flags & MDBX_IUD_CURRENT)))
    return MDBX_EINVAL;

  if (unlikely(flags &
               ~(MDBX_IUD_NOOVERWRITE | MDBX_IUD_NODUP | MDBX_IUD_RESERVE |
                 MDBX_IUD_APPEND | MDBX_IUD_APPENDDUP | MDBX_IUD_CURRENT)))
    return MDBX_EINVAL;

  int rc = validate_txn_rw(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  MDBX_cursor mc;
  rc = cursor_init(&mc, txn, rp.aht);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  MDBX_cursor **const head = cursor_listhead(&mc);
  mc.mc_next = *head;
  *head = &mc;

  MDBX_iov present_key = *key;
  if (F_ISSET(flags, MDBX_IUD_CURRENT | MDBX_IUD_NOOVERWRITE)) {
    /* в old_data значение для выбора конкретного дубликата */
    if (unlikely(!(rp.aht->aa.flags16 & MDBX_DUPSORT))) {
      rc = MDBX_EINVAL;
      goto bailout;
    }

    /* убираем лишний бит, он был признаком запрошенного режима */
    flags -= MDBX_IUD_NOOVERWRITE;

    rc = mdbx_cursor_get(&mc, &present_key, old_data, MDBX_GET_BOTH);
    if (rc != MDBX_SUCCESS)
      goto bailout;

    if (new_data) {
      /* обновление конкретного дубликата */
      if (mdbx_iov_eq(old_data, new_data))
        /* если данные совпадают, то ничего делать не надо */
        goto bailout;
    }
  } else {
    /* в old_data буфер для сохранения предыдущего значения */
    if (unlikely(new_data && old_data->iov_base == new_data->iov_base))
      return MDBX_EINVAL;
    MDBX_iov present_data;
    rc = mdbx_cursor_get(&mc, &present_key, &present_data, MDBX_SET_KEY);
    if (unlikely(rc != MDBX_SUCCESS)) {
      old_data->iov_base = nullptr;
      old_data->iov_len = rc;
      if (rc != MDBX_NOTFOUND || (flags & MDBX_IUD_CURRENT))
        goto bailout;
    } else if (flags & MDBX_IUD_NOOVERWRITE) {
      rc = MDBX_KEYEXIST;
      *old_data = present_data;
      goto bailout;
    } else {
      page_t *page = mc.primal.mc_pg[mc.primal.mc_top];
      if (rp.aht->aa.flags16 & MDBX_DUPSORT) {
        if (flags & MDBX_IUD_CURRENT) {
          /* для не-уникальных ключей позволяем update/delete только если ключ
           * один */
          node_t *leaf = node_ptr(page, mc.primal.mc_ki[mc.primal.mc_top]);
          if (F_ISSET(leaf->node_flags8, NODE_DUP)) {
            assert((mc.subordinate.mx_cursor.mc_state8 & C_INITIALIZED) &&
                   mc.subordinate.mx_aht_body.aa.entries > 1);
            if (mc.subordinate.mx_aht_body.aa.entries > 1) {
              rc = MDBX_EMULTIVAL;
              goto bailout;
            }
          }
          /* если данные совпадают, то ничего делать не надо */
          if (new_data && mdbx_iov_eq(&present_data, new_data)) {
            *old_data = *new_data;
            goto bailout;
          }
          /* В оригинальной LMDB фладок MDBX_IUD_CURRENT здесь приведет
           * к замене данных без учета MDBX_DUPSORT сортировки,
           * но здесь это в любом случае допустимо, так как мы
           * проверили что для ключа есть только одно значение. */
        } else if ((flags & MDBX_IUD_NODUP) &&
                   mdbx_iov_eq(&present_data, new_data)) {
          /* если данные совпадают и установлен MDBX_IUD_NODUP */
          rc = MDBX_KEYEXIST;
          goto bailout;
        }
      } else {
        /* если данные совпадают, то ничего делать не надо */
        if (new_data && mdbx_iov_eq(&present_data, new_data)) {
          *old_data = *new_data;
          goto bailout;
        }
        flags |= MDBX_IUD_CURRENT;
      }

      if (page->mp_flags16 & P_DIRTY) {
        if (unlikely(old_data->iov_len < present_data.iov_len)) {
          old_data->iov_base = nullptr;
          old_data->iov_len = present_data.iov_len;
          rc = MDBX_RESULT_TRUE;
          goto bailout;
        }
        memcpy(old_data->iov_base, present_data.iov_base, present_data.iov_len);
        old_data->iov_len = present_data.iov_len;
      } else {
        *old_data = present_data;
      }
    }
  }

  if (likely(new_data))
    rc = cursor_put(&mc.primal, key, new_data, flags);
  else
    rc = mdbx_cursor_delete(&mc, 0);

bailout:
  *head = mc.mc_next;
  return rc;
}

int mdbx_get(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data) {

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  DKBUF;
  mdbx_debug("===> get db %" PRIuFAST32 " key [%s]", aah, DKEY(key));

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  MDBX_cursor mc;
  rc = cursor_init(&mc, txn, rp.aht);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  int exact = 0;
  return cursor_set(&mc.primal, key, data, MDBX_SET, &exact);
}

int mdbx_get_ex(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
                size_t *values_count) {
  DKBUF;
  mdbx_debug("===> get AA %" PRIuFAST32 " key [%s]", aah, DKEY(key));

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  int rc = validate_txn_rw(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  MDBX_cursor mc;
  rc = cursor_init(&mc, txn, rp.aht);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  int exact = 0;
  rc = cursor_set(&mc.primal, key, data, MDBX_SET_KEY, &exact);
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc == MDBX_NOTFOUND && values_count)
      *values_count = 0;
    return rc;
  }

  if (values_count) {
    *values_count = 1;
    if (mc.primal.mc_kind8 & S_HAVESUB) {
      node_t *leaf = node_ptr(mc.primal.mc_pg[mc.primal.mc_top],
                              mc.primal.mc_ki[mc.primal.mc_top]);
      if (F_ISSET(leaf->node_flags8, NODE_DUP)) {
        assert(mc.subordinate.mx_cursor.mc_state8 & C_INITIALIZED);
        *values_count = (sizeof(*values_count) >=
                             sizeof(mc.subordinate.mx_aht_body.aa.entries) ||
                         mc.subordinate.mx_aht_body.aa.entries <= SIZE_MAX)
                            ? (size_t)mc.subordinate.mx_aht_body.aa.entries
                            : SIZE_MAX;
      }
    }
  }
  return MDBX_SUCCESS;
}

/* Функция сообщает находится ли указанный адрес в "грязной" странице у
 * заданной пишущей транзакции. В конечном счете это позволяет избавиться от
 * лишнего копирования данных из НЕ-грязных страниц.
 *
 * "Грязные" страницы - это те, которые уже были изменены в ходе пишущей
 * транзакции. Соответственно, какие-либо дальнейшие изменения могут привести
 * к перезаписи таких страниц. Поэтому все функции, выполняющие изменения, в
 * качестве аргументов НЕ должны получать указатели на данные в таких
 * страницах. В свою очередь "НЕ грязные" страницы перед модификацией будут
 * скопированы.
 *
 * Другими словами, данные из "грязных" страниц должны быть либо скопированы
 * перед передачей в качестве аргументов для дальнейших модификаций, либо
 * отвергнуты на стадии проверки корректности аргументов.
 *
 * Таким образом, функция позволяет как избавится от лишнего копирования,
 * так и выполнить более полную проверку аргументов.
 *
 * ВАЖНО: Передаваемый указатель должен указывать на начало данных. Только
 * так гарантируется что актуальный заголовок страницы будет физически
 * расположен в той-же странице памяти, в том числе для многостраничных
 * P_OVERFLOW страниц с длинными данными. */
int mdbx_is_dirty(const MDBX_txn *txn, const void *ptr) {
  int rc = validate_txn(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(txn->mt_flags & MDBX_RDONLY))
    return MDBX_RESULT_FALSE;

  const MDBX_milieu *bk = txn->mt_book;
  const uintptr_t mask = ~(uintptr_t)(bk->me_psize - 1);
  const page_t *page = (const page_t *)((uintptr_t)ptr & mask);

  /* LY: Тут не всё хорошо с абсолютной достоверностью результата,
   * так как флажок P_DIRTY в LMDB может означать не совсем то,
   * что было исходно задумано, детали см в логике кода page_touch().
   *
   * Более того, в режиме БЕЗ WRITEMAP грязные страницы выделяются через
   * malloc(), т.е. находятся вне mmap-диапазона и тогда чтобы отличить
   * действительно грязную страницу от указателя на данные пользователя
   * следует сканировать dirtylist, что накладно.
   *
   * Тем не менее, однозначно страница "не грязная" (не будет переписана
   * во время транзакции) если адрес находится внутри mmap-диапазона
   * и в заголовке страницы нет флажка P_DIRTY. */
  if (bk->me_map < (uint8_t *)page) {
    const size_t usedbytes = pgno2bytes(bk, txn->mt_next_pgno);
    if ((uint8_t *)page < bk->me_map + usedbytes) {
      /* страница внутри диапазона, смотрим на флажки */
      return (page->mp_flags16 & (P_DIRTY | P_LOOSE | P_KEEP))
                 ? MDBX_RESULT_TRUE
                 : MDBX_RESULT_FALSE;
    }
    /* Гипотетически здесь возможна ситуация, когда указатель адресует что-то
     * в пределах mmap, но за границей распределенных страниц. Это тяжелая
     * ошибка, к которой не возможно прийти без каких-то больших нарушений.
     * Поэтому не проверяем этот случай кроме как assert-ом, на то что
     * страница вне mmap-диаппазона. */
    assert((uint8_t *)page >= bk->me_map + bk->me_mapsize);
  }

  /* Страница вне используемого mmap-диапазона, т.е. либо в функцию был
   * передан некорректный адрес, либо адрес в теневой странице, которая была
   * выделена посредством malloc().
   *
   * Для WRITE_MAP режима такая страница однозначно "не грязная",
   * а для режимов без WRITE_MAP следует просматривать списки dirty
   * и spilled страниц у каких-либо транзакций (в том числе дочерних).
   *
   * Поэтому для WRITE_MAP возвращаем false, а для остальных режимов
   * всегда true. Такая логика имеет ряд преимуществ:
   *  - не тратим время на просмотр списков;
   *  - результат всегда безопасен (может быть ложно-положительным,
   *    но не ложно-отрицательным);
   *  - результат не зависит от вложенности транзакций и от относительного
   *    положения переданной транзакции в этой рекурсии. */
  return (bk->me_flags32 & MDBX_WRITEMAP) ? MDBX_RESULT_FALSE
                                          : MDBX_RESULT_TRUE;
}

/*----------------------------------------------------------------------------*/

/* Dump a key in ascii or hexadecimal. */
char *mdbx_dump_iov(const MDBX_iov *iov, char *const buf,
                    const size_t bufsize) {
  if (!iov)
    return "<null>";
  if (!buf || bufsize < 4)
    return nullptr;
  if (!iov->iov_len)
    return "<empty>";

  const uint8_t *const data = iov->iov_base;
  bool is_ascii = true;
  unsigned i;
  for (i = 0; is_ascii && i < iov->iov_len; i++)
    if (data[i] < ' ' || data[i] > 127)
      is_ascii = false;

  if (is_ascii) {
    int len =
        snprintf(buf, bufsize, "%.*s",
                 (iov->iov_len > INT_MAX) ? INT_MAX : (int)iov->iov_len, data);
    assert(len > 0 && (unsigned)len < bufsize);
    (void)len;
  } else {
    char *const detent = buf + bufsize - 2;
    char *ptr = buf;
    *ptr++ = '<';
    for (i = 0; i < iov->iov_len; i++) {
      const ptrdiff_t left = detent - ptr;
      assert(left > 0);
      int len = snprintf(ptr, left, "%02x", data[i]);
      if (len < 0 || len >= left)
        break;
      ptr += len;
    }
    if (ptr < detent) {
      ptr[0] = '>';
      ptr[1] = '\0';
    }
  }
  return buf;
}

/*----------------------------------------------------------------------------*/
/* attribute support functions for Nexenta */

static int mdbx_attr_peek(MDBX_iov *data, MDBX_attr_t *attrptr) {
  if (unlikely(data->iov_len < sizeof(MDBX_attr_t)))
    return MDBX_INCOMPATIBLE;

  if (likely(attrptr != nullptr))
    *attrptr = *(MDBX_attr_t *)data->iov_base;
  data->iov_len -= sizeof(MDBX_attr_t);
  data->iov_base =
      likely(data->iov_len > 0) ? ((MDBX_attr_t *)data->iov_base) + 1 : nullptr;

  return MDBX_SUCCESS;
}

static int mdbx_attr_poke(MDBX_iov *reserved, MDBX_iov *data, MDBX_attr_t attr,
                          unsigned flags) {
  MDBX_attr_t *space = reserved->iov_base;
  if (flags & MDBX_IUD_RESERVE) {
    if (likely(data != nullptr)) {
      data->iov_base = data->iov_len ? space + 1 : nullptr;
    }
  } else {
    *space = attr;
    if (likely(data != nullptr)) {
      memcpy(space + 1, data->iov_base, data->iov_len);
    }
  }

  return MDBX_SUCCESS;
}

int mdbx_cr_get_attr(MDBX_cursor *mc, MDBX_iov *key, MDBX_iov *data,
                     MDBX_attr_t *attrptr, enum MDBX_cursor_op op) {
  int rc = mdbx_cursor_get(mc, key, data, op);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_peek(data, attrptr);
}

int mdbx_get_attr(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
                  uint64_t *attrptr) {
  int rc = mdbx_get(txn, aah, key, data);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_peek(data, attrptr);
}

int mdbx_put_attr(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
                  MDBX_attr_t attr, unsigned flags) {
  MDBX_iov reserve;
  reserve.iov_base = nullptr;
  reserve.iov_len = (data ? data->iov_len : 0) + sizeof(MDBX_attr_t);

  int rc = mdbx_put(txn, aah, key, &reserve, flags | MDBX_IUD_RESERVE);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_poke(&reserve, data, attr, flags);
}

int mdbx_cr_put_attr(MDBX_cursor *cursor, MDBX_iov *key, MDBX_iov *data,
                     MDBX_attr_t attr, unsigned flags) {
  MDBX_iov reserve;
  reserve.iov_base = nullptr;
  reserve.iov_len = (data ? data->iov_len : 0) + sizeof(MDBX_attr_t);

  int rc = mdbx_cursor_put(cursor, key, &reserve, flags | MDBX_IUD_RESERVE);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return mdbx_attr_poke(&reserve, data, attr, flags);
}

int mdbx_set_attr(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
                  MDBX_attr_t attr) {
  if (unlikely(!key))
    return MDBX_EINVAL;

  int rc = validate_txn_rw(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const aht_rc_t rp = aa_take(txn, aah);
  if (unlikely(rp.err != MDBX_SUCCESS))
    return rp.err;

  MDBX_cursor mc;
  rc = cursor_init(&mc, txn, rp.aht);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_iov old_data;
  rc = cursor_set(&mc.primal, key, &old_data, MDBX_SET, nullptr);
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc == MDBX_NOTFOUND && data) {
      MDBX_cursor **const head = cursor_listhead(&mc);
      mc.mc_next = *head;
      *head = &mc;
      rc = mdbx_cr_put_attr(&mc, key, data, attr, 0);
      *head = mc.mc_next;
    }
    return rc;
  }

  MDBX_attr_t old_attr = 0;
  rc = mdbx_attr_peek(&old_data, &old_attr);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (old_attr == attr && (!data || mdbx_iov_eq(data, &old_data)))
    return MDBX_SUCCESS;

  MDBX_cursor **const head = cursor_listhead(&mc);
  mc.mc_next = *head;
  *head = &mc;
  assert(!mdbx_is_dirty(txn, old_data.iov_base));
  rc = mdbx_cr_put_attr(&mc, key, data ? data : &old_data, attr,
                        MDBX_IUD_CURRENT);
  *head = mc.mc_next;
  return rc;
}
