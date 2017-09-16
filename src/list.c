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

/* Allocate an PNL.
 * Allocates memory for an PNL of the given size.
 * Returns PNL on success, nullptr on failure. */
static MDBX_PNL mdbx_pnl_alloc(size_t size) {
  MDBX_PNL pl = malloc((size + 2) * sizeof(pgno_t));
  if (likely(pl)) {
    *pl++ = (pgno_t)size;
    *pl = 0;
  }
  return pl;
}

static MDBX_TXL mdbx_txl_alloc(void) {
  const size_t malloc_overhead = sizeof(void *) * 2;
  const size_t bytes = mdbx_roundup2(malloc_overhead + sizeof(txnid_t) * 61,
                                     MDBX_CACHELINE_SIZE) -
                       malloc_overhead;
  MDBX_TXL ptr = malloc(bytes);
  if (likely(ptr)) {
    *ptr++ = bytes / sizeof(txnid_t) - 2;
    *ptr = 0;
  }
  return ptr;
}

/* Free an PNL.
 * [in] pl The PNL to free. */
static void mdbx_pnl_free(MDBX_PNL pl) {
  if (likely(pl))
    free(pl - 1);
}

static void mdbx_txl_free(MDBX_TXL list) {
  if (likely(list))
    free(list - 1);
}

/* Append ID to PNL. The PNL must be big enough. */
static void mdbx_pnl_xappend(MDBX_PNL pl, pgno_t id) {
  assert(pl[0] + (size_t)1 < MDBX_PNL_ALLOCLEN(pl));
  pl[pl[0] += 1] = id;
}

static bool mdbx_pnl_check(MDBX_PNL pl) {
  if (pl) {
    for (const pgno_t *ptr = pl + pl[0]; --ptr > pl;) {
      assert(MDBX_PNL_ORDERED(ptr[0], ptr[1]));
      if (unlikely(MDBX_PNL_DISORDERED(ptr[0], ptr[1])))
        return false;
    }
  }
  return true;
}

/* Sort an PNL.
 * [in,out] pnl The PNL to sort. */
static void __hot mdbx_pnl_sort(MDBX_PNL pnl) {
  /* Max possible depth of int-indexed tree * 2 items/level */
  int istack[sizeof(int) * CHAR_BIT * 2];
  int i, j, k, l, ir, jstack;
  pgno_t a;

/* Quicksort + Insertion sort for small arrays */
#define PNL_SMALL 8
#define PNL_SWAP(a, b)                                                         \
  do {                                                                         \
    pgno_t tmp_pgno = (a);                                                     \
    (a) = (b);                                                                 \
    (b) = tmp_pgno;                                                            \
  } while (0)

  ir = (int)pnl[0];
  l = 1;
  jstack = 0;
  while (1) {
    if (ir - l < PNL_SMALL) { /* Insertion sort */
      for (j = l + 1; j <= ir; j++) {
        a = pnl[j];
        for (i = j - 1; i >= 1; i--) {
          if (MDBX_PNL_DISORDERED(a, pnl[i]))
            break;
          pnl[i + 1] = pnl[i];
        }
        pnl[i + 1] = a;
      }
      if (jstack == 0)
        break;
      ir = istack[jstack--];
      l = istack[jstack--];
    } else {
      k = (l + ir) >> 1; /* Choose median of left, center, right */
      PNL_SWAP(pnl[k], pnl[l + 1]);
      if (MDBX_PNL_ORDERED(pnl[ir], pnl[l]))
        PNL_SWAP(pnl[l], pnl[ir]);

      if (MDBX_PNL_ORDERED(pnl[ir], pnl[l + 1]))
        PNL_SWAP(pnl[l + 1], pnl[ir]);

      if (MDBX_PNL_ORDERED(pnl[l + 1], pnl[l]))
        PNL_SWAP(pnl[l], pnl[l + 1]);

      i = l + 1;
      j = ir;
      a = pnl[l + 1];
      while (1) {
        do
          i++;
        while (MDBX_PNL_ORDERED(pnl[i], a));
        do
          j--;
        while (MDBX_PNL_ORDERED(a, pnl[j]));
        if (j < i)
          break;
        PNL_SWAP(pnl[i], pnl[j]);
      }
      pnl[l + 1] = pnl[j];
      pnl[j] = a;
      jstack += 2;
      if (ir - i + 1 >= j - l) {
        istack[jstack] = ir;
        istack[jstack - 1] = i;
        ir = j - 1;
      } else {
        istack[jstack] = j - 1;
        istack[jstack - 1] = l;
        l = i;
      }
    }
  }
#undef PNL_SMALL
#undef PNL_SWAP
  assert(mdbx_pnl_check(pnl));
}

/* Search for an ID in an PNL.
 * [in] pl The PNL to search.
 * [in] id The ID to search for.
 * Returns The index of the first ID greater than or equal to id. */
