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

// static uint64_t mdbx_ivo_hash(const MDBX_iov iov) {
//  /* TODO: t1ha */
//  (void)iov;
//  return 0;
//}

static bool mdbx_iov_eq(const MDBX_iov *a, const MDBX_iov *b) {
  return a->iov_len == b->iov_len &&
         memcmp(a->iov_base, b->iov_base, a->iov_len) == 0;
}

static int mdbx_iov_dup(MDBX_iov *iov) {
  if (likely(iov->iov_len)) {
    void *dup = malloc(iov->iov_len);
    if (unlikely(!dup))
      return MDBX_ENOENT;
    iov->iov_base = memcpy(dup, iov->iov_base, iov->iov_len);
  }
  return MDBX_SUCCESS;
}

static void mdbx_iov_free(MDBX_iov *iov) {
  if (likely(iov->iov_base != mdbx_droppped_name_stub)) {
    void *tmp = iov->iov_base;
    iov->iov_len = 0;
    iov->iov_base = (void *)mdbx_droppped_name_stub;
    paranoia_barrier();
    free(tmp);
  }
}
