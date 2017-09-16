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

/* Some platforms define the EOWNERDEAD error code
 * even though they don't support Robust Mutexes.
 * Compile with -DMDBX_USE_ROBUST=0. */
#ifndef MDBX_USE_ROBUST
/* Howard Chu: Android currently lacks Robust Mutex support */
#if defined(EOWNERDEAD) &&                                                     \
    !defined(ANDROID) /* LY: glibc before 2.10 has a troubles with Robust      \
                         Mutex too. */                                         \
    && __GLIBC_PREREQ(2, 10)
#define MDBX_USE_ROBUST 1
#else
#define MDBX_USE_ROBUST 0
#endif
#endif /* MDBX_USE_ROBUST */

/*----------------------------------------------------------------------------*/
/* rthc */

static pthread_mutex_t mdbx_rthc_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rthc_lock(void) {
  mdbx_ensure(NULL, pthread_mutex_lock(&mdbx_rthc_mutex) == 0);
}

static void rthc_unlock(void) {
  mdbx_ensure(NULL, pthread_mutex_unlock(&mdbx_rthc_mutex) == 0);
}

static void __attribute__((destructor)) mdbx_global_destructor(void) {
  rthc_cleanup();
}

/*----------------------------------------------------------------------------*/
/* lck */

#ifndef OFF_T_MAX
#define OFF_T_MAX (sizeof(off_t) > 4 ? INT64_MAX : INT32_MAX)
#endif
#define LCK_WHOLE OFF_T_MAX

static int mdbx_lck_op(MDBX_filehandle_t fd, int op, int lck, off_t offset,
                       off_t len) {
  for (;;) {
    int rc;
    struct flock lock_op;
    memset(&lock_op, 0, sizeof(lock_op));
    lock_op.l_type = lck;
    lock_op.l_whence = SEEK_SET;
    lock_op.l_start = offset;
    lock_op.l_len = len;
    if ((rc = fcntl(fd, op, &lock_op)) == 0) {
      if (op == F_GETLK && lock_op.l_type != F_UNLCK)
        rc = -lock_op.l_pid;
    } else if ((rc = errno) == EINTR) {
      continue;
    }
    return rc;
  }
}

static inline int mdbx_lck_exclusive(int lfd) {
  assert(lfd != MDBX_INVALID_FD);
  if (flock(lfd, LOCK_EX | LOCK_NB))
    return errno;
  return mdbx_lck_op(lfd, F_SETLK, F_WRLCK, 0, 1);
}

static inline int mdbx_lck_shared(int lfd) {
  assert(lfd != MDBX_INVALID_FD);
  while (flock(lfd, LOCK_SH)) {
    int rc = errno;
    if (rc != EINTR)
      return rc;
  }
  return mdbx_lck_op(lfd, F_SETLKW, F_RDLCK, 0, 1);
}

static int mdbx_lck_downgrade(MDBX_milieu *bk, bool complete) {
  return complete ? mdbx_lck_shared(bk->me_lck_fd) : MDBX_SUCCESS;
}

static int mdbx_lck_upgrade(MDBX_milieu *bk) {
  return mdbx_lck_exclusive(bk->me_lck_fd);
}

static int mdbx_rpid_set(MDBX_milieu *bk) {
  return mdbx_lck_op(bk->me_lck_fd, F_SETLK, F_WRLCK, bk->me_pid, 1);
}

static int mdbx_rpid_clear(MDBX_milieu *bk) {
  return mdbx_lck_op(bk->me_lck_fd, F_SETLKW, F_UNLCK, bk->me_pid, 1);
}

/* Checks reader by pid.
 *
 * Returns:
 *   MDBX_RESULT_TRUE, if pid is live (unable to acquire lock)
 *   MDBX_RESULT_FALSE, if pid is dead (lock acquired)
 *   or otherwise the errcode. */
static int mdbx_rpid_check(MDBX_milieu *bk, MDBX_pid_t pid) {
  int rc = mdbx_lck_op(bk->me_lck_fd, F_GETLK, F_WRLCK, pid, 1);
  if (rc == 0)
    return MDBX_RESULT_FALSE;
  if (rc < 0 && -rc == pid)
    return MDBX_RESULT_TRUE;
  return rc;
}