static size_t mdbx_pnl_search(MDBX_PNL pnl, pgno_t id) {
  assert(mdbx_pnl_check(pnl));

  /* binary search of id in pl
   * if found, returns position of id
   * if not found, returns first position greater than id */
  size_t base = 0;
  size_t cursor = 1;
  int val = 0;
  size_t n = pnl[0];

  while (n > 0) {
    size_t pivot = n >> 1;
    cursor = base + pivot + 1;
    val = MDBX_PNL_ASCENDING ? mdbx_cmp2int(pnl[cursor], id)
                             : mdbx_cmp2int(id, pnl[cursor]);

    if (val < 0) {
      n = pivot;
    } else if (val > 0) {
      base = cursor;
      n -= pivot + 1;
    } else {
      return cursor;
    }
  }

  if (val > 0)
    ++cursor;

  return cursor;
}

/* Shrink an PNL.
 * Return the PNL to the default size if it has grown larger.
 * [in,out] ppl Address of the PNL to shrink. */
static void mdbx_pnl_shrink(MDBX_PNL *ppl) {
  MDBX_PNL pl = *ppl - 1;
  if (unlikely(*pl > MDBX_PNL_UM_MAX)) {
    /* shrink to MDBX_PNL_UM_MAX */
    pl = realloc(pl, (MDBX_PNL_UM_MAX + 2) * sizeof(pgno_t));
    if (likely(pl)) {
      *pl++ = MDBX_PNL_UM_MAX;
      *ppl = pl;
    }
  }
}

/* Grow an PNL.
 * Return the PNL to the size growed by given number.
 * [in,out] ppl Address of the PNL to grow. */
static int mdbx_pnl_grow(MDBX_PNL *ppl, size_t num) {
  MDBX_PNL idn = *ppl - 1;
  /* grow it */
  idn = realloc(idn, (*idn + num + 2) * sizeof(pgno_t));
  if (unlikely(!idn))
    return MDBX_ENOMEM;
  *idn++ += (pgno_t)num;
  *ppl = idn;
  return 0;
}

static int mdbx_txl_grow(MDBX_TXL *ptr, size_t num) {
  MDBX_TXL list = *ptr - 1;
  /* grow it */
  list = realloc(list, ((size_t)*list + num + 2) * sizeof(txnid_t));
  if (unlikely(!list))
    return MDBX_ENOMEM;
  *list++ += num;
  *ptr = list;
  return 0;
}

/* Make room for num additional elements in an PNL.
 * [in,out] ppl Address of the PNL.
 * [in] num Number of elements to make room for.
 * Returns 0 on success, MDBX_ENOMEM on failure. */
static int mdbx_pnl_need(MDBX_PNL *ppl, size_t num) {
  MDBX_PNL pl = *ppl;
  num += pl[0];
  if (unlikely(num > pl[-1])) {
    num = (num + num / 4 + (256 + 2)) & -256;
    pl = realloc(pl - 1, num * sizeof(pgno_t));
    if (unlikely(!pl))
      return MDBX_ENOMEM;
    *pl++ = (pgno_t)num - 2;
    *ppl = pl;
  }
  return 0;
}

/* Append an ID onto an PNL.
 * [in,out] ppl Address of the PNL to append to.
 * [in] id The ID to append.
 * Returns 0 on success, MDBX_ENOMEM if the PNL is too large. */
static int mdbx_pnl_append(MDBX_PNL *ppl, pgno_t id) {
  MDBX_PNL pl = *ppl;
  /* Too big? */
  if (unlikely(pl[0] >= pl[-1])) {
    if (mdbx_pnl_grow(ppl, MDBX_PNL_UM_MAX))
      return MDBX_ENOMEM;
    pl = *ppl;
  }
  pl[0]++;
  pl[pl[0]] = id;
  return 0;
}

static int mdbx_txl_append(MDBX_TXL *ptr, txnid_t id) {
  MDBX_TXL list = *ptr;
  /* Too big? */
  if (unlikely(list[0] >= list[-1])) {
    if (mdbx_txl_grow(ptr, (size_t)list[0]))
      return MDBX_ENOMEM;
    list = *ptr;
  }
  list[0]++;
  list[list[0]] = id;
  return 0;
}

/* Append an PNL onto an PNL.
 * [in,out] ppl Address of the PNL to append to.
 * [in] app The PNL to append.
 * Returns 0 on success, MDBX_ENOMEM if the PNL is too large. */
static int mdbx_pnl_append_list(MDBX_PNL *ppl, MDBX_PNL app) {
  MDBX_PNL pnl = *ppl;
  /* Too big? */
  if (unlikely(pnl[0] + app[0] >= pnl[-1])) {
    if (mdbx_pnl_grow(ppl, app[0]))
      return MDBX_ENOMEM;
    pnl = *ppl;
  }
  memcpy(&pnl[pnl[0] + 1], &app[1], app[0] * sizeof(pgno_t));
  pnl[0] += app[0];
  return 0;
}

