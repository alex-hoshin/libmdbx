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

static inline meta_t *metapage(const MDBX_milieu *bk, unsigned n) {
  return &pgno2page(bk, n)->mp_meta;
}

#define METAPAGE_END(bk) metapage(bk, NUM_METAS)

static inline txnid_t meta_txnid(const MDBX_milieu *bk, const meta_t *meta,
                                 bool allow_volatile) {
  mdbx_assert(bk, meta >= metapage(bk, 0) || meta < METAPAGE_END(bk));
  txnid_t a = meta->mm_txnid_a;
  txnid_t b = meta->mm_txnid_b;
  if (allow_volatile)
    return (a == b) ? a : 0;
  mdbx_assert(bk, a == b);
  return a;
}

static inline txnid_t meta_txnid_stable(const MDBX_milieu *bk,
                                        const meta_t *meta) {
  return meta_txnid(bk, meta, false);
}

static inline txnid_t meta_txnid_fluid(const MDBX_milieu *bk,
                                       const meta_t *meta) {
  return meta_txnid(bk, meta, true);
}

static inline void meta_update_begin(const MDBX_milieu *bk, meta_t *meta,
                                     txnid_t txnid) {
  mdbx_assert(bk, meta >= metapage(bk, 0) || meta < METAPAGE_END(bk));
  mdbx_assert(bk, meta->mm_txnid_a < txnid && meta->mm_txnid_b < txnid);
  meta->mm_txnid_a = txnid;
  (void)bk;
  mdbx_coherent_barrier();
}

static inline void meta_update_end(const MDBX_milieu *bk, meta_t *meta,
                                   txnid_t txnid) {
  mdbx_assert(bk, meta >= metapage(bk, 0) || meta < METAPAGE_END(bk));
  mdbx_assert(bk, meta->mm_txnid_a == txnid);
  mdbx_assert(bk, meta->mm_txnid_b < txnid);

  jitter4testing(true);
  meta->mm_txnid_b = txnid;
  mdbx_coherent_barrier();
}

static inline void meta_set_txnid(const MDBX_milieu *bk, meta_t *meta,
                                  txnid_t txnid) {
  mdbx_assert(bk, meta < metapage(bk, 0) || meta > METAPAGE_END(bk));
  meta->mm_txnid_a = txnid;
  meta->mm_txnid_b = txnid;
}

static inline uint64_t meta_sign(const meta_t *meta) {
  uint64_t sign = MDBX_DATASIGN_NONE;
#if 0 /* TODO */
  sign = hippeus_hash64(...);
#else
  (void)meta;
#endif
  /* LY: newer returns MDBX_DATASIGN_NONE or MDBX_DATASIGN_WEAK */
  return (sign > MDBX_DATASIGN_WEAK) ? sign : ~sign;
}

static inline bool meta_ot(const enum meta_choise_mode mode,
                           const MDBX_milieu *bk, const meta_t *a,
                           const meta_t *b) {
  jitter4testing(true);
  txnid_t txnid_a = meta_txnid_fluid(bk, a);
  txnid_t txnid_b = meta_txnid_fluid(bk, b);
  if (txnid_a == txnid_b)
    return META_IS_STEADY(b) || (META_IS_WEAK(a) && !META_IS_WEAK(a));

  jitter4testing(true);
  switch (mode) {
  default:
    assert(false);
  /* fall through */
  case prefer_steady:
    if (META_IS_STEADY(a) != META_IS_STEADY(b))
      return META_IS_STEADY(b);
  /* fall through */
  case prefer_noweak:
    if (META_IS_WEAK(a) != META_IS_WEAK(b))
      return !META_IS_WEAK(b);
  /* fall through */
  case prefer_last:
    jitter4testing(true);
    return txnid_a < txnid_b;
  }
}

static inline bool meta_eq(const MDBX_milieu *bk, const meta_t *a,
                           const meta_t *b) {
  jitter4testing(true);
  const txnid_t txnid = meta_txnid_fluid(bk, a);
  if (!txnid || txnid != meta_txnid_fluid(bk, b))
    return false;

  jitter4testing(true);
  if (META_IS_STEADY(a) != META_IS_STEADY(b))
    return false;

  jitter4testing(true);
  return true;
}

static int meta_eq_mask(const MDBX_milieu *bk) {
  meta_t *m0 = metapage(bk, 0);
  meta_t *m1 = metapage(bk, 1);
  meta_t *m2 = metapage(bk, 2);

  int rc = meta_eq(bk, m0, m1) ? 1 : 0;
  if (meta_eq(bk, m1, m2))
    rc += 2;
  if (meta_eq(bk, m2, m0))
    rc += 4;
  return rc;
}

static inline meta_t *meta_recent(const enum meta_choise_mode mode,
                                  const MDBX_milieu *bk, meta_t *a, meta_t *b) {
  const bool a_older_that_b = meta_ot(mode, bk, a, b);
  mdbx_assert(bk, !meta_eq(bk, a, b));
  return a_older_that_b ? b : a;
}

static inline meta_t *meta_ancient(const enum meta_choise_mode mode,
                                   const MDBX_milieu *bk, meta_t *a,
                                   meta_t *b) {
  const bool a_older_that_b = meta_ot(mode, bk, a, b);
  mdbx_assert(bk, !meta_eq(bk, a, b));
  return a_older_that_b ? a : b;
}

