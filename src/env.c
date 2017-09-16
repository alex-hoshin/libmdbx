/*
 * Copyright 2015-2017 Leonid Yuriev <leo@yuriev.ru>
 * and other libmdbx authors: please see AUTHORS file.
 * All rights reserved.
 *
 * This code is derived from "LMDB engine" written by
 * Howard Chu (Symas Corporation), which itself derived from btree.c
 * written by Martin Hedenfalk.
 *
 * ---
 *
 * Portions Copyright 2011-2015 Howard Chu, Symas Corp. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 *
 * ---
 *
 * Portions Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include "./bits.h"
#include "./proto.h"

//-----------------------------------------------------------------------------

int __cold mdbx_reader_list(MDBX_milieu *bk, MDBX_msg_func *func, void *ctx) {
  char buf[64];
  int rc = 0, first = 1;

  if (unlikely(!bk || !func))
    return -MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  const MDBX_lockinfo *const lck = bk->me_lck;
  const unsigned snap_nreaders = lck->li_numreaders;
  for (unsigned i = 0; i < snap_nreaders; i++) {
    if (lck->li_readers[i].mr_pid) {
      const txnid_t txnid = lck->li_readers[i].mr_txnid;
      if (txnid == ~(txnid_t)0)
        snprintf(buf, sizeof(buf), "%10" PRIuPTR " %" PRIxPTR " -\n",
                 (uintptr_t)lck->li_readers[i].mr_pid,
                 (uintptr_t)lck->li_readers[i].mr_tid);
      else
        snprintf(buf, sizeof(buf), "%10" PRIuPTR " %" PRIxPTR " %" PRIaTXN "\n",
                 (uintptr_t)lck->li_readers[i].mr_pid,
                 (uintptr_t)lck->li_readers[i].mr_tid, txnid);

      if (first) {
        first = 0;
        rc = func("    pid     thread     txnid\n", ctx);
        if (rc < 0)
          break;
      }
      rc = func(buf, ctx);
      if (rc < 0)
        break;
    }
  }
  if (first)
    rc = func("(no active readers)\n", ctx);

  return rc;
}

/* Insert pid into list if not already present.
 * return -1 if already present. */
static int __cold mdbx_pid_insert(MDBX_pid_t *ids, MDBX_pid_t pid) {
  /* binary search of pid in list */
  unsigned base = 0;
  unsigned cursor = 1;
  int val = 0;
  unsigned n = ids[0];

  while (n > 0) {
    unsigned pivot = n >> 1;
    cursor = base + pivot + 1;
    val = pid - ids[cursor];

    if (val < 0) {
      n = pivot;
    } else if (val > 0) {
      base = cursor;
      n -= pivot + 1;
    } else {
      /* found, so it's a duplicate */
      return -1;
    }
  }

  if (val > 0)
    ++cursor;

  ids[0]++;
  for (n = ids[0]; n > cursor; n--)
    ids[n] = ids[n - 1];
  ids[n] = pid;
  return 0;
}

