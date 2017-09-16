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

/* PREAMBLE FOR WINDOWS:
 *
 * We are not concerned for performance here.
 * If you are running Windows a performance could NOT be the goal.
 * Otherwise please use Linux.
 *
 * Regards,
 * LY
 */

/*----------------------------------------------------------------------------*/
/* rthc */

static CRITICAL_SECTION rthc_critical_section;

static void NTAPI tls_callback(PVOID module, DWORD reason, PVOID reserved) {
  (void)module;
  (void)reserved;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    InitializeCriticalSection(&rthc_critical_section);
    break;
  case DLL_PROCESS_DETACH:
    DeleteCriticalSection(&rthc_critical_section);
    break;

  case DLL_THREAD_ATTACH:
    break;
  case DLL_THREAD_DETACH:
    rthc_cleanup();
    break;
  }
}

void rthc_lock(void) { EnterCriticalSection(&rthc_critical_section); }

void rthc_unlock(void) { LeaveCriticalSection(&rthc_critical_section); }

/* *INDENT-OFF* */
/* clang-format off */
#if defined(_MSC_VER)
#  pragma const_seg(push)
#  pragma data_seg(push)

#  ifdef _WIN64
     /* kick a linker to create the TLS directory if not already done */
#    pragma comment(linker, "/INCLUDE:_tls_used")
     /* Force some symbol references. */
#    pragma comment(linker, "/INCLUDE:mdbx_tls_callback")
     /* specific const-segment for WIN64 */
#    pragma const_seg(".CRT$XLB")
     const
#  else
     /* kick a linker to create the TLS directory if not already done */
#    pragma comment(linker, "/INCLUDE:__tls_used")
     /* Force some symbol references. */
#    pragma comment(linker, "/INCLUDE:_mdbx_tls_callback")
     /* specific data-segment for WIN32 */
#    pragma data_seg(".CRT$XLB")
#  endif

   PIMAGE_TLS_CALLBACK mdbx_tls_callback = tls_callback;
#  pragma data_seg(pop)
#  pragma const_seg(pop)

#elif defined(__GNUC__)
#  ifdef _WIN64
     const
#  endif
   PIMAGE_TLS_CALLBACK mdbx_tls_callback __attribute__((section(".CRT$XLB"), used))
     = tls_callback;
#else
#  error FIXME
#endif
/* *INDENT-ON* */
/* clang-format on */

/*----------------------------------------------------------------------------*/

#define LCK_SHARED 0
#define LCK_EXCLUSIVE LOCKFILE_EXCLUSIVE_LOCK
#define LCK_WAITFOR 0
#define LCK_DONTWAIT LOCKFILE_FAIL_IMMEDIATELY

static inline BOOL flock(MDBX_filehandle_t fd, DWORD flags, uint64_t offset,
                         size_t bytes) {
  OVERLAPPED ov;
  ov.hEvent = 0;
  ov.Offset = (DWORD)offset;
  ov.OffsetHigh = HIGH_DWORD(offset);
  return LockFileEx(fd, flags, 0, (DWORD)bytes, HIGH_DWORD(bytes), &ov);
}

static inline BOOL funlock(MDBX_filehandle_t fd, uint64_t offset,
                           size_t bytes) {
  return UnlockFile(fd, (DWORD)offset, HIGH_DWORD(offset), (DWORD)bytes,
                    HIGH_DWORD(bytes));
}

/*----------------------------------------------------------------------------*/
/* global `write` lock for write-txt processing,
 * exclusive locking both meta-pages) */

#define LCK_MAXLEN (1u + (size_t)(MAXSSIZE_T))
#define LCK_META_OFFSET 0
#define LCK_META_LEN 0x10000u
#define LCK_BODY_OFFSET LCK_META_LEN
#define LCK_BODY_LEN (LCK_MAXLEN - LCK_BODY_OFFSET + 1u)
#define LCK_META LCK_META_OFFSET, LCK_META_LEN
#define LCK_BODY LCK_BODY_OFFSET, LCK_BODY_LEN
#define LCK_WHOLE 0, LCK_MAXLEN

static int tn_lock(MDBX_milieu *bk) {
  if (flock(bk->me_dxb_fd, LCK_EXCLUSIVE | LCK_WAITFOR, LCK_BODY))
    return MDBX_SUCCESS;
  return GetLastError();
}