/*----------------------------------------------------------------------------*/

static int mdbx_lck_init(MDBX_milieu *bk) {
  pthread_mutexattr_t ma;
  int rc = pthread_mutexattr_init(&ma);
  if (rc)
    return rc;

  rc = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
  if (rc)
    goto bailout;

#if MDBX_USE_ROBUST
#if __GLIBC_PREREQ(2, 12)
  rc = pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
#else
  rc = pthread_mutexattr_setrobust_np(&ma, PTHREAD_MUTEX_ROBUST_NP);
#endif
  if (rc)
    goto bailout;
#endif /* MDBX_USE_ROBUST */

#if _POSIX_C_SOURCE >= 199506L
  rc = pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
  if (rc == ENOTSUP)
    rc = pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_NONE);
  if (rc)
    goto bailout;
#endif /* PTHREAD_PRIO_INHERIT */

  rc = pthread_mutex_init(&bk->me_lck->li_rmutex, &ma);
  if (rc)
    goto bailout;
  rc = pthread_mutex_init(&bk->me_lck->li_wmutex, &ma);

bailout:
  pthread_mutexattr_destroy(&ma);
  return rc;
}

static void mdbx_lck_destroy(MDBX_milieu *bk) {
  if (bk->me_lck_fd != MDBX_INVALID_FD) {
    /* try get exclusive access */
    if (bk->me_lck && mdbx_lck_exclusive(bk->me_lck_fd) == 0) {
      mdbx_info("%s: got exclusive, drown mutexes", mdbx_func_);
      int rc = pthread_mutex_destroy(&bk->me_lck->li_rmutex);
      if (rc == 0)
        rc = pthread_mutex_destroy(&bk->me_lck->li_wmutex);
      assert(rc == 0);
      (void)rc;
      /* lock would be released (by kernel) while the me_lfd will be closed */
    }
  }
}

static int mdbx_robust_lock(MDBX_milieu *bk, pthread_mutex_t *mutex) {
  int rc = pthread_mutex_lock(mutex);
  if (unlikely(rc != 0))
    rc = mutex_failed(bk, mutex, rc);
  return rc;
}

static int mdbx_robust_unlock(MDBX_milieu *bk, pthread_mutex_t *mutex) {
  int rc = pthread_mutex_unlock(mutex);
  if (unlikely(rc != 0))
    rc = mutex_failed(bk, mutex, rc);
  return rc;
}

static int mdbx_rdt_lock(MDBX_milieu *bk) {
  mdbx_trace(">>");
  int rc = mdbx_robust_lock(bk, &bk->me_lck->li_rmutex);
  mdbx_trace("<< rc %d", rc);
  return rc;
}

static void mdbx_rdt_unlock(MDBX_milieu *bk) {
  mdbx_trace(">>");
  int rc = mdbx_robust_unlock(bk, &bk->me_lck->li_rmutex);
  mdbx_trace("<< rc %d", rc);
  if (unlikely(MDBX_IS_ERROR(rc)))
    mdbx_panic("%s() failed: errcode %d\n", mdbx_func_, rc);
}

static int mdbx_tn_lock(MDBX_milieu *bk) {
  mdbx_trace(">>");
  int rc = mdbx_robust_lock(bk, &bk->me_lck->li_wmutex);
  mdbx_trace("<< rc %d", rc);
  return MDBX_IS_ERROR(rc) ? rc : MDBX_SUCCESS;
}

static void mdbx_tn_unlock(MDBX_milieu *bk) {
  mdbx_trace(">>");
  int rc = mdbx_robust_unlock(bk, &bk->me_lck->li_wmutex);
  mdbx_trace("<< rc %d", rc);
  if (unlikely(MDBX_IS_ERROR(rc)))
    mdbx_panic("%s() failed: errcode %d\n", mdbx_func_, rc);
}

