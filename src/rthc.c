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

/* rthc (tls keys and destructors)
 *
 * Mostly of this code is a workaround for couple of glibc "features":
 *  - https://sourceware.org/bugzilla/show_bug.cgi?id=21031
 *  - https://sourceware.org/bugzilla/show_bug.cgi?id=21032
 */

typedef struct rthc_entry_t {
  MDBX_reader *begin;
  MDBX_reader *end;
  mdbx_thread_key_t key;
} rthc_entry_t;

#if MDBX_DEBUG
#define RTHC_INITIAL_LIMIT 1
#else
#define RTHC_INITIAL_LIMIT 16
#endif

static unsigned rthc_count;
static unsigned rthc_limit = RTHC_INITIAL_LIMIT;
static rthc_entry_t rthc_table_static[RTHC_INITIAL_LIMIT];
static rthc_entry_t *rthc_table = rthc_table_static;

/*----------------------------------------------------------------------------*/

static __cold void rthc_dtor(void *ptr) {
  MDBX_reader *rthc = (MDBX_reader *)ptr;

  rthc_lock();
  const MDBX_pid_t self_pid = mdbx_getpid();
  for (unsigned i = 0; i < rthc_count; ++i) {
    if (rthc >= rthc_table[i].begin && rthc < rthc_table[i].end) {
      if (rthc->mr_pid == self_pid) {
        rthc->mr_pid = 0;
        mdbx_coherent_barrier();
      }
      break;
    }
  }
  rthc_unlock();
}

static __cold void rthc_cleanup(void) {
  rthc_lock();
  const MDBX_pid_t self_pid = mdbx_getpid();
  for (unsigned i = 0; i < rthc_count; ++i) {
    mdbx_thread_key_t key = rthc_table[i].key;
    MDBX_reader *rthc = mdbx_thread_rthc_get(key);
    if (rthc) {
      mdbx_thread_rthc_set(key, nullptr);
      if (rthc->mr_pid == self_pid) {
        rthc->mr_pid = 0;
        mdbx_coherent_barrier();
      }
    }
  }
  rthc_unlock();
}

static __cold int rthc_alloc(mdbx_thread_key_t *key, MDBX_reader *begin,
                             MDBX_reader *end) {
#ifndef NDEBUG
  *key = (mdbx_thread_key_t)0xBADBADBAD;
#endif /* NDEBUG */
  int rc = mdbx_thread_key_create(key);
  if (rc != MDBX_SUCCESS)
    return rc;

  rthc_lock();
  if (rthc_count == rthc_limit) {
    rthc_entry_t *new_table =
        realloc((rthc_table == rthc_table_static) ? nullptr : rthc_table,
                sizeof(rthc_entry_t) * rthc_limit * 2);
    if (new_table == nullptr) {
      rc = MDBX_ENOMEM;
      goto bailout;
    }
    if (rthc_table == rthc_table_static)
      memcpy(new_table, rthc_table_static, sizeof(rthc_table_static));
    rthc_table = new_table;
    rthc_limit *= 2;
  }

  rthc_table[rthc_count].key = *key;
  rthc_table[rthc_count].begin = begin;
  rthc_table[rthc_count].end = end;
  ++rthc_count;
  rthc_unlock();
  return MDBX_SUCCESS;

bailout:
  mdbx_thread_key_delete(*key);
  rthc_unlock();
  return rc;
}

static __cold void rthc_remove(mdbx_thread_key_t key) {
  rthc_lock();
  mdbx_thread_key_delete(key);

  for (unsigned i = 0; i < rthc_count; ++i) {
    if (key == rthc_table[i].key) {
      const MDBX_pid_t self_pid = mdbx_getpid();
      for (MDBX_reader *rthc = rthc_table[i].begin; rthc < rthc_table[i].end;
           ++rthc)
        if (rthc->mr_pid == self_pid)
          rthc->mr_pid = 0;
      mdbx_coherent_barrier();
      if (--rthc_count > 0)
        rthc_table[i] = rthc_table[rthc_count];
      else if (rthc_table != rthc_table_static) {
        free(rthc_table);
        rthc_table = rthc_table_static;
        rthc_limit = RTHC_INITIAL_LIMIT;
      }
      break;
    }
  }

  rthc_unlock();
}