static int mdbx_txl_append_list(MDBX_TXL *ptr, MDBX_TXL append) {
  MDBX_TXL list = *ptr;
  /* Too big? */
  if (unlikely(list[0] + append[0] >= list[-1])) {
    if (mdbx_txl_grow(ptr, (size_t)append[0]))
      return MDBX_ENOMEM;
    list = *ptr;
  }
  memcpy(&list[list[0] + 1], &append[1], (size_t)append[0] * sizeof(txnid_t));
  list[0] += append[0];
  return 0;
}

/* Append an ID range onto an PNL.
 * [in,out] ppl Address of the PNL to append to.
 * [in] id The lowest ID to append.
 * [in] n Number of IDs to append.
 * Returns 0 on success, MDBX_ENOMEM if the PNL is too large. */
static int mdbx_pnl_append_range(MDBX_PNL *ppl, pgno_t id, size_t n) {
  pgno_t *pnl = *ppl, len = pnl[0];
  /* Too big? */
  if (unlikely(len + n > pnl[-1])) {
    if (mdbx_pnl_grow(ppl, n | MDBX_PNL_UM_MAX))
      return MDBX_ENOMEM;
    pnl = *ppl;
  }
  pnl[0] = len + (pgno_t)n;
  pnl += len;
  while (n)
    pnl[n--] = id++;
  return 0;
}

/* Merge an PNL onto an PNL. The destination PNL must be big enough.
 * [in] pl The PNL to merge into.
 * [in] merge The PNL to merge. */
static void __hot mdbx_pnl_xmerge(MDBX_PNL pnl, MDBX_PNL merge) {
  assert(mdbx_pnl_check(pnl));
  assert(mdbx_pnl_check(merge));
  pgno_t old_id, merge_id, i = merge[0], j = pnl[0], k = i + j, total = k;
  pnl[0] =
      MDBX_PNL_ASCENDING ? 0 : ~(pgno_t)0; /* delimiter for pl scan below */
  old_id = pnl[j];
  while (i) {
    merge_id = merge[i--];
    for (; MDBX_PNL_ORDERED(merge_id, old_id); old_id = pnl[--j])
      pnl[k--] = old_id;
    pnl[k--] = merge_id;
  }
  pnl[0] = total;
  assert(mdbx_pnl_check(pnl));
}

/* Search for an ID in an ID2L.
 * [in] pnl The ID2L to search.
 * [in] id The ID to search for.
 * Returns The index of the first ID2 whose mid member is greater than
 * or equal to id. */
static unsigned __hot mdbx_mid2l_search(MDBX_ID2L pnl, pgno_t id) {
  /* binary search of id in pnl
   * if found, returns position of id
   * if not found, returns first position greater than id */
  unsigned base = 0;
  unsigned cursor = 1;
  int val = 0;
  unsigned n = (unsigned)pnl[0].mid;

  while (n > 0) {
    unsigned pivot = n >> 1;
    cursor = base + pivot + 1;
    val = mdbx_cmp2int(id, pnl[cursor].mid);

    if (val < 0) {
      n = pivot;
    } else if (val > 0) {
      base = cursor;
      n -= pivot + 1;
    } else {
      return cursor;
    }
  }

  if (val > 0)
    ++cursor;

  return cursor;
}

/* Insert an ID2 into a ID2L.
 * [in,out] pnl The ID2L to insert into.
 * [in] id The ID2 to insert.
 * Returns 0 on success, -1 if the ID was already present in the ID2L. */
static int mdbx_mid2l_insert(MDBX_ID2L pnl, MDBX_ID2 *id) {
  unsigned x = mdbx_mid2l_search(pnl, id->mid);
  if (unlikely(x < 1))
    return /* internal error */ -2;

  if (x <= pnl[0].mid && pnl[x].mid == id->mid)
    return /* duplicate */ -1;

  if (unlikely(pnl[0].mid >= MDBX_PNL_UM_MAX))
    return /* too big */ -2;

  /* insert id */
  pnl[0].mid++;
  for (unsigned i = (unsigned)pnl[0].mid; i > x; i--)
    pnl[i] = pnl[i - 1];
  pnl[x] = *id;
  return 0;
}

/* Append an ID2 into a ID2L.
 * [in,out] pnl The ID2L to append into.
 * [in] id The ID2 to append.
 * Returns 0 on success, -2 if the ID2L is too big. */
static int mdbx_mid2l_append(MDBX_ID2L pnl, MDBX_ID2 *id) {
  /* Too big? */
  if (unlikely(pnl[0].mid >= MDBX_PNL_UM_MAX))
    return -2;

  pnl[0].mid++;
  pnl[pnl[0].mid] = *id;
  return 0;
}
