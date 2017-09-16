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

#pragma once

#include "./bits.h"

/*----------------------------------------------------------------------------*/
/* Internal prototypes */

static txnid_t find_oldest(MDBX_txn *txn);
static int reader_check(MDBX_milieu *bk, int rlocked, int *dead);
static void rthc_dtor(void *rthc);
static void rthc_lock(void);
static void rthc_unlock(void);
static int rthc_alloc(mdbx_thread_key_t *key, MDBX_reader *begin,
                      MDBX_reader *end);
static void rthc_remove(mdbx_thread_key_t key);
static void rthc_cleanup(void);
static int mutex_failed(MDBX_milieu *bk, pthread_mutex_t *mutex, int rc);

static bool mdbx_iov_eq(const MDBX_iov *a, const MDBX_iov *b);
static int mdbx_iov_dup(MDBX_iov *iov);
static void mdbx_iov_free(MDBX_iov *iov);

static ahe_t *bk_aah2ahe(MDBX_milieu *bk, MDBX_aah aah);
static MDBX_aah bk_ahe2aah(MDBX_milieu *bk, ahe_t *ahe);
static aht_t *tnx_ahe2aht(MDBX_txn *txn, ahe_t *ahe);
static ahe_t *ahe_main(MDBX_milieu *bk);
static ahe_t *ahe_gaco(MDBX_milieu *bk);
static aht_t *aht_main(MDBX_txn *txn);
static aht_t *aht_gaco(MDBX_txn *txn);

static int validate_bk(const MDBX_milieu *bk);
static int validate_txn(const MDBX_txn *txn);
static int validate_txn_more4rw(MDBX_txn *txn);
static int validate_txn_rw(MDBX_txn *txn);

static MDBX_TXL mdbx_txl_alloc(void);
static MDBX_PNL mdbx_pnl_alloc(size_t size);
static void mdbx_pnl_free(MDBX_PNL pl);
static void mdbx_txl_free(MDBX_TXL list);
static void mdbx_pnl_xappend(MDBX_PNL pl, pgno_t id);
static bool mdbx_pnl_check(MDBX_PNL pl);
static void mdbx_pnl_sort(MDBX_PNL pnl);
static size_t mdbx_pnl_search(MDBX_PNL pnl, pgno_t id);
static void mdbx_pnl_shrink(MDBX_PNL *ppl);
static int mdbx_txl_grow(MDBX_TXL *ptr, size_t num);
static int mdbx_pnl_need(MDBX_PNL *ppl, size_t num);
static int mdbx_pnl_append(MDBX_PNL *ppl, pgno_t id);
static int mdbx_txl_append(MDBX_TXL *ptr, txnid_t id);
static int mdbx_pnl_append_list(MDBX_PNL *ppl, MDBX_PNL app);
static int mdbx_txl_append_list(MDBX_TXL *ptr, MDBX_TXL append);
static int mdbx_pnl_append_range(MDBX_PNL *ppl, pgno_t id, size_t n);
static void mdbx_pnl_xmerge(MDBX_PNL pnl, MDBX_PNL merge);
static unsigned mdbx_mid2l_search(MDBX_ID2L pnl, pgno_t id);
static int mdbx_mid2l_insert(MDBX_ID2L pnl, MDBX_ID2 *id);
static int mdbx_mid2l_append(MDBX_ID2L pnl, MDBX_ID2 *id);

#define MDBX_ALLOC_CACHE 1
#define MDBX_ALLOC_GC 2
#define MDBX_ALLOC_NEW 4
#define MDBX_ALLOC_KICK 8
#define MDBX_ALLOC_ALL                                                         \
  (MDBX_ALLOC_CACHE | MDBX_ALLOC_GC | MDBX_ALLOC_NEW | MDBX_ALLOC_KICK)

static int page_alloc(cursor_t *mc, unsigned num, page_t **mp, int flags);
static int page_new(cursor_t *mc, uint32_t flags, unsigned num, page_t **mp);
static int page_touch(cursor_t *mc);
static int cursor_touch(cursor_t *mc);
static void cursor_copy(const cursor_t *src, cursor_t *dst);

#define MDBX_END_NAMES                                                         \
  {                                                                            \
    "committed", "empty-commit", "abort", "reset", "reset-tmp", "fail-begin",  \
        "fail-beginchild"                                                      \
  }