int __cold mdbx_check_readers(MDBX_milieu *bk, int *dead) {
  if (unlikely(!bk || bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EINVAL;
  if (dead)
    *dead = 0;
  return reader_check(bk, false, dead);
}

/* Return:
 *  MDBX_RESULT_TRUE - done and mutex recovered
 *  MDBX_SUCCESS     - done
 *  Otherwise errcode. */
int __cold reader_check(MDBX_milieu *bk, int rdt_locked, int *dead) {
  assert(rdt_locked >= 0);

  if (unlikely(bk->me_pid != mdbx_getpid())) {
    bk->me_flags32 |= MDBX_FATAL_ERROR;
    return MDBX_PANIC;
  }

  MDBX_lockinfo *const lck = bk->me_lck;
  const unsigned snap_nreaders = lck->li_numreaders;
  MDBX_pid_t *pids = alloca((snap_nreaders + 1) * sizeof(MDBX_pid_t));
  pids[0] = 0;

  int rc = MDBX_SUCCESS, count = 0;
  for (unsigned i = 0; i < snap_nreaders; i++) {
    const MDBX_pid_t pid = lck->li_readers[i].mr_pid;
    if (pid == 0)
      continue /* skip empty */;
    if (pid == bk->me_pid)
      continue /* skip self */;
    if (mdbx_pid_insert(pids, pid) != 0)
      continue /* such pid already processed */;

    int err = mdbx_rpid_check(bk, pid);
    if (err == MDBX_RESULT_TRUE)
      continue /* reader is live */;

    if (err != MDBX_SUCCESS) {
      rc = err;
      break /* mdbx_rpid_check() failed */;
    }

    /* stale reader found */
    if (!rdt_locked) {
      err = mdbx_rdt_lock(bk);
      if (MDBX_IS_ERROR(err)) {
        rc = err;
        break;
      }

      rdt_locked = -1;
      if (err == MDBX_RESULT_TRUE) {
        /* mutex recovered, the mutex_failed() checked all readers */
        rc = MDBX_RESULT_TRUE;
        break;
      }

      /* a other process may have clean and reused slot, recheck */
      if (lck->li_readers[i].mr_pid != pid)
        continue;

      err = mdbx_rpid_check(bk, pid);
      if (MDBX_IS_ERROR(err)) {
        rc = err;
        break;
      }

      if (err != MDBX_SUCCESS)
        continue /* the race with other process, slot reused */;
    }

    /* clean it */
    for (unsigned j = i; j < snap_nreaders; j++) {
      if (lck->li_readers[j].mr_pid == pid) {
        mdbx_debug("clear stale reader pid %" PRIuPTR " txn %" PRIaTXN "",
                   (size_t)pid, lck->li_readers[j].mr_txnid);
        lck->li_readers[j].mr_pid = 0;
        lck->li_reader_finished_flag = true;
        count++;
      }
    }
  }

  if (rdt_locked < 0)
    mdbx_rdt_unlock(bk);

  if (dead)
    *dead = count;
  return rc;
}

int __cold mdbx_set_debug(int flags, MDBX_debug_func *logger) {
  unsigned ret = mdbx_runtime_flags;
  mdbx_runtime_flags = flags;

#ifdef __linux__
  if (flags & MDBX_DBG_DUMP) {
    int core_filter_fd = open("/proc/self/coredump_filter", O_TRUNC | O_RDWR);
    if (core_filter_fd >= 0) {
      char buf[32];
      const unsigned r = pread(core_filter_fd, buf, sizeof(buf), 0);
      if (r > 0 && r < sizeof(buf)) {
        buf[r] = 0;
        unsigned long mask = strtoul(buf, nullptr, 16);
        if (mask != ULONG_MAX) {
          mask |= 1 << 3 /* Dump file-backed shared mappings */;
          mask |= 1 << 6 /* Dump shared huge pages */;
          mask |= 1 << 8 /* Dump shared DAX pages */;
          unsigned w = snprintf(buf, sizeof(buf), "0x%lx\n", mask);
          if (w > 0 && w < sizeof(buf)) {
            w = pwrite(core_filter_fd, buf, w, 0);
            (void)w;
          }
        }
      }
      close(core_filter_fd);
    }
  }
#endif /* __linux__ */

  mdbx_debug_logger = logger;
  return ret;
}

static txnid_t __cold rbr(MDBX_milieu *bk, const txnid_t laggard) {
  mdbx_debug("databook size maxed out");

  int retry;
  for (retry = 0; retry < INT_MAX; ++retry) {
    txnid_t oldest = reclaiming_detent(bk);
    mdbx_assert(bk, oldest < bk->me_wpa_txn->mt_txnid);
    mdbx_assert(bk, oldest >= laggard);
    mdbx_assert(bk, oldest >= bk->me_oldest[0]);
    if (oldest == laggard)
      return oldest;

    if (MDBX_IS_ERROR(reader_check(bk, false, nullptr)))
      break;

    MDBX_reader *const rtbl = bk->me_lck->li_readers;
    MDBX_reader *asleep = nullptr;
    for (int i = bk->me_lck->li_numreaders; --i >= 0;) {
      if (rtbl[i].mr_pid) {
        jitter4testing(true);
        const txnid_t snap = rtbl[i].mr_txnid;
        if (oldest > snap && laggard <= /* ignore pending updates */ snap) {
          oldest = snap;
          asleep = &rtbl[i];
        }
      }
    }

    if (laggard < oldest || !asleep) {
      if (retry && bk->me_callback_rbr) {
        /* LY: notify end of RBR-loop */
        const txnid_t gap = oldest - laggard;
        bk->me_callback_rbr(bk, 0, 0, laggard,
                            (gap < UINT_MAX) ? (unsigned)gap : UINT_MAX,
                            -retry);
      }
      mdbx_notice("RBR-kick: update oldest %" PRIaTXN " -> %" PRIaTXN,
                  bk->me_oldest[0], oldest);
      mdbx_assert(bk, bk->me_oldest[0] <= oldest);
      return bk->me_oldest[0] = oldest;
    }

    MDBX_tid_t tid;
    MDBX_pid_t pid;
    int rc;

    if (!bk->me_callback_rbr)
      break;

    pid = asleep->mr_pid;
    tid = asleep->mr_tid;
    if (asleep->mr_txnid != laggard || pid <= 0)
      continue;

    const txnid_t gap = meta_txnid_stable(bk, meta_head(bk)) - laggard;
    rc =
        bk->me_callback_rbr(bk, pid, tid, laggard,
                            (gap < UINT_MAX) ? (unsigned)gap : UINT_MAX, retry);
    if (rc < 0)
      break;

    if (rc) {
      asleep->mr_txnid = ~(txnid_t)0;
      bk->me_lck->li_reader_finished_flag = true;
      if (rc > 1) {
        asleep->mr_tid = 0;
        asleep->mr_pid = 0;
        mdbx_coherent_barrier();
      }
    }
  }

  if (retry && bk->me_callback_rbr) {
    /* LY: notify end of RBR-loop */
    bk->me_callback_rbr(bk, 0, 0, laggard, 0, -retry);
  }
  return find_oldest(bk->me_current_txn);
}

int __cold mdbx_set_syncbytes(MDBX_milieu *bk, size_t bytes) {
  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  if (!bk->me_lck)
    return MDBX_EPERM;

  bk->me_lck->li_autosync_threshold = bytes;
  return mdbx_bk_sync(bk, 0);
}

int __cold rbr_set(MDBX_milieu *bk, MDBX_rbr_callback *cb) {
  if (unlikely(!bk))
    return MDBX_EINVAL;

  if (unlikely(bk->me_signature != MDBX_ME_SIGNATURE))
    return MDBX_EBADSIGN;

  bk->me_callback_rbr = cb;
  return MDBX_SUCCESS;
}

//----------------------------------------------------------------------------

#ifdef __SANITIZE_ADDRESS__
LIBMDBX_API __attribute__((weak)) const char *__asan_default_options() {
  return "symbolize=1:allow_addr2line=1:"
#ifdef _DEBUG
         "debug=1:"
#endif /* _DEBUG */
         "report_globals=1:"
         "replace_str=1:replace_intrin=1:"
         "malloc_context_size=9:"
         "detect_leaks=1:"
         "check_printf=1:"
         "detect_deadlocks=1:"
#ifndef LTO_ENABLED
         "check_initialization_order=1:"
#endif
         "detect_stack_use_after_return=1:"
         "intercept_tls_get_addr=1:"
         "decorate_proc_maps=1:"
         "abort_on_error=1";
}
#endif /* __SANITIZE_ADDRESS__ */