static inline meta_t *meta_mostrecent(const enum meta_choise_mode mode,
                                      const MDBX_milieu *bk) {
  meta_t *m0 = metapage(bk, 0);
  meta_t *m1 = metapage(bk, 1);
  meta_t *m2 = metapage(bk, 2);

  meta_t *head = meta_recent(mode, bk, m0, m1);
  head = meta_recent(mode, bk, head, m2);
  return head;
}

static __hot meta_t *meta_steady(const MDBX_milieu *bk) {
  return meta_mostrecent(prefer_steady, bk);
}

static __hot meta_t *meta_head(const MDBX_milieu *bk) {
  return meta_mostrecent(prefer_last, bk);
}

static __hot txnid_t reclaiming_detent(const MDBX_milieu *bk) {
  if (F_ISSET(bk->me_flags32, MDBX_UTTERLY_NOSYNC))
    return bk->me_current_txn->mt_txnid - 1;

  return meta_txnid_stable(bk, meta_steady(bk));
}

static const char *durable_str(const meta_t *const meta) {
  if (META_IS_WEAK(meta))
    return "Weak";
  if (META_IS_STEADY(meta))
    return (meta->mm_datasync_sign == meta_sign(meta)) ? "Steady" : "Tainted";
  return "Legacy";
}

static page_t *__cold meta_model(const MDBX_milieu *bk, page_t *model,
                                 unsigned num) {

  mdbx_ensure(bk, is_power_of_2(bk->me_psize));
  mdbx_ensure(bk, bk->me_psize >= MIN_PAGESIZE);
  mdbx_ensure(bk, bk->me_psize <= MAX_PAGESIZE);
  mdbx_ensure(bk, bk->me_bookgeo.lower >= MIN_MAPSIZE);
  mdbx_ensure(bk, bk->me_bookgeo.upper <= MAX_MAPSIZE);
  mdbx_ensure(bk, bk->me_bookgeo.now >= bk->me_bookgeo.lower);
  mdbx_ensure(bk, bk->me_bookgeo.now <= bk->me_bookgeo.upper);

  memset(model, 0, sizeof(*model));
  model->mp_pgno = num;
  model->mp_flags16 = P_META;
  model->mp_meta.mm_magic_and_version = MDBX_DATA_MAGIC;

  model->mp_meta.mm_geo.lower = bytes2pgno(bk, bk->me_bookgeo.lower);
  model->mp_meta.mm_geo.upper = bytes2pgno(bk, bk->me_bookgeo.upper);
  model->mp_meta.mm_geo.grow16 = (uint16_t)bytes2pgno(bk, bk->me_bookgeo.grow);
  model->mp_meta.mm_geo.shrink16 =
      (uint16_t)bytes2pgno(bk, bk->me_bookgeo.shrink);
  model->mp_meta.mm_geo.now = bytes2pgno(bk, bk->me_bookgeo.now);
  model->mp_meta.mm_geo.next = NUM_METAS;

  mdbx_ensure(bk, model->mp_meta.mm_geo.lower >= MIN_PAGENO);
  mdbx_ensure(bk, model->mp_meta.mm_geo.upper <= MAX_PAGENO);
  mdbx_ensure(bk, model->mp_meta.mm_geo.now >= model->mp_meta.mm_geo.lower);
  mdbx_ensure(bk, model->mp_meta.mm_geo.now <= model->mp_meta.mm_geo.upper);
  mdbx_ensure(bk, model->mp_meta.mm_geo.next >= MIN_PAGENO);
  mdbx_ensure(bk, model->mp_meta.mm_geo.next <= model->mp_meta.mm_geo.now);
  mdbx_ensure(bk, model->mp_meta.mm_geo.grow16 ==
                      bytes2pgno(bk, bk->me_bookgeo.grow));
  mdbx_ensure(bk, model->mp_meta.mm_geo.shrink16 ==
                      bytes2pgno(bk, bk->me_bookgeo.shrink));

  model->mp_meta.mm_psize32 = bk->me_psize;
  model->mp_meta.mm_flags16 =
      (uint16_t)(bk->me_flags32 & MDBX_DB_FLAGS) |
      MDBX_INTEGERKEY /* this is mm_aas[GACO_AAH].aa_flags */;

  model->mp_meta.mm_aas[MDBX_GACO_AAH].aa_root = P_INVALID;
  model->mp_meta.mm_aas[MDBX_MAIN_AAH].aa_root = P_INVALID;
  meta_set_txnid(bk, &model->mp_meta, MIN_TXNID + num);
  model->mp_meta.mm_datasync_sign = meta_sign(&model->mp_meta);
  return (page_t *)((uint8_t *)model + bk->me_psize);
}

/* Fill in most of the zeroed meta-pages for an empty database databook.
 * Return pointer to recenly (head) meta-page. */
static page_t *__cold init_metas(const MDBX_milieu *bk, void *buffer) {
  page_t *page0 = (page_t *)buffer;
  page_t *page1 = meta_model(bk, page0, 0);
  page_t *page2 = meta_model(bk, page1, 1);
  meta_model(bk, page2, 2);
  page2->mp_meta.mm_datasync_sign = MDBX_DATASIGN_WEAK;
  mdbx_assert(bk, !meta_eq(bk, &page0->mp_meta, &page1->mp_meta));
  mdbx_assert(bk, !meta_eq(bk, &page1->mp_meta, &page2->mp_meta));
  mdbx_assert(bk, !meta_eq(bk, &page2->mp_meta, &page0->mp_meta));
  return page1;
}