enum {
  /* txn_end operation number, for logging */
  MDBX_END_COMMITTED,
  MDBX_END_EMPTY_COMMIT,
  MDBX_END_ABORT,
  MDBX_END_RESET,
  MDBX_END_RESET_TMP,
  MDBX_END_FAIL_BEGIN,
  MDBX_END_FAIL_BEGINCHILD
};
#define MDBX_END_OPMASK 0x0F   /* mask for txn_end() operation number */
#define MDBX_END_FIXUPAAH 0x10 /* keep/export opended AAh to environment */
#define MDBX_END_FREE 0x20     /* free txn unless it is MDBX_milieu.me_txn0 */
#define MDBX_END_EOTDONE 0x40  /* txn's cursors already closed */
#define MDBX_END_SLOT 0x80     /* release any reader slot if MDBX_NOTLS */
#define MDBX_END_FLAGS                                                         \
  (MDBX_END_OPMASK | MDBX_END_FIXUPAAH | MDBX_END_FREE | MDBX_END_EOTDONE |    \
   MDBX_END_SLOT)
static int txn_end(MDBX_txn *txn, unsigned mode);

static int page_get(MDBX_txn *const txn, pgno_t pgno, page_t **mp, int *lvl);
static int page_search_root(cursor_t *mc, MDBX_iov *key, int modify);

#define MDBX_PS_MODIFY 1
#define MDBX_PS_ROOTONLY 2
#define MDBX_PS_FIRST 4
#define MDBX_PS_LAST 8
#define MDBX_PS_FLAGS                                                          \
  (MDBX_PS_MODIFY | MDBX_PS_ROOTONLY | MDBX_PS_FIRST | MDBX_PS_LAST)
static int page_search(cursor_t *mc, MDBX_iov *key, int flags);
static int page_merge(cursor_t *csrc, cursor_t *cdst);

#define MDBX_SPLIT_REPLACE                                                     \
  MDBX_IUD_CURRENT /* newkey is not new, don't save current */
static int page_split(cursor_t *mc, MDBX_iov *newkey, MDBX_iov *newdata,
                      pgno_t newpgno, unsigned nflags);

static int mdbx_read_header(MDBX_milieu *bk, meta_t *meta);
static int mdbx_sync_locked(MDBX_milieu *bk, unsigned flags,
                            meta_t *const pending);
static void bk_release(MDBX_milieu *bk);

static node_t *node_search(cursor_t *mc, MDBX_iov key, int *exactp);
static int node_add(cursor_t *mc, unsigned indx, MDBX_iov *key, MDBX_iov *data,
                    pgno_t pgno, unsigned flags);
static void node_del(cursor_t *mc, size_t keysize);
static void node_shrink(page_t *mp, unsigned indx);
static int node_move(cursor_t *csrc, cursor_t *cdst, int fromleft);
static int node_read(cursor_t *mc, node_t *leaf, MDBX_iov *data);
static size_t leaf_size(MDBX_milieu *bk, MDBX_iov *key, MDBX_iov *data);
static size_t branch_size(MDBX_milieu *bk, MDBX_iov *key);

static int tree_rebalance(cursor_t *mc);
static int update_key(cursor_t *mc, MDBX_iov *key);

static meta_t *metapage(const MDBX_milieu *bk, unsigned n);
static txnid_t meta_txnid_stable(const MDBX_milieu *bk, const meta_t *meta);

static txnid_t meta_txnid_fluid(const MDBX_milieu *bk, const meta_t *meta);

static void meta_update_begin(const MDBX_milieu *bk, meta_t *meta,
                              txnid_t txnid);

static void meta_update_end(const MDBX_milieu *bk, meta_t *meta, txnid_t txnid);

static void meta_set_txnid(const MDBX_milieu *bk, meta_t *meta, txnid_t txnid);
static uint64_t meta_sign(const meta_t *meta);

enum meta_choise_mode { prefer_last, prefer_noweak, prefer_steady };

static bool meta_ot(const enum meta_choise_mode mode, const MDBX_milieu *bk,
                    const meta_t *a, const meta_t *b);
static bool meta_eq(const MDBX_milieu *bk, const meta_t *a, const meta_t *b);
static int meta_eq_mask(const MDBX_milieu *bk);
static meta_t *meta_recent(const enum meta_choise_mode mode,
                           const MDBX_milieu *bk, meta_t *a, meta_t *b);
