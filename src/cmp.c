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

/* Compare two items pointing at aligned unsigned int's. */
static ptrdiff_t __hot cmp_int_aligned(const MDBX_iov a, const MDBX_iov b) {
  mdbx_assert(nullptr, a.iov_len == b.iov_len);
  mdbx_assert(nullptr, 0 == (uintptr_t)a.iov_base % sizeof(int) &&
                           0 == (uintptr_t)b.iov_base % sizeof(int));
  switch (a.iov_len) {
  case 4:
    return mdbx_cmp2int(*(uint32_t *)a.iov_base, *(uint32_t *)b.iov_base);
  case 8:
    return mdbx_cmp2int(*(uint64_t *)a.iov_base, *(uint64_t *)b.iov_base);
  default:
    mdbx_assert_fail(nullptr, "invalid size for INTEGERKEY/INTEGERDUP",
                     mdbx_func_, __LINE__);
    return 0;
  }
}

/* Compare two items pointing at 2-byte aligned unsigned int's. */
static ptrdiff_t __hot cmp_int_aligned_to2(const MDBX_iov a, const MDBX_iov b) {
  mdbx_assert(nullptr, a.iov_len == b.iov_len);
  mdbx_assert(nullptr, 0 == (uintptr_t)a.iov_base % sizeof(uint16_t) &&
                           0 == (uintptr_t)b.iov_base % sizeof(uint16_t));
#if UNALIGNED_OK
  switch (a.iov_len) {
  case 4:
    return mdbx_cmp2int(*(uint32_t *)a.iov_base, *(uint32_t *)b.iov_base);
  case 8:
    return mdbx_cmp2int(*(uint64_t *)a.iov_base, *(uint64_t *)b.iov_base);
  default:
    mdbx_assert_fail(nullptr, "invalid size for INTEGERKEY/INTEGERDUP",
                     mdbx_func_, __LINE__);
    return 0;
  }
#else
  mdbx_assert(nullptr, 0 == a.iov_len % sizeof(uint16_t));
  {
    int diff;
    const uint16_t *pa, *pb, *end;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    end = (const uint16_t *)a.iov_base;
    pa = (const uint16_t *)((char *)a.iov_base + a.iov_len);
    pb = (const uint16_t *)((char *)b.iov_base + a.iov_len);
    do {
      diff = *--pa - *--pb;
#else  /* __BYTE_ORDER__ */
    end = (const uint16_t *)((char *)a.iov_base + a.iov_len);
    pa = (const uint16_t *)a.iov_base;
    pb = (const uint16_t *)b.iov_base;
    do {
      diff = *pa++ - *pb++;
#endif /* __BYTE_ORDER__ */
      if (likely(diff != 0))
        break;
    } while (pa != end);
    return diff;
  }
#endif /* UNALIGNED_OK */
}

/* Compare two items pointing at unsigneds of unknown alignment.
 *
 * This is also set as MDBX_INTEGERDUP|MDBX_DUPFIXED's
 * runtime_auxiliary_AA_context.aa_dcmp. */
static ptrdiff_t __hot cmp_int_unaligned(const MDBX_iov a, const MDBX_iov b) {
  mdbx_assert(nullptr, a.iov_len == b.iov_len);
#if UNALIGNED_OK
  switch (a.iov_len) {
  case 4:
    return mdbx_cmp2int(*(uint32_t *)a.iov_base, *(uint32_t *)b.iov_base);
  case 8:
    return mdbx_cmp2int(*(uint64_t *)a.iov_base, *(uint64_t *)b.iov_base);
  default:
    mdbx_assert_fail(nullptr, "invalid size for INTEGERKEY/INTEGERDUP",
                     mdbx_func_, __LINE__);
    return 0;
  }
#else
  mdbx_assert(nullptr, a.iov_len == sizeof(int) || a.iov_len == sizeof(size_t));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  {
    int diff;
    const uint8_t *pa, *pb;

    pa = (const uint8_t *)a.iov_base + a.iov_len;
    pb = (const uint8_t *)b.iov_base + a.iov_len;

    do {
      diff = *--pa - *--pb;
      if (likely(diff != 0))
        break;
    } while (pa != a.iov_base);
    return diff;
  }
#else  /* __BYTE_ORDER__ */
  return memcmp(a.iov_base, b.iov_base, a.iov_len);
#endif /* __BYTE_ORDER__ */
#endif /* UNALIGNED_OK */
}

static ptrdiff_t __hot cmp_int_aligned_to2_reverse(const MDBX_iov a,
                                                   const MDBX_iov b) {
  /* FIXME: TODO */
  mdbx_ensure_msg(
      nullptr, false,
      "MDBX_DUPSORT|MDBX_REVERSEDUP|MDBX_INTEGERDUP not yet implemented");
  (void)a;
  (void)b;
  return 0;
}

static ptrdiff_t __hot cmp_int_unaligned_reverse(const MDBX_iov a,
                                                 const MDBX_iov b) {
  /* FIXME: TODO */
  mdbx_ensure_msg(
      nullptr, false,
      "MDBX_DUPSORT|MDBX_REVERSEDUP|MDBX_INTEGERDUP not yet implemented");
  (void)a;
  (void)b;
  return 0;
}

static ptrdiff_t __cold cmp_none(const MDBX_iov a, const MDBX_iov b) {
  (void)a;
  (void)b;
  return 0;
}

/* Compare two items lexically */
static ptrdiff_t __hot cmp_binstr_obverse(const MDBX_iov a, const MDBX_iov b) {
/* LY: assumes that length of keys are NOT equal for most cases,
 * if no then branch-prediction should mitigate the problem */
#if 0
  /* LY: without branch instructions on x86,
   * but isn't best for equal length of keys */
  int diff_len = mdbx_cmp2int(a.iov_len, b.iov_len);
#else
  /* LY: best when length of keys are equal,
   * but got a branch-penalty otherwise */
  if (likely(a.iov_len == b.iov_len))
    return memcmp(a.iov_base, b.iov_base, a.iov_len);
  int diff_len = (a.iov_len < b.iov_len) ? -1 : 1;
#endif
  size_t shortest = (a.iov_len < b.iov_len) ? a.iov_len : b.iov_len;
  int diff_data = memcmp(a.iov_base, b.iov_base, shortest);
  return likely(diff_data) ? diff_data : diff_len;
}

/* Compare two items in reverse byte order */
static ptrdiff_t __hot cmp_bytestr_reverse(const MDBX_iov a, const MDBX_iov b) {
  const uint8_t *pa, *pb, *end;

  pa = (const uint8_t *)a.iov_base + a.iov_len;
  pb = (const uint8_t *)b.iov_base + b.iov_len;
  size_t minlen = (a.iov_len < b.iov_len) ? a.iov_len : b.iov_len;
  end = pa - minlen;

  while (pa != end) {
    int diff = *--pa - *--pb;
    if (likely(diff))
      return diff;
  }
  return mdbx_cmp2int(a.iov_len, b.iov_len);
}

//-----------------------------------------------------------------------------

static MDBX_comparer *default_keycmp(unsigned flags) {
  if (flags & MDBX_INTEGERKEY) {
    if (flags & MDBX_REVERSEKEY)
      return cmp_int_aligned_to2_reverse;
    return cmp_int_aligned_to2;
  }
  if (flags & MDBX_REVERSEKEY)
    return cmp_bytestr_reverse;
  return cmp_binstr_obverse;
}

static MDBX_comparer *default_datacmp(unsigned flags) {
  if (flags & MDBX_DUPSORT) {
    if (flags & MDBX_REVERSEDUP) {
      if (flags & MDBX_INTEGERDUP)
        return cmp_int_unaligned_reverse;
      return cmp_bytestr_reverse;
    }
    if (flags & MDBX_INTEGERDUP)
      return cmp_int_unaligned;
    return cmp_binstr_obverse;
  }
  return cmp_none;
}