static void tn_unlock(MDBX_milieu *bk) {
  if (!funlock(bk->me_dxb_fd, LCK_BODY))
    mdbx_panic("%s failed: errcode %u", mdbx_func_, GetLastError());
}

/*----------------------------------------------------------------------------*/
/* global `read` lock for readers registration,
 * exclusive locking `li_numreaders` (second) cacheline */

#define LCK_LO_OFFSET 0
#define LCK_LO_LEN offsetof(MDBX_lockinfo, li_numreaders)
#define LCK_UP_OFFSET LCK_LO_LEN
#define LCK_UP_LEN (MDBX_LOCKINFO_WHOLE_SIZE - LCK_UP_OFFSET)
#define LCK_LOWER LCK_LO_OFFSET, LCK_LO_LEN
#define LCK_UPPER LCK_UP_OFFSET, LCK_UP_LEN

int mdbx_rdt_lock(MDBX_milieu *bk) {
  if (bk->me_lck_fd == INVALID_HANDLE_VALUE)
    return MDBX_SUCCESS; /* readonly database in readonly filesystem */

  /* transite from S-? (used) to S-E (locked), e.g. exclusive lock upper-part */
  if (flock(bk->me_lck_fd, LCK_EXCLUSIVE | LCK_WAITFOR, LCK_UPPER))
    return MDBX_SUCCESS;
  return GetLastError();
}

void mdbx_rdt_unlock(MDBX_milieu *bk) {
  if (bk->me_lck_fd != INVALID_HANDLE_VALUE) {
    /* transite from S-E (locked) to S-? (used), e.g. unlock upper-part */
    if (!funlock(bk->me_lck_fd, LCK_UPPER))
      mdbx_panic("%s failed: errcode %u", mdbx_func_, GetLastError());
  }
}

/*----------------------------------------------------------------------------*/
/* global `initial` lock for lockfile initialization,
 * exclusive/shared locking first cacheline */

/* FIXME: locking schema/algo descritpion.
 ?-?  = free
 S-?  = used
 E-?  = exclusive-read
 ?-S
 ?-E  = middle
 S-S
 S-E  = locked
 E-S
 E-E  = exclusive-write
*/

static int lck_init(MDBX_milieu *bk) {
  (void)bk;
  return MDBX_SUCCESS;
}

/* Seize state as 'exclusive-write' (E-E and returns MDBX_RESULT_TRUE)
 * or as 'used' (S-? and returns MDBX_RESULT_FALSE), otherwise returns an error
 */
static int internal_seize_lck(HANDLE lfd) {
  int rc;
  assert(lfd != INVALID_HANDLE_VALUE);

  /* 1) now on ?-? (free), get ?-E (middle) */
  mdbx_jitter4testing(false);
  if (!flock(lfd, LCK_EXCLUSIVE | LCK_WAITFOR, LCK_UPPER)) {
    rc = GetLastError() /* 2) something went wrong, give up */;
    mdbx_error("%s(%s) failed: errcode %u", mdbx_func_,
               "?-?(free) >> ?-E(middle)", rc);
    return rc;
  }

  /* 3) now on ?-E (middle), try E-E (exclusive-write) */
  mdbx_jitter4testing(false);
  if (flock(lfd, LCK_EXCLUSIVE | LCK_DONTWAIT, LCK_LOWER))
    return MDBX_RESULT_TRUE /* 4) got E-E (exclusive-write), done */;

  /* 5) still on ?-E (middle) */
  rc = GetLastError();
  mdbx_jitter4testing(false);
  if (rc != ERROR_SHARING_VIOLATION && rc != ERROR_LOCK_VIOLATION) {
    /* 6) something went wrong, give up */
    if (!funlock(lfd, LCK_UPPER))
      mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
                 "?-E(middle) >> ?-?(free)", GetLastError());
    return rc;
  }

  /* 7) still on ?-E (middle), try S-E (locked) */
  mdbx_jitter4testing(false);
  rc = flock(lfd, LCK_SHARED | LCK_DONTWAIT, LCK_LOWER) ? MDBX_RESULT_FALSE
                                                        : GetLastError();

  mdbx_jitter4testing(false);
  if (rc != MDBX_RESULT_FALSE)
    mdbx_error("%s(%s) failed: errcode %u", mdbx_func_,
               "?-E(middle) >> S-E(locked)", rc);

  /* 8) now on S-E (locked) or still on ?-E (middle),
  *    transite to S-? (used) or ?-? (free) */
  if (!funlock(lfd, LCK_UPPER))
    mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
               "X-E(locked/middle) >> X-?(used/free)", GetLastError());

  /* 9) now on S-? (used, DONE) or ?-? (free, FAILURE) */
  return rc;
}