static meta_t *meta_ancient(const enum meta_choise_mode mode,
                            const MDBX_milieu *bk, meta_t *a, meta_t *b);
static meta_t *meta_mostrecent(const enum meta_choise_mode mode,
                               const MDBX_milieu *bk);
static meta_t *meta_steady(const MDBX_milieu *bk);
static meta_t *meta_head(const MDBX_milieu *bk);
static txnid_t reclaiming_detent(const MDBX_milieu *bk);
static const char *durable_str(const meta_t *const meta);

////////////////////
static int txn_renew0(MDBX_txn *txn, unsigned flags);
static int txn_end(MDBX_txn *txn, unsigned mode);
static int prep_backlog(MDBX_txn *txn, cursor_t *mc);
static int freelist_save(MDBX_txn *txn);
static int mdbx_read_header(MDBX_milieu *bk, meta_t *meta);
static page_t *meta_model(const MDBX_milieu *bk, page_t *model, unsigned num);
static page_t *init_metas(const MDBX_milieu *bk, void *buffer);
static int mdbx_sync_locked(MDBX_milieu *bk, unsigned flags,
                            meta_t *const pending);
static void setup_pagesize(MDBX_milieu *bk, const size_t pagesize);
static int mdbx_bk_map(MDBX_milieu *bk, size_t usedsize);
static int mdbx_setup_dxb(MDBX_milieu *bk, int lck_rc);
static int setup_lck(MDBX_milieu *bk, const char *lck_pathname, mode_t mode);
static void bk_release(MDBX_milieu *bk);
static int tree_drop(cursor_t *mc, int subs);

// static int aa_fetch(MDBX_txn *txn, ahe_t *slot);
static aht_rc_t aa_take(MDBX_txn *txn, MDBX_aah aah);
static unsigned aa_state(MDBX_txn *txn, ahe_t *ahe);
static int aa_create(MDBX_txn *txn, aht_t *aht);
static ahe_rc_t aa_open(MDBX_milieu *bk, MDBX_txn *txn, const MDBX_iov aa_ident,
                        unsigned flags, MDBX_comparer *keycmp,
                        MDBX_comparer *datacmp);
static int aa_addref(MDBX_milieu *bk, MDBX_aah aah);
static int aa_close(MDBX_milieu *bk, MDBX_aah aah);
static int aa_drop(MDBX_txn *txn, enum mdbx_drop_flags_t flags, aht_t *aht);
static int aa_return(MDBX_txn *txn, unsigned txn_end_flags);
static int aa_sequence(MDBX_txn *txn, aht_t *aht, uint64_t *result,
                       uint64_t increment);
////////////////////

static size_t cursor_size(aht_t *aht);
static int cursor_open(MDBX_txn *txn, aht_t *aht, MDBX_cursor *bc);
static int cursor_close(MDBX_cursor *bc);
static void cursor_untrack(MDBX_cursor *bundle);
static void cursor_pop(cursor_t *mc);
static int cursor_push(cursor_t *mc, page_t *mp);
static int txn_shadow_cursors(MDBX_txn *src, MDBX_txn *dst);
static void cursor_unshadow(MDBX_cursor *mc, unsigned commit);

static void cursors_eot(MDBX_txn *txn, unsigned merge);

static int cursor_put(cursor_t *mc, MDBX_iov *key, MDBX_iov *data,
                      unsigned flags);
static int cursor_delete(cursor_t *mc, unsigned flags);

static int mdbx_cr_del0(cursor_t *mc);
static int mdbx_del0(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key, MDBX_iov *data,
                     unsigned flags);
static int cursor_sibling(cursor_t *mc, bool move_right);
static int cursor_next(cursor_t *mc, MDBX_iov *key, MDBX_iov *data,
                       enum MDBX_cursor_op op);
static int cursor_prev(cursor_t *mc, MDBX_iov *key, MDBX_iov *data,
                       enum MDBX_cursor_op op);
static int cursor_set(cursor_t *mc, MDBX_iov *key, MDBX_iov *data,
                      enum MDBX_cursor_op op, int *exactp);
static int cursor_first(cursor_t *mc, MDBX_iov *key, MDBX_iov *data);
static int cursor_last(cursor_t *mc, MDBX_iov *key, MDBX_iov *data);
static int cursor_get(cursor_t *mc, MDBX_iov *key, MDBX_iov *data,
                      enum MDBX_cursor_op op);