static int internal_seize_lck(int lfd) {
  assert(lfd != MDBX_INVALID_FD);

  /* try exclusive access */
  int rc = mdbx_lck_exclusive(lfd);
  if (rc == 0)
    /* got exclusive */
    return MDBX_RESULT_TRUE;
  if (rc == EAGAIN || rc == EACCES || rc == EBUSY || rc == EWOULDBLOCK) {
    /* get shared access */
    rc = mdbx_lck_shared(lfd);
    if (rc == 0) {
      /* got shared, try exclusive again */
      rc = mdbx_lck_exclusive(lfd);
      if (rc == 0)
        /* now got exclusive */
        return MDBX_RESULT_TRUE;
      if (rc == EAGAIN || rc == EACCES || rc == EBUSY || rc == EWOULDBLOCK)
        /* unable exclusive, but stay shared */
        return MDBX_RESULT_FALSE;
    }
  }
  assert(MDBX_IS_ERROR(rc));
  return rc;
}

static int mdbx_lck_seize(MDBX_milieu *bk) {
  assert(bk->me_dxb_fd != MDBX_INVALID_FD);

  if (bk->me_lck_fd == MDBX_INVALID_FD) {
    /* LY: without-lck mode (e.g. on read-only filesystem) */
    int rc = mdbx_lck_op(bk->me_dxb_fd, F_SETLK, F_RDLCK, 0, LCK_WHOLE);
    if (rc != 0) {
      mdbx_error("%s(%s) failed: errcode %u", mdbx_func_, "without-lck", rc);
      return rc;
    }
    return MDBX_RESULT_FALSE;
  }

  if ((bk->me_flags32 & MDBX_RDONLY) == 0) {
    /* Check that another process don't operates in without-lck mode. */
    int rc = mdbx_lck_op(bk->me_dxb_fd, F_SETLK, F_WRLCK, bk->me_pid, 1);
    if (rc != 0) {
      mdbx_error("%s(%s) failed: errcode %u", mdbx_func_,
                 "lock-against-without-lck", rc);
      return rc;
    }
  }

  return internal_seize_lck(bk->me_lck_fd);
}

#if !__GLIBC_PREREQ(2, 12) && !defined(pthread_mutex_consistent)
#define pthread_mutex_consistent(mutex) pthread_mutex_consistent_np(mutex)
#endif

static int __cold mutex_failed(MDBX_milieu *bk, pthread_mutex_t *mutex,
                               int rc) {
#if MDBX_USE_ROBUST
  if (rc == EOWNERDEAD) {
    /* We own the mutex. Clean up after dead previous owner. */

    int rlocked = (mutex == &bk->me_lck->li_rmutex);
    rc = MDBX_SUCCESS;
    if (!rlocked) {
      if (unlikely(bk->me_txn)) {
        /* bk is hosed if the dead thread was ours */
        bk->me_flags |= MDBX_FATAL_ERROR;
        bk->me_txn = NULL;
        rc = MDBX_PANIC;
      }
    }
    mdbx_notice("%cmutex owner died, %s", (rlocked ? 'r' : 'w'),
                (rc ? "this process' bk is hosed" : "recovering"));

    int check_rc = reader_check(bk, rlocked, NULL);
    check_rc = (check_rc == MDBX_SUCCESS) ? MDBX_RESULT_TRUE : check_rc;

    int mreco_rc = pthread_mutex_consistent(mutex);
    check_rc = (mreco_rc == 0) ? check_rc : mreco_rc;

    if (unlikely(mreco_rc))
      mdbx_error("mutex recovery failed, %s", mdbx_strerror(mreco_rc));

    rc = (rc == MDBX_SUCCESS) ? check_rc : rc;
    if (MDBX_IS_ERROR(rc))
      pthread_mutex_unlock(mutex);
    return rc;
  }
#endif /* MDBX_USE_ROBUST */

  mdbx_error("mutex (un)lock failed, %s", mdbx_strerror(rc));
  if (rc != EDEADLK) {
    bk->me_flags32 |= MDBX_FATAL_ERROR;
    rc = MDBX_PANIC;
  }
  return rc;
}