static int lck_seize(MDBX_milieu *bk) {
  int rc;

  assert(bk->me_dxb_fd != INVALID_HANDLE_VALUE);
  if (bk->me_lck_fd == INVALID_HANDLE_VALUE) {
    /* LY: without-lck mode (e.g. on read-only filesystem) */
    mdbx_jitter4testing(false);
    if (!flock(bk->me_dxb_fd, LCK_SHARED | LCK_DONTWAIT, LCK_WHOLE)) {
      rc = GetLastError();
      mdbx_error("%s(%s) failed: errcode %u", mdbx_func_, "without-lck", rc);
      return rc;
    }
    return MDBX_RESULT_FALSE;
  }

  rc = internal_seize_lck(bk->me_lck_fd);
  mdbx_jitter4testing(false);
  if (rc == MDBX_RESULT_TRUE && (bk->me_flags32 & MDBX_RDONLY) == 0) {
    /* Check that another process don't operates in without-lck mode.
     * Doing such check by exclusive locking the body-part of db. Should be
     * noted:
     *  - we need an exclusive lock for do so;
     *  - we can't lock meta-pages, otherwise other process could get an error
     *    while opening db in valid (non-conflict) mode. */
    if (!flock(bk->me_dxb_fd, LCK_EXCLUSIVE | LCK_DONTWAIT, LCK_BODY)) {
      rc = GetLastError();
      mdbx_error("%s(%s) failed: errcode %u", mdbx_func_,
                 "lock-against-without-lck", rc);
      mdbx_jitter4testing(false);
      mdbx_lck_destroy(bk);
    } else {
      mdbx_jitter4testing(false);
      if (!funlock(bk->me_dxb_fd, LCK_BODY))
        mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
                   "unlock-against-without-lck", GetLastError());
    }
  }

  return rc;
}

int mdbx_lck_downgrade(MDBX_milieu *bk, bool complete) {
  /* Transite from exclusive state (E-?) to used (S-?) */
  assert(bk->me_dxb_fd != INVALID_HANDLE_VALUE);
  assert(bk->me_lck_fd != INVALID_HANDLE_VALUE);

  /* 1) must be at E-E (exclusive-write) */
  if (!complete) {
    /* transite from E-E to E_? (exclusive-read) */
    if (!funlock(bk->me_lck_fd, LCK_UPPER))
      mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
                 "E-E(exclusive-write) >> E-?(exclusive-read)", GetLastError());
    return MDBX_SUCCESS /* 2) now at E-? (exclusive-read), done */;
  }

  /* 3) now at E-E (exclusive-write), transite to ?_E (middle) */
  if (!funlock(bk->me_lck_fd, LCK_LOWER))
    mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
               "E-E(exclusive-write) >> ?-E(middle)", GetLastError());

  /* 4) now at ?-E (middle), transite to S-E (locked) */
  if (!flock(bk->me_lck_fd, LCK_SHARED | LCK_DONTWAIT, LCK_LOWER)) {
    int rc = GetLastError() /* 5) something went wrong, give up */;
    mdbx_error("%s(%s) failed: errcode %u", mdbx_func_,
               "?-E(middle) >> S-E(locked)", rc);
    return rc;
  }

  /* 6) got S-E (locked), continue transition to S-? (used) */
  if (!funlock(bk->me_lck_fd, LCK_UPPER))
    mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
               "S-E(locked) >> S-?(used)", GetLastError());

  return MDBX_SUCCESS /* 7) now at S-? (used), done */;
}