static int cursor_init(MDBX_cursor *bundle, MDBX_txn *txn, aht_t *aht);
static cursor_t *subordinate_setup(cursor_t *mc, node_t *node);
static void subcursor_fixup(MDBX_cursor *mc, cursor_t *src_mx,
                            bool new_dupdata);

static MDBX_comparer cmp_int_aligned, cmp_int_aligned_to2;
static MDBX_comparer *default_keycmp(unsigned flags);
static MDBX_comparer *default_datacmp(unsigned flags);

static txnid_t rbr(MDBX_milieu *bk, const txnid_t laggard);
static int audit(MDBX_txn *txn);
static page_t *page_malloc(MDBX_txn *txn, unsigned num);
static void dpage_free(MDBX_milieu *bk, page_t *mp);
static void dlist_free(MDBX_txn *txn);
static size_t bytes_align2os_bytes(const MDBX_milieu *bk, size_t bytes);
static void kill_page(MDBX_milieu *bk, pgno_t pgno);
static int page_loose(cursor_t *mc, page_t *mp);
static int page_flush(MDBX_txn *txn, pgno_t keep);
static int page_spill(cursor_t *curor, MDBX_iov *key, MDBX_iov *data);
static void mdbx_page_dirty(MDBX_txn *txn, page_t *mp);
static int page_alloc(cursor_t *mc, unsigned num, page_t **mp, int flags);
static void page_copy(page_t *dst, page_t *src, unsigned psize);
static int page_unspill(MDBX_txn *txn, page_t *mp, page_t **ret);
static int page_touch(cursor_t *mc);

static txnid_t find_oldest(MDBX_txn *txn);
static int mdbx_mapresize(MDBX_milieu *bk, const pgno_t size_pgno,
                          const pgno_t limit_pgno);

//-----------------------------------------------------------------------------

static void osal_jitter(bool tiny);

static int mdbx_lck_init(MDBX_milieu *bk);

static int mdbx_lck_seize(MDBX_milieu *bk);
static int mdbx_lck_downgrade(MDBX_milieu *bk, bool complete);
static int mdbx_lck_upgrade(MDBX_milieu *bk);
static void mdbx_lck_destroy(MDBX_milieu *bk);

static int mdbx_rdt_lock(MDBX_milieu *bk);
static void mdbx_rdt_unlock(MDBX_milieu *bk);

static int mdbx_tn_lock(MDBX_milieu *bk);
static void mdbx_tn_unlock(MDBX_milieu *bk);

static int mdbx_rpid_set(MDBX_milieu *bk);
static int mdbx_rpid_clear(MDBX_milieu *bk);

/* Checks reader by pid.
 *
 * Returns:
 *   MDBX_RESULT_TRUE, if pid is live (unable to acquire lock)
 *   MDBX_RESULT_FALSE, if pid is dead (lock acquired)
 *   or otherwise the errcode. */
static int mdbx_rpid_check(MDBX_milieu *bk, MDBX_pid_t pid);

static int mdbx_memalign_alloc(size_t alignment, size_t bytes, void **result);
static void mdbx_memalign_free(void *ptr);

static MDBX_pid_t mdbx_getpid(void);
static MDBX_tid_t mdbx_thread_self(void);

static int mdbx_condmutex_init(mdbx_condmutex_t *condmutex);
static int mdbx_condmutex_lock(mdbx_condmutex_t *condmutex);
static int mdbx_condmutex_unlock(mdbx_condmutex_t *condmutex);
static int mdbx_condmutex_signal(mdbx_condmutex_t *condmutex);
static int mdbx_condmutex_wait(mdbx_condmutex_t *condmutex);
static int mdbx_condmutex_destroy(mdbx_condmutex_t *condmutex);

static int mdbx_fastmutex_init(mdbx_fastmutex_t *fastmutex);
static int mdbx_fastmutex_acquire(mdbx_fastmutex_t *fastmutex, unsigned flags);
static int mdbx_fastmutex_release(mdbx_fastmutex_t *fastmutex);
static int mdbx_fastmutex_destroy(mdbx_fastmutex_t *fastmutex);

static int mdbx_pwritev(MDBX_filehandle_t fd, struct iovec *iov, int iovcnt,
                        uint64_t offset, size_t expected_written);
static int mdbx_pread(MDBX_filehandle_t fd, void *buf, size_t count,
                      uint64_t offset);
static int mdbx_pwrite(MDBX_filehandle_t fd, const void *buf, size_t count,
                       uint64_t offset);
static int mdbx_write(MDBX_filehandle_t fd, const void *buf, size_t count);

static int mdbx_thread_create(mdbx_thread_t *thread,
                              THREAD_RESULT(THREAD_CALL *start_routine)(void *),
                              void *arg);
static int mdbx_thread_join(mdbx_thread_t thread);
static int mdbx_thread_key_create(mdbx_thread_key_t *key);
static void mdbx_thread_key_delete(mdbx_thread_key_t key);
static void *mdbx_thread_rthc_get(mdbx_thread_key_t key);
static void mdbx_thread_rthc_set(mdbx_thread_key_t key, const void *value);

static int mdbx_filesync(MDBX_filehandle_t fd, bool fullsync);
static int mdbx_filesize_sync(MDBX_filehandle_t fd);
static int mdbx_ftruncate(MDBX_filehandle_t fd, uint64_t length);
static int mdbx_filesize(MDBX_filehandle_t fd, uint64_t *length);
static int mdbx_openfile(const char *pathname, int flags, mode_t mode,
                         MDBX_filehandle_t *fd);
static int mdbx_closefile(MDBX_filehandle_t fd);
static int mdbx_is_directory(const char *pathname);

static int mdbx_mmap(int flags, mdbx_mmap_t *map, size_t must, size_t limit);
static int mdbx_munmap(mdbx_mmap_t *map);
static int mdbx_mresize(int flags, mdbx_mmap_t *map, size_t current,
                        size_t wanna);
static int mdbx_msync(mdbx_mmap_t *map, size_t offset, size_t length,
                      int async);

/*----------------------------------------------------------------------------*/
/* Internal prototypes and inlines */

static inline void paranoia_barrier(void) { mdbx_compiler_barrier(); }

static inline bool is_power_of_2(size_t x) { return (x & (x - 1)) == 0; }

static inline size_t mdbx_roundup2(size_t value, size_t granularity) {
  assert(is_power_of_2(granularity));
  return (value + granularity - 1) & ~(granularity - 1);
}

static size_t bytes_align2os_bytes(const MDBX_milieu *bk, size_t bytes) {
  return mdbx_roundup2(mdbx_roundup2(bytes, bk->me_psize), bk->me_os_psize);
}

static unsigned uint_log2_ceil(size_t value) {
  assert(is_power_of_2(value));

  unsigned log = 0;
  while (value > 1) {
    log += 1;
    value >>= 1;
  }
  return log;
}

static MDBX_cursor **cursor_listhead(const MDBX_cursor *cursor);
static MDBX_comparer *cursor_key_comparer(const cursor_t *cursor);
static ptrdiff_t cursor_compare_keys(const cursor_t *cursor, const MDBX_iov *a,
                                     const MDBX_iov *b);
static MDBX_comparer *cursor_data_comparer(const cursor_t *cursor);
static ptrdiff_t cursor_compare_data(const cursor_t *cursor, const MDBX_iov *a,
                                     const MDBX_iov *b);
static bool cursor_is_aah_valid(const MDBX_cursor *cursor);

static MDBX_cursor *cursor_bundle(const cursor_t *cursor);
static cursor_t *cursor_primal(cursor_t *cursor);
static cursor_t *cursor_subordinate(const cursor_t *cursor);
static subcursor_t *cursor_subcursor(const cursor_t *cursor);
static subcursor_t *subordinate_subcursor(const cursor_t *cursor);

typedef struct copy_ctx_ copy_ctx_t;
static int bk_copy_compact(MDBX_milieu *bk, MDBX_filehandle_t fd);
static int bk_copy_asis(MDBX_milieu *bk, MDBX_filehandle_t fd);
static int bk_copy_walk(copy_ctx_t *my, pgno_t *pg, int flags);

typedef struct walk_ctx_ {
  MDBX_txn *mw_txn;
  void *mw_user;
  MDBX_walk_func *mw_visitor;
} walk_ctx_t;
static int do_walk(walk_ctx_t *ctx, const char *aah, pgno_t pg, int deep);