int mdbx_lck_upgrade(MDBX_milieu *bk) {
  /* Transite from locked state (S-E) to exclusive-write (E-E) */
  assert(bk->me_dxb_fd != INVALID_HANDLE_VALUE);
  assert(bk->me_lck_fd != INVALID_HANDLE_VALUE);

  /* 1) must be at S-E (locked), transite to ?_E (middle) */
  if (!funlock(bk->me_lck_fd, LCK_LOWER))
    mdbx_panic("%s(%s) failed: errcode %u", mdbx_func_,
               "S-E(locked) >> ?-E(middle)", GetLastError());

  /* 3) now on ?-E (middle), try E-E (exclusive-write) */
  mdbx_jitter4testing(false);
  if (flock(bk->me_lck_fd, LCK_EXCLUSIVE | LCK_DONTWAIT, LCK_LOWER))
    return MDBX_RESULT_TRUE; /* 4) got E-E (exclusive-write), done */

  /* 5) still on ?-E (middle) */
  int rc = GetLastError();
  mdbx_jitter4testing(false);
  if (rc != ERROR_SHARING_VIOLATION && rc != ERROR_LOCK_VIOLATION) {
    /* 6) something went wrong, report but continue */
    mdbx_error("%s(%s) failed: errcode %u", mdbx_func_,
               "?-E(middle) >> E-E(exclusive-write)", rc);
  }

  /* 7) still on ?-E (middle), try restore S-E (locked) */
  mdbx_jitter4testing(false);
  rc = flock(bk->me_lck_fd, LCK_SHARED | LCK_DONTWAIT, LCK_LOWER)
           ? MDBX_RESULT_FALSE
           : GetLastError();

  mdbx_jitter4testing(false);
  if (rc != MDBX_RESULT_FALSE) {
    mdbx_fatal("%s(%s) failed: errcode %u", mdbx_func_,
               "?-E(middle) >> S-E(locked)", rc);
    return rc;
  }

  /* 8) now on S-E (locked) */
  return MDBX_RESULT_FALSE;
}

void mdbx_lck_destroy(MDBX_milieu *bk) {
  int rc;

  if (bk->me_lck_fd != INVALID_HANDLE_VALUE) {
    /* double `unlock` for robustly remove overlapped shared/exclusive locks */
    while (funlock(bk->me_lck_fd, LCK_LOWER))
      ;
    rc = GetLastError();
    assert(rc == ERROR_NOT_LOCKED);
    (void)rc;
    SetLastError(ERROR_SUCCESS);

    while (funlock(bk->me_lck_fd, LCK_UPPER))
      ;
    rc = GetLastError();
    assert(rc == ERROR_NOT_LOCKED);
    (void)rc;
    SetLastError(ERROR_SUCCESS);
  }

  if (bk->me_dxb_fd != INVALID_HANDLE_VALUE) {
    /* explicitly unlock to avoid latency for other processes (windows kernel
     * releases such locks via deferred queues) */
    while (funlock(bk->me_dxb_fd, LCK_BODY))
      ;
    rc = GetLastError();
    assert(rc == ERROR_NOT_LOCKED);
    (void)rc;
    SetLastError(ERROR_SUCCESS);

    while (funlock(bk->me_dxb_fd, LCK_META))
      ;
    rc = GetLastError();
    assert(rc == ERROR_NOT_LOCKED);
    (void)rc;
    SetLastError(ERROR_SUCCESS);

    while (funlock(bk->me_dxb_fd, LCK_WHOLE))
      ;
    rc = GetLastError();
    assert(rc == ERROR_NOT_LOCKED);
    (void)rc;
    SetLastError(ERROR_SUCCESS);
  }
}

/*----------------------------------------------------------------------------*/
/* reader checking (by pid) */

int mdbx_rpid_set(MDBX_milieu *bk) {
  (void)bk;
  return MDBX_SUCCESS;
}

int mdbx_rpid_clear(MDBX_milieu *bk) {
  (void)bk;
  return MDBX_SUCCESS;
}

/* Checks reader by pid.
 *
 * Returns:
 *   MDBX_RESULT_TRUE, if pid is live (unable to acquire lock)
 *   MDBX_RESULT_FALSE, if pid is dead (lock acquired)
 *   or otherwise the errcode. */
int mdbx_rpid_check(MDBX_milieu *bk, MDBX_pid_t pid) {
  (void)bk;
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  int rc;
  if (hProcess) {
    rc = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);
  } else {
    rc = GetLastError();
  }

  switch (rc) {
  case ERROR_INVALID_PARAMETER:
    /* pid seem invalid */
    return MDBX_RESULT_FALSE;
  case WAIT_OBJECT_0:
    /* process just exited */
    return MDBX_RESULT_FALSE;
  case WAIT_TIMEOUT:
    /* pid running */
    return MDBX_RESULT_TRUE;
  default:
    /* failure */
    return rc;
  }
}