/* Perform act while tracking temporary cursor mn */
#define WITH_CURSOR_TRACKING(mn, act)                                          \
  do {                                                                         \
    MDBX_cursor *_tracked = &(mn);                                             \
    MDBX_cursor **_head = cursor_listhead(_tracked);                           \
    _tracked->mc_next = *_head;                                                \
    *_head = _tracked;                                                         \
    { act; }                                                                   \
    *_head = _tracked->mc_next;                                                \
  } while (0)

static inline void jitter4testing(bool tiny) {
#ifndef NDEBUG
  if (MDBX_DBG_JITTER & mdbx_runtime_flags)
    osal_jitter(tiny);
#else
  (void)tiny;
#endif
}

static inline size_t pgno2bytes(const MDBX_milieu *bk, pgno_t pgno) {
  mdbx_assert(bk, (1u << bk->me_psize2log) == bk->me_psize);
  return ((size_t)pgno) << bk->me_psize2log;
}

static inline page_t *pgno2page(const MDBX_milieu *bk, pgno_t pgno) {
  return (page_t *)(bk->me_map + pgno2bytes(bk, pgno));
}

static inline pgno_t bytes2pgno(const MDBX_milieu *bk, size_t bytes) {
  mdbx_assert(bk, (bk->me_psize >> bk->me_psize2log) == 1);
  return (pgno_t)(bytes >> bk->me_psize2log);
}

static inline pgno_t pgno_add(pgno_t base, pgno_t augend) {
  assert(base <= MAX_PAGENO);
  return (augend < MAX_PAGENO - base) ? base + augend : MAX_PAGENO;
}

static inline pgno_t pgno_sub(pgno_t base, pgno_t subtrahend) {
  assert(base >= MIN_PAGENO);
  return (subtrahend < base - MIN_PAGENO) ? base - subtrahend : MIN_PAGENO;
}

static inline size_t pgno_align2os_bytes(const MDBX_milieu *bk, pgno_t pgno) {
  return mdbx_roundup2(pgno2bytes(bk, pgno), bk->me_os_psize);
}

static inline pgno_t pgno_align2os_pgno(const MDBX_milieu *bk, pgno_t pgno) {
  return bytes2pgno(bk, pgno_align2os_bytes(bk, pgno));
}

static inline void set_pgno_lea16(void *ptr, pgno_t pgno) {
  if (sizeof(pgno_t) == 4)
    set_le32_aligned16(ptr, (uint32_t)pgno);
  else
    set_le64_aligned16(ptr, (uint64_t)pgno);
}

static inline pgno_t get_pgno_lea16(void *ptr) {
  if (sizeof(pgno_t) == 4)
    return (pgno_t)get_le32_aligned16(ptr);
  else
    return (pgno_t)get_le64_aligned16(ptr);
}

/*----------------------------------------------------------------------------*/
/* Validation functions */

static inline int validate_bk(const MDBX_milieu *bk) {
  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  return MDBX_SUCCESS;
}

static int validate_bk_aah(MDBX_milieu *bk, MDBX_aah aah) {
  int rc = validate_bk(bk);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(aah < CORE_AAH || aah >= bk->env_ah_max))
    return MDBX_EINVAL;

  if (unlikely(!bk_aah2ahe(bk, aah)))
    return MDBX_BAD_AAH;

  return MDBX_SUCCESS;
}

static inline int validate_cursor4close(const MDBX_cursor *cursor) {
  if (unlikely(!cursor))
    return MDBX_EINVAL;

  if (unlikely(cursor->mc_signature != MDBX_MC_SIGNATURE &&
               cursor->mc_signature != MDBX_MC_READY4CLOSE))
    return MDBX_EBADSIGN;

  return MDBX_SUCCESS;
}

static inline int validate_cursor4operation_ro(const MDBX_cursor *cursor) {
  if (unlikely(!cursor))
    return MDBX_EINVAL;

  if (unlikely(cursor->mc_signature != MDBX_MC_SIGNATURE))
    return MDBX_EBADSIGN;

  return validate_txn(cursor->primal.mc_txn);
}

static inline int validate_cursor4operation_rw(const MDBX_cursor *cursor) {
  int rc = validate_cursor4operation_ro(cursor);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = validate_txn_more4rw(cursor->primal.mc_txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return MDBX_SUCCESS;
}
