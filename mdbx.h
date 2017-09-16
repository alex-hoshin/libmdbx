/* LICENSE AND COPYRUSTING *****************************************************
 *
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
 *
 * ---
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

/* ACKNOWLEDGEMENTS ************************************************************
 *
 * Howard Chu (Symas Corporation) - the author of LMDB,
 * from which originated the MDBX in 2015.
 *
 * Martin Hedenfalk <martin@bzero.se> - the author of `btree.c` code,
 * which was used for begin development of LMDB. */

#pragma once

/*******************************************************************************

  TERMINOLOGY PACT
  and related suggestions:

  1) Use "ASSOCIATIVE ARRAY" instead of "MAP".

     MDBX is a storage that operates on a memory-mapped files so the
     term "mapped" would be overambiguous/vague.  Thus, it is better
     to strictly avoid the term "map" while referring to an associative
     container to prevent confusion with memory-mapped files.

     Next, in line with https://en.wikipedia.org/wiki/Associative_array
     generalization, choose the "ASSOCIATIVE ARRAY" rather than "dictionary"
     or "associative container".


  2) Use "DATA BOOK" instead of "DATABASE".

     Unfortunately, the term "database" is badly overloaded and can assume
     too many different things.  Particularly, it can equally well apply
     to both a collection of key-value pairs and the entire container the
     former resides; as such, it is better to be avoided to prevent ambiguity.

     Since MDBX container is essentially a set of sophisticatedly handled
     data pages, the term "DATA BOOK" is appropriate.

*******************************************************************************/

#ifndef LIBMDBX_H
#define LIBMDBX_H

/* IMPENDING CHANGES WARNING ***************************************************
 *
 * Now MDBX is under active development and until November 2017 is expected a
 * big change both of API and database format.  Unfortunately those update will
 * lead to loss of compatibility with previous versions.
 *
 * The aim of this revolution in providing a clearer robust API and adding new
 * features, including the database properties. */

/*------------------------------------------------------------------------------
 * Platform-dependent defines and types */

#ifdef _MSC_VER
#pragma warning(push, 1)
#pragma warning(disable : 4548) /* expression before comma has no effect;      \
                                   expected expression with side - effect */
#pragma warning(disable : 4530) /* C++ exception handler used, but unwind      \
                                 * semantics are not enabled. Specify /EHsc */
#pragma warning(disable : 4577) /* 'noexcept' used with no exception handling  \
                                 * mode specified; termination on exception is \
                                 * not guaranteed. Specify /EHsc */
#endif                          /* _MSC_VER (warnings) */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <winnt.h>
#ifndef __mode_t_defined
typedef unsigned short mode_t;
#endif
typedef HANDLE MDBX_filehandle_t;
typedef DWORD MDBX_pid_t;
typedef DWORD MDBX_tid_t;
#else
#include <errno.h>     /* for error codes */
#include <pthread.h>   /* for pthread_t */
#include <sys/types.h> /* for pid_t */
#include <sys/uio.h>   /* for truct iovec */
#define HAVE_STRUCT_IOVEC 1
typedef int MDBX_filehandle_t;
typedef pid_t MDBX_pid_t;
typedef pthread_t MDBX_tid_t;
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef __has_attribute
#define __has_attribute(x) (0)
#endif

#ifndef __dll_export
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(__GNUC__) || __has_attribute(dllexport)
#define __dll_export __attribute__((dllexport))
#elif defined(_MSC_VER)
#define __dll_export __declspec(dllexport)
#else
#define __dll_export
#endif
#elif defined(__GNUC__) || __has_attribute(visibility)
#define __dll_export __attribute__((visibility("default")))
#else
#define __dll_export
#endif
#endif /* __dll_export */

#ifndef __dll_import
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(__GNUC__) || __has_attribute(dllimport)
#define __dll_import __attribute__((dllimport))
#elif defined(_MSC_VER)
#define __dll_import __declspec(dllimport)
#else
#define __dll_import
#endif
#else
#define __dll_import
#endif
#endif /* __dll_import */

/*------------------------------------------------------------------------------
 * Top-level MDBX defines and types */

#define MDBX_VERSION_MAJOR 0
#define MDBX_VERSION_MINOR 0

#if defined(LIBMDBX_EXPORTS)
#define LIBMDBX_API __dll_export
#elif defined(LIBMDBX_IMPORTS)
#define LIBMDBX_API __dll_import
#else
#define LIBMDBX_API
#endif /* LIBMDBX_API */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdbx_version_info {
  uint8_t major;
  uint8_t minor;
  uint16_t release;
  uint32_t revision;
  struct {
    const char *datetime;
    const char *tree;
    const char *commit;
    const char *describe;
  } git;
} mdbx_version_info;

typedef struct mdbx_build_info {
  const char *datetime;
  const char *target;
  const char *options;
  const char *compiler;
  const char *flags;
} mdbx_build_info;

extern LIBMDBX_API const mdbx_version_info mdbx_version;
extern LIBMDBX_API const mdbx_build_info mdbx_build;

/* The name of the lock file in the databook */
#define MDBX_LOCKNAME "mdbx.lck"
/* The name of the data file in the databook */
#define MDBX_DATANAME "mdbx.dat"
/* The suffix of the lock file when no subdir is used */
#define MDBX_LOCK_SUFFIX "-lck"

/* Opaque structure for a databook handle.
 *
 * A databook supports multiple associative arrays, all residing
 * in the same container (shared-memory map). */
typedef struct MDBX_milieu_ MDBX_milieu;

/* Opaque structure for a transaction handle.
 *
 * All databook operations require a transaction handle. Transactions may be
 * read-only or read-write. */
typedef struct MDBX_txn_ MDBX_txn;

/* Opaque structure for navigating through an associative array */
typedef struct MDBX_cursor_ MDBX_cursor;

/* A handle for an individual associative array in the databook.
 *
 * Briefly, AA-handles is a runtime shortcuts that provide references to a
 * named special B-tree nodes and thus allows operating with more than single
 * associative array.
 *
 * The AA-handles opens by mdbx_aa_open() and then may be closed by
 * mdbx_aa_close() or mdbx_aa_drop(MDBX_CLOSE_HANDLE) with reference counting.
 * In other words, handle still live, until mdbx_aa_close() or
 * mdbx_aa_drop(MDBX_CLOSE_HANDLE) will be called for each corresponding
 * mdbx_aa_open() for given named associative array and the it handle.
 *
 * The mdbx_aa_open() could be called within or without transaction to increase
 * reference counter for the handle of given associative array name:
 *  - When be called WITHOUT TRANSACTION, mdbx_aa_open() just create the
 *    handle for given name or increase reference counter in case such handle
 *    is already present and valid.
 *  - When be called WITHIN TRANSACTION, prior to create a handle mdbx_aa_open()
 *    will check the presence of associative array with given name or create it
 *    in case write transaction and MDBX_CREATE option.
 *
 * The MDBX_SUCCESS returned from mdbx_aa_open() indicated that the reference
 * counter for a returned handle was increased and correspondingly call
 * mdbx_aa_close() is required to dispose it. Additionally, MDBX_INTERIM flag
 * could be used for mdbx_aa_open() with write transaction to automatically
 * call mdbx_aa_close() for returned handle at end of transaction regardless
 * of it will be committed or aborted.
 *
 * In most cases is no reason to closing a handles, excepting to free a slot
 * in the handle table keeping it small for less transaction overhead. On the
 * other hand, should be noted that closing a handle with simultaneously used
 * it in transaction(s) could lead to undefined behavior, including data
 * corruption and SIGSEGV. */
typedef uint_fast32_t MDBX_aah;
#define MDBX_INVALID_AAH UINT_FAST32_MAX

/* Predefined handle for carbage collector AA. */
#define MDBX_GACO_AAH 0
/* Predefined handle for main AA. */
#define MDBX_MAIN_AAH 1

typedef struct MDBX_aah_result_ {
  int err;
  MDBX_aah aah;
} MDBX_aah_result_t;

typedef struct MDBX_txn_result_ {
  int err;
  MDBX_txn *txn;
} MDBX_txn_result_t;

typedef struct MDBX_cursor_result_ {
  int err;
  MDBX_cursor *cursor;
} MDBX_cursor_result_t;

typedef struct MDBX_milieu_result_ {
  int err;
  MDBX_milieu *milieu;
} MDBX_milieu_result_t;

typedef struct MDBX_numeric_result_ {
  int err;
  uintptr_t value;
} MDBX_numeric_result_t;

/* Generic structure used for passing keys and data in and out
 * of the engine.
 *
 * Values returned from the databook are valid only until a subsequent
 * update operation, or the end of the transaction. Do not modify or
 * free them, they commonly point into the databook itself.
 *
 * Key sizes must be between 1 and mdbx_get_maxkeysize() inclusive.
 * The same applies to data sizes in associative arrays with the MDBX_DUPSORT
 * flag. Other data items can in theory be from 0 to 0xffffffff bytes long. */
#ifdef HAVE_STRUCT_IOVEC
typedef struct iovec MDBX_iov;
#else
typedef struct iovec {
  void *iov_base;
  size_t iov_len;
} MDBX_iov;
#define HAVE_STRUCT_IOVEC
#endif /* HAVE_STRUCT_IOVEC */

/* The maximum size of a data item.
 * MDBX only store a 32 bit value for node sizes. */
#define MDBX_MAXDATASIZE INT32_MAX

/* A callback function used to compare two keys in an associative arrays */
typedef ptrdiff_t MDBX_comparer(MDBX_iov left, MDBX_iov right);

enum MDBX_flags_t
#if defined(__cplusplus) && __cplusplus >= 201103L
    : uint_least32_t
#endif /* C++11 */
{
  /*--------------------------------------------------- Associative Arrays */
  /* use reverse string keys */
  MDBX_REVERSEKEY = 1u << 0,
  /* use sorted duplicates */
  MDBX_DUPSORT = 1u << 1,
  /* numeric keys in native byte order, either uint32_t or uint64_t.
   * The keys must all be of the same size. */
  MDBX_INTEGERKEY = 1u << 2,
  /* with MDBX_DUPSORT, sorted dup items have fixed size */
  MDBX_DUPFIXED = 1u << 3,
  /* with MDBX_DUPSORT, dups are MDBX_INTEGERKEY-style integers */
  MDBX_INTEGERDUP = 1u << 4,
  /* with MDBX_DUPSORT, use reverse string dups */
  MDBX_REVERSEDUP = 1u << 5,
  /* Custom comparison flags */
  MDBX_KCMP = 1u << 6 /* custom key comparer */,
  MDBX_DCMP = 1u << 7 /* custom data comparer */,
  MDBX_AA_FLAGS = MDBX_REVERSEKEY | MDBX_DUPSORT | MDBX_INTEGERKEY |
                  MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_REVERSEDUP |
                  MDBX_KCMP | MDBX_DCMP,

  MDBX_SLD = 1 << 8 /* reserved: Separated Large Data */,
  MDBX_CRC64 = 1u << 9 /* reserved: use CRC64 */,
  MDBX_T1HA_FAST = 1u << 10 /* reserved: fast t1ha */,
  MDBX_T1HA_STRONG = MDBX_CRC64 | MDBX_T1HA_FAST /* reserved: strong t1ha */,
  MDBX_MERKLE = 1u << 11 /* reserved: verification by Merkle-tree */,
  MDBX_DB_FLAGS = MDBX_SLD | MDBX_CRC64 | MDBX_T1HA_FAST | MDBX_MERKLE,

  MDBX_INTERIM = 1u << 12 /* auto closing a handle at end of transaction */,
  MDBX_NONBLOCK = 1u << 13 /* reserved: Request lock-free guarantee */,
  MDBX_CREATE = 1u << 14 /* Create AA/DB if not already existing */,
  MDBX_OPEN_FLAGS = MDBX_CREATE | MDBX_NONBLOCK | MDBX_INTERIM,

  /* internal */
  MDBX_NOT_YET_IMPLEMENTED =
      MDBX_SLD | MDBX_CRC64 | MDBX_T1HA_FAST | MDBX_NONBLOCK | MDBX_MERKLE,

  /*---------------------------------------------------- Running regime flags */
  /* read only */
  MDBX_RDONLY = 1u << 16,
  /* read-write */
  MDBX_RDWR = 0,
  /* use writable mmap */
  MDBX_WRITEMAP = 1u << 19,
  /* tie reader locktable slots to MDBX_txn objects instead of to threads */
  MDBX_NOTLS = 1u << 21,
  /* don't do readahead */
  MDBX_NORDAHEAD = 1u << 22,
  /* LIFO policy for reclaiming GACO records */
  MDBX_LIFORECLAIM = 1u << 25,
  /* don't do any locking, caller must manage their own locks
   * WARNING: currently libmdbx don't support this mode. */
  MDBX_NOLOCK = 1u << 28,
  /* Changing those flags requires closing the
   * databook and re-opening it with the new flags. */
  MDBX_REGIME_CHANGELESS = MDBX_RDONLY | MDBX_RDWR | MDBX_WRITEMAP |
                           MDBX_NOTLS | MDBX_NORDAHEAD | MDBX_LIFORECLAIM |
                           MDBX_NOLOCK,

  /* don't fsync after commit */
  MDBX_NOSYNC = 1u << 17,
  /* don't fsync metapage after commit */
  MDBX_NOMETASYNC = 1u << 18,
  /* use asynchronous msync when MDBX_WRITEMAP is used */
  MDBX_MAPASYNC = 1u << 20,
  /* don't initialize malloc'd memory before writing to datafile */
  MDBX_NOMEMINIT = 1u << 23,
  /* aim to coalesce GACO records */
  MDBX_COALESCE = 1u << 24,
  /* make a steady-sync only on close and explicit bk-sync */
  MDBX_UTTERLY_NOSYNC = (MDBX_NOSYNC | MDBX_MAPASYNC),
  /* fill/perturb released pages */
  MDBX_PAGEPERTURB = 1u << 26,
  /* request exclusive mode, e.g. for check Databook */
  MDBX_EXCLUSIVE = 1u << 27,
  /* Those flags can be changed at runtime */
  MDBX_REGIME_CHANGEABLE =
      MDBX_NOSYNC | MDBX_NOMETASYNC | MDBX_MAPASYNC | MDBX_NOMEMINIT |
      MDBX_COALESCE | MDBX_UTTERLY_NOSYNC | MDBX_PAGEPERTURB | MDBX_EXCLUSIVE,

  /* Principal operation mode */
  MDBX_REGIME_PRINCIPAL_FLAGS = MDBX_WRITEMAP | MDBX_NOSYNC | MDBX_NOMETASYNC |
                                MDBX_MAPASYNC | MDBX_NOLOCK,

  /* Flags valid for mdbx_bk_open() */
  MDBX_ENV_FLAGS =
      MDBX_OPEN_FLAGS | MDBX_REGIME_CHANGELESS | MDBX_REGIME_CHANGEABLE,

  /*----------------------------------------------------- Internal state bits */
  /* Databook state flags */
  MDBX_ENV_TXKEY = 1u << 29 /* me_txkey is set */,
  MDBX_ENV_ACTIVE = 1u << 30 /* bk active/busy */,
  MDBX_SHRINK_ALLOWED = MDBX_ENV_ACTIVE /* only fo for mdbx_sync_locked() */,
  MDBX_FATAL_ERROR = 1u << 31 /* got a fatal error */,
  MDBX_ES_FLAGS =
      MDBX_ENV_TXKEY | MDBX_ENV_ACTIVE | MDBX_SHRINK_ALLOWED | MDBX_FATAL_ERROR,

  /* Internal transaction flags */
  MDBX_TXN_FINISHED = 1u << 0 /* txn is finished or never began */,
  MDBX_TXN_ERROR = 1u << 2 /* txn is unusable after an error */,
  MDBX_TXN_DIRTY = 1u << 3 /* must write, even if dirty list is empty */,
  MDBX_TXN_SPILLS = 1u << 4 /* txn or a parent has spilled pages */,
  MDBX_TXN_HAS_CHILD = 1u << 5 /* txn has an MDBX_txn.mt_child */,

  MDBX_TS_FLAGS = MDBX_TXN_FINISHED | MDBX_TXN_ERROR | MDBX_TXN_DIRTY |
                  MDBX_TXN_SPILLS | MDBX_TXN_HAS_CHILD,

  /* most operations on the txn are currently illegal */
  MDBX_TXN_BLOCKED = (MDBX_TXN_FINISHED | MDBX_TXN_ERROR | MDBX_TXN_HAS_CHILD),

  MDBX_TXN_BEGIN_FLAGS = (MDBX_NOMETASYNC | MDBX_NOSYNC |
                          MDBX_RDONLY) /* mask for mdbx_tn_begin() flags */,

};

/* MDBX return codes */
enum MDBX_error_t {
  /*--------------------------------------------------- Principal error codes */
  MDBX_SUCCESS = 0 /* Successful result */,
  MDBX_RESULT_FALSE = MDBX_SUCCESS,
  MDBX_RESULT_TRUE = -1 /* Successful with some significant condition */,

  MDBX_RESULT_LT = INT32_MIN,
  MDBX_RESULT_EQ = 0,
  MDBX_RESULT_GT = INT32_MAX,

  /*------------------------------------------------------ MDBX's error codes */
  MDBX_ERROR_BASE =
      -30765 /* BerkeleyDB uses -30800 to -30999, we'll go under them */,

  /* key/data pair already exists */
  MDBX_KEYEXIST,
  /* key/data pair not found (EOF) */
  MDBX_NOTFOUND,
  /* Requested page not found - this usually indicates corruption */
  MDBX_PAGE_NOTFOUND,
  /* Located page was wrong type */
  MDBX_CORRUPTED,
  /* Update of meta page failed or databook had fatal error */
  MDBX_PANIC,
  /* Databook file version mismatch with libmdbx */
  MDBX_VERSION_MISMATCH,
  /* File is not a valid MDBX file */
  MDBX_INVALID,
  /* Databook mapsize reached */
  MDBX_MAP_FULL,
  /* Databook maxdbs reached */
  MDBX_DBS_FULL,
  /* Databook maxreaders reached */
  MDBX_READERS_FULL,
  /* Txn has too many dirty pages */
  MDBX_TXN_FULL,
  /* Cursor stack too deep - internal error */
  MDBX_CURSOR_FULL,
  /* Page has not enough space - internal error */
  MDBX_PAGE_FULL,
  /* Databook contents grew beyond mapsize limit */
  MDBX_MAP_RESIZED,
  /* Operation and AA incompatible, or AA type changed. This can mean:
   *  - The operation expects an MDBX_DUPSORT / MDBX_DUPFIXED associative array.
   *  - Opening a named AA when the unnamed AA has MDBX_DUPSORT/MDBX_INTEGERKEY.
   *  - Accessing a data record as an associative arrays, or vice versa.
   *  - The associative arrays was dropped and recreated with different flags.
   */
  MDBX_INCOMPATIBLE,
  /* Invalid reuse of reader locktable slot */
  MDBX_BAD_RSLOT,
  /* Transaction must abort, has a child, or is invalid */
  MDBX_BAD_TXN,
  /* Unsupported size of key/AA name/data, or wrong DUPFIXED size */
  MDBX_BAD_VALSIZE,
  /* The specified AAH was changed unexpectedly */
  MDBX_BAD_AAH,
  /* Unexpected problem - txn should abort */
  MDBX_PROBLEM,

  /* The mdbx_put() or mdbx_replace() was called for key,
      that has more that one associated value. */
  MDBX_EMULTIVAL,

  /* Bad signature of a runtime object(s), this can mean:
   *  - memory corruption or double-free;
   *  - ABI version mismatch (rare case); */
  MDBX_EBADSIGN,

  /* Databook should be recovered, but this could NOT be done automatically
   * right now (e.g. in readonly mode and so forth). */
  MDBX_WANNA_RECOVERY,

  /* The given key value is mismatched to the current cursor position,
   * when mdbx_cursor_put() called with MDBX_IUD_CURRENT option. */
  MDBX_EKEYMISMATCH,

  /* Databook is too large for current system,
   * e.g. could NOT be mapped into RAM. */
  MDBX_TOO_LARGE,

  /* A thread has attempted to use a not owned object,
   * e.g. a transaction that started by another thread. */
  MDBX_THREAD_MISMATCH,

  /* FIXME: description */
  MDBX_EALREADY_OPENED,
  MDBX_EREFCNT_OVERFLOW,

  /* The last MDBX's error code */
  MDBX_LAST_ERRCODE,

/*---------------------------------------------- System-related error codes */
#if defined(_WIN32) || defined(_WIN64)
  MDBX_ENODATA = ERROR_HANDLE_EOF,
  MDBX_EINVAL = ERROR_INVALID_PARAMETER,
  MDBX_EACCESS = ERROR_ACCESS_DENIED,
  MDBX_ENOMEM = ERROR_OUTOFMEMORY,
  MDBX_EROFS = ERROR_FILE_READ_ONLY,
  MDBX_ENOSYS = ERROR_NOT_SUPPORTED,
  MDBX_EIO = ERROR_WRITE_FAULT,
  MDBX_EPERM = ERROR_INVALID_FUNCTION,
  MDBX_EINTR = ERROR_CANCELLED,
  MDBX_ENOENT = ERROR_FILE_NOT_FOUND,
  MDBX_EBUSY = ERROR_BUSY,
  MDBX_EWOULDBLOCK = ERROR_CANT_WAIT,
#else
  MDBX_ENODATA = ENODATA,
  MDBX_EINVAL = EINVAL,
  MDBX_EACCESS = EACCES,
  MDBX_ENOMEM = ENOMEM,
  MDBX_EROFS = EROFS,
  MDBX_ENOSYS = ENOSYS,
  MDBX_EIO = EIO,
  MDBX_EPERM = EPERM,
  MDBX_EINTR = EINTR,
  MDBX_ENOENT = ENOENT,
  MDBX_EBUSY = EBUSY,
  MDBX_EWOULDBLOCK = EWOULDBLOCK,
#endif
};

/* Information about of associative array in the databook */
typedef struct MDBX_aa_info {
  uint64_t ai_branch_pages;   /* Number of internal (non-leaf) pages */
  uint64_t ai_leaf_pages;     /* Number of leaf pages */
  uint64_t ai_overflow_pages; /* Number of overflow pages */
  uint64_t ai_entries;        /* Number of data items */
  uint64_t ai_sequence;       /* Sequrence counter */
  uint32_t ai_tree_depth;     /* Depth (height) of the B-tree */
  uint32_t ai_flags;
  MDBX_iov ai_ident;
} MDBX_aa_info;

/* Information about the databook */
typedef struct MDBX_milieu_info {
  struct {
    uint64_t lower;   /* lower limit for datafile size */
    uint64_t upper;   /* upper limit for datafile size */
    uint64_t current; /* current datafile size */
    uint64_t shrink;  /* shrink theshold for datafile */
    uint64_t grow;    /* growth step for datafile */
  } bi_geo;
  uint64_t bi_mapsize;             /* size of the memory map section */
  uint64_t bi_last_pgno;           /* ID of the last used page */
  uint64_t bi_recent_txnid;        /* ID of the last committed transaction */
  uint64_t bi_latter_reader_txnid; /* ID of the last reader transaction */
  uint64_t bi_autosync_threshold;
  uint64_t bi_dirty_volume;
  struct {
    uint64_t txnid0, sign0;
    uint64_t txnid1, sign1;
    uint64_t txnid2, sign2;
  } bi_meta;
  uint32_t bi_maxkeysize;   /* maximum size of keys and MDBX_DUPSORT data */
  uint32_t bi_pagesize;     /* databook pagesize */
  uint32_t bi_sys_pagesize; /* system pagesize */
  uint32_t bi_dxb_ioblk;    /* system ioblocksize for dxb file */
  uint32_t bi_ovf_ioblk;    /* system ioblocksize for ovf file (if used) */

  uint32_t bi_regime;
  uint32_t bi_readers_max; /* max reader slots in the databook */
  uint32_t bi_readers_num; /* max reader slots used in the databook */
} MDBX_milieu_info;

/* A callback function for killing or waiting a laggard readers. Called in case
 * of MDBX_MAP_FULL error as described below for rbr_set().
 *
 * [in] bk      An databook handle returned by mdbx_bk_init().
 * [in] pid     pid of the reader process.
 * [in] tid     thread_id of the reader thread.
 * [in] txn     Read transaction number on which stalled.
 * [in] gap     A lag from the last commited txn.
 * [in] retry   A retry number, will negative for notification
 *              about of end of RBR-loop.
 *
 * The return result is extremely important and determines further actions:
 *  <0 To break the retry cycle and immediately raise MDBX_MAP_FULL error.
 *  =0 For retry. Assumes that the callback has already made a reasonable pause
 *     or waiting, taken in account the passed arguments (mainly the retry).
 *  =1 Remove the reader's lock. Implies that reader's was aborted and will
 *     not continue execute current transaction.
 *  >1 drop reader registration (imply that reader process was already killed).
 */
typedef int(MDBX_rbr_callback)(MDBX_milieu *bk, MDBX_pid_t pid, MDBX_tid_t tid,
                               uint64_t txn, unsigned gap, int retry);

/* A callback function for most MDBX assert() failures,
 * called before printing the message and aborting.
 *
 * [in] bk   An databook handle returned by mdbx_bk_init().
 * [in] msg  The assertion message, not including newline. */
typedef void MDBX_assert_func(const MDBX_milieu *bk, const char *msg,
                              const char *function, unsigned line);

/* FIXME: Describe */
typedef void MDBX_debug_func(int type, const char *function, int line,
                             const char *msg, va_list args);

/* Auxilary runtime/environment information */
typedef struct MDBX_environment_info {
  /* FIXME: Describe */
  uint32_t ei_aah_max;
  uint32_t ei_aah_num;
  uint32_t ei_debugflags;
  void *ei_userctx;
  MDBX_debug_func *ei_debug_callback;
  MDBX_assert_func *ei_assert_callback;
  MDBX_rbr_callback *ei_rbr_callback;

  MDBX_filehandle_t ei_dxb_fd;
  MDBX_filehandle_t ei_clk_fd;
  MDBX_filehandle_t ei_ovf_fd;

  /* Address of a string pointer to contain zero-separated
   * pathnames of databook files: data (dxb), lock (lck) and
   * optional overflow (OVF) files.
   * This is the actual string in the databook, not a copy.
   * It should not be altered in any way */
  const char *ei_pathnames;
} MDBX_env_info;

/*------------------------------------------------------------------------------
 * Databook Control functions */

/* Create an initialize an MDBX databook handle.
 *
 * This function allocates memory for a MDBX_milieu structure. To release
 * the allocated memory and discard the handle, call mdbx_bk_close().
 *
 * Before the handle may be used, it must be opened using mdbx_bk_open().
 * Various other options may also need to be set before opening the handle,
 * depending on usage requirements:
 *  - mdbx_set_geometry() or mdbx_set_mapsize();
 *  - mdbx_set_maxreaders();
 *  - mdbx_set_max_handles().
 *
 * [out] bk The address where the new handle will be stored.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_bk_init(MDBX_milieu **pbk);

/* Open an databook.
 *
 * If this function fails, mdbx_bk_close() or mdbx_bk_close_ex() must be
 * called to discard the MDBX_milieu handle.
 *
 * This function creates its databook in a directory whose pathname is
 * given in the `path` argument, and creates its data and lock files
 * under that directory. For other operation modes (e.g. similar to LMDB's
 * MDB_NOSUBDIR option) advanced mdbx_bk_open_ex() API should be used.
 *
 * [in] bk     An databook handle returned by mdbx_bk_init()
 * [in] path   The directory in which the databook files (LCK and DXB) reside.
 *             This directory must already exist and be writable.
 * [in] flags  Special options for this databook. This parameter
 *             must be set to 0 or by bitwise OR'ing together one
 *             or more of the values described here.
 *
 *  - MDBX_RDONLY
 *      Open the databook in read-only mode. No write operations will
 *      be allowed. MDBX will still modify the lock file - except on
 *      read-only filesystems, where MDBX does not use locks.
 *
 *  - MDBX_WRITEMAP
 *      Use a writeable memory map unless MDBX_RDONLY is set. This uses fewer
 *      mallocs but loses protection from application bugs like wild pointer
 *      writes and other bad updates into the databook.
 *      This may be slightly faster for DBs that fit entirely in RAM,
 *      but is slower for DBs larger than RAM.
 *      Incompatible with nested transactions.
 *      Do not mix processes with and without MDBX_WRITEMAP on the same
 *      databook.  This can defeat durability (mdbx_bk_sync etc).
 *
 *  - MDBX_NOMETASYNC
 *      Flush system buffers to disk only once per transaction, omit the
 *      metadata flush. Defer that until the system flushes files to disk,
 *      or next non-MDBX_RDONLY commit or mdbx_bk_sync(). This optimization
 *      maintains databook integrity, but a system crash may undo the last
 *      committed transaction. I.e. it preserves the ACI (atomicity,
 *      consistency, isolation) but not D (durability) databook property.
 *
 *  - MDBX_NOSYNC
 *      Don't flush system buffers to disk when committing a transaction.
 *      This optimization means a system crash can corrupt the databook or
 *      lose the last transactions if buffers are not yet flushed to disk.
 *      The risk is governed by how often the system flushes dirty buffers
 *      to disk and how often mdbx_bk_sync() is called.  However, if the
 *      filesystem preserves write order and the MDBX_WRITEMAP and/or
 *      MDBX_LIFORECLAIM flags are not used, transactions exhibit ACI
 *      (atomicity, consistency, isolation) properties and only lose D
 *      (durability).  I.e. databook integrity is maintained, but a system
 *      crash may undo the final transactions.
 *
 *      Note that (MDBX_NOSYNC | MDBX_WRITEMAP) leaves the system with no
 *      hint for when to write transactions to disk.
 *      Therefore the (MDBX_MAPASYNC | MDBX_WRITEMAP) may be preferable.
 *
 *  - MDBX_UTTERLY_NOSYNC (internally MDBX_NOSYNC | MDBX_MAPASYNC)
 *      FIXME: TODO
 *
 *  - MDBX_MAPASYNC
 *      When using MDBX_WRITEMAP, use asynchronous flushes to disk. As with
 *      MDBX_NOSYNC, a system crash can then corrupt the databook or lose
 *      the last transactions. Calling mdbx_bk_sync() ensures on-disk
 *      databook integrity until next commit.
 *
 *  - MDBX_NOTLS
 *      Don't use Thread-Local Storage. Tie reader locktable slots to
 *      MDBX_txn objects instead of to threads. I.e. mdbx_tn_reset() keeps
 *      the slot reseved for the MDBX_txn object. A thread may use parallel
 *      read-only transactions. A read-only transaction may span threads if
 *      the user synchronizes its use. Applications that multiplex many
 *      user threads over individual OS threads need this option. Such an
 *      application must also serialize the write transactions in an OS
 *      thread, since MDBX's write locking is unaware of the user threads.
 *
 *  - MDBX_NOLOCK (don't supported by MDBX)
 *      Don't do any locking. If concurrent access is anticipated, the
 *      caller must manage all concurrency itself. For proper operation
 *      the caller must enforce single-writer semantics, and must ensure
 *      that no readers are using old transactions while a writer is
 *      active. The simplest approach is to use an exclusive lock so that
 *      no readers may be active at all when a writer begins.
 *
 *  - MDBX_NORDAHEAD
 *      Turn off readahead. Most operating systems perform readahead on
 *      read requests by default. This option turns it off if the OS
 *      supports it. Turning it off may help random read performance
 *      when the Databook is larger than RAM and system RAM is full.
 *
 *  - MDBX_NOMEMINIT
 *      Don't initialize malloc'd memory before writing to unused spaces
 *      in the data file. By default, memory for pages written to the data
 *      file is obtained using malloc. While these pages may be reused in
 *      subsequent transactions, freshly malloc'd pages will be initialized
 *      to zeroes before use. This avoids persisting leftover data from other
 *      code (that used the heap and subsequently freed the memory) into the
 *      data file. Note that many other system libraries may allocate and free
 *      memory from the heap for arbitrary uses. E.g., stdio may use the heap
 *      for file I/O buffers. This initialization step has a modest performance
 *      cost so some applications may want to disable it using this flag. This
 *      option can be a problem for applications which handle sensitive data
 *      like passwords, and it makes memory checkers like Valgrind noisy. This
 *      flag is not needed with MDBX_WRITEMAP, which writes directly to the
 *      mmap instead of using malloc for pages. The initialization is also
 *      skipped if MDBX_IUD_RESERVE is used; the caller is expected to overwrite
 *      all of the memory that was reserved in that case.
 *
 *  - MDBX_COALESCE
 *      Aim to coalesce records while reclaiming GACO.
 *      FIXME: TODO
 *
 *  - MDBX_LIFORECLAIM
 *      LIFO policy for reclaiming GACO records. This significantly reduce
 *      write IPOs in case MDBX_NOSYNC with periodically checkpoints.
 *      FIXME: TODO
 *
 * [in] mode4create The UNIX permissions to set on created files.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *   - MDBX_VERSION_MISMATCH - the version of the MDBX library doesn't match the
 *                             version that created the databook.
 *   - MDBX_INCOMPATIBLE     - the databook was already opened with
 *                             incompatible mode/flags, e.g. MDBX_WRITEMAP.
 *   - MDBX_INVALID  - the databook file headers are corrupted.
 *   - MDBX_ENOENT   - the directory specified by the path parameter
 *                     doesn't exist.
 *   - MDBX_EACCES   - the user didn't have permission to access
 *                     the databook files.
 *   - MDBX_EAGAIN   - the datqabook was locked by another process. */
LIBMDBX_API int mdbx_bk_open(MDBX_milieu *bk, const char *path, unsigned flags,
                             mode_t mode4create);

/* Advanced open an databook.
 *
 * In comparison to mdbx_bk_open() this function provides more control
 * and features:
 *  - create mapping with a given (fixed) address;
 *  - use specific LCK (lock) and DXB (data) files with an unrelated pathnames,
 *    e.g use a raw block devices at /dev/block;
 *  - use separate data files for large (aka overflow) pages,
 *    e.g. use a dedicated slow and cheap media for large data;
 *  - compatible opening with checking only the specific flags and accepting
 *    others, e.g. attaching to an databook which was already opened by
 *    another process with unknown mode/flags;
 *  - open databook in exclusive mode, e.g. for checking.
 *
 * In general, MDBX support a set of modes for files placement. But firstly,
 * the OVF-file should be noted:
 *  - It is optionals separate file for large data items (aka overflow pages);
 *  - Reasonably usage this dedicated file only when it placed on separate
 *    media, which implied to be cheap and slowly-seeking;
 *  - Therefore, when required it should be defined by while databook created,
 *    and then the same file must be preserved for every databook opening;
 *  - This file always defined by `ovf_pathname` argument, independently
 *    from other files and pathnames;
 *
 * Secondly, MDBX support three options for placement of data (DXB)
 * and lock (LCK) files:
 * 1. `subdir` mode: Databook files have default (predefined) names
 *    and resides in the directory, whose pathname is given by the argument.
 *    In this case `dxb_pathname` defines the directory pathname,
 *    but `lck_pathname` must be NULL;
 * 2. `no-subdir` mode: Pathname for DBX (data) file is given by the argument,
 *    and LCK (lock) file is same with MDBX_LOCK_SUFFIX appended. This is an
 *    equivalent for the LDMB's MDB_NOSUBDIR flag, which was removed from MDBX.
 *    In this case `dxb_pathname` defines DXB (data) file pathname,
 *    but `lck_pathname` MUST be just the "." string (one dot),
 *    which will be substituted as described above;
 * 3. `separate` mode: Both DXB (data) and LCK (lock) files placed
 *    independently, whose pathnames defines by two arguments.
 *    In this case both `dxb_pathname` and `lck_pathname` must be valid
 *    and `lck_pathname` must NOT be the "." (one dot).
 *
 *    NOTE: Should be avoided simultaneously use one DXB (data) file
 *    !!!!  with differ LCK (lock) files. Otherwise deadlock and/or
 *    !!!!  data corruption should be expected.
 *
 *
 * [in] bk                   - An databook handle returned by mdbx_bk_init().
 *
 * [in] target_base_address  - non-NULL value means that mapping for given
 *                             address is required, i.e. corresponding
 *                             to the LMDB's MDB_FIMEXMAP option.
 * [in] dxb_pathname         - Pathname of DXB (data) file or the directory in
 *                             which the DXB (data) and LCK (lock) files reside.
 *                             In the second case `lck_pathname` must be NULL.
 * [in] lck_pathname         - Optional pathname of LCK (lock) file. This param
 *                             should be NULL when `dxb_pathname` defines a
 *                             directory for databook files. Otherwise it
 *                             defines pathname of LCK (lock) file, which could
 *                             be placed separately.
 * [in] ovf_pathname         - The optional separate file for large (overflow)
 *                             pages. Reasonably usage this dedicated file only
 *                             when it placed on separate media. Therefore,
 *                             when required it should be defined while
 *                             databook created, and then the same file must
 *                             be preserved for every databook opening.
 * [in] regime_flags
 * [in] regime_check_mask
 * [out] regime_present
 * [in] mode4create
 */
LIBMDBX_API int mdbx_bk_open_ex(MDBX_milieu *bk, void *required_base_address,
                                const char *dxb_pathname,
                                const char *lck_pathname,
                                const char *ovf_pathname, unsigned regime_flags,
                                unsigned regime_check_mask,
                                unsigned *regime_present, mode_t mode4create);

/* Return information about the MDBX databook.
 *
 * [in]  bk         An databook handle returned by mdbx_bk_init()
 * [out] info       The optional address of an MDBX_milieu_info structure
 *                  where the information will be copied.
 * [in]  info_size  Size in bytes of MDBX_milieu_info which the caller expects.
 * [out] env        The optional address of an MDBX_env_info structure where
 *                  the runtime/environment will be copied
 * [in]  env_size   Size in bytes of MDBX_env_info which the caller expects.
 */
LIBMDBX_API int mdbx_bk_info(MDBX_milieu *bk, MDBX_milieu_info *info,
                             size_t info_size, MDBX_env_info *env,
                             size_t env_size);

/* Flush the data buffers to disk.
 *
 * Data is always written to disk when mdbx_tn_commit() is called,
 * but the operating system may keep it buffered. MDBX always flushes
 * the OS buffers upon commit as well, unless the databook was
 * opened with MDBX_NOSYNC or in part MDBX_NOMETASYNC. This call is
 * not valid if the databook was opened with MDBX_RDONLY.
 *
 * [in] bk    An databook handle returned by mdbx_bk_init()
 * [in] force If non-zero, force a synchronous flush.  Otherwise if the
 *            databook has the MDBX_NOSYNC flag set the flushes will be
 *            omitted, and with MDBX_MAPASYNC they will be asynchronous.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EACCES   - the databook is read-only.
 *  - MDBX_EINVAL   - an invalid parameter was specified.
 *  - MDBX_EIO      - an error occurred during synchronization. */
LIBMDBX_API int mdbx_bk_sync(MDBX_milieu *bk, int force);

/* Close the databook and release the memory map.
 *
 * Only a single thread may call this function. All transactions, associative
 * arrays handles, and cursors must already be closed before calling this
 * function. Attempts to use any such handles after calling this function will
 * cause a SIGSEGV. The databook handle will be freed and must not be used
 * again after this call.
 *
 * [in] bk         An databook handle returned by mdbx_bk_init()
 * [in] dont_sync  A dont'sync flag, if non-zero the last checkpoint (meta-page
 *                 update) will be kept "as is" and may be still "weak" in the
 *                 NOSYNC/MAPASYNC modes. Such "weak" checkpoint will be
 *                 ignored on opening next time, and transactions since the
 *                 last non-weak checkpoint (meta-page update) will rolledback
 *                 for consistency guarantee.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EIO      - an error occurred during synchronization.
 *  - MDBX_EINVAL   - an invalid parameter was specified.
 */
LIBMDBX_API int mdbx_bk_shutdown(MDBX_milieu *bk, int dont_sync);

/* Set the size of the memory map to use for this databook.
 *
 * The size should be a multiple of the OS page size. The default is
 * 10485760 bytes. The size of the memory map is also the maximum size
 * of the databook. The value should be chosen as large as possible,
 * to accommodate future growth of the databook.
 * This function should be called after mdbx_bk_init() and before
 * mdbx_bk_open(). It may be called at later times if no transactions
 * are active in this process. Note that the library does not check for
 * this condition, the caller must ensure it explicitly.
 *
 * The new size takes effect immediately for the current process but
 * will not be persisted to any others until a write transaction has been
 * committed by the current process. Also, only mapsize increases are
 * persisted into the databook.
 *
 * If the mapsize is increased by another process, and data has grown
 * beyond the range of the current mapsize, mdbx_tn_begin() will
 * return MDBX_MAP_RESIZED. This function may be called with a size
 * of zero to adopt the new size.
 *
 * Any attempt to set a size smaller than the space already consumed by the
 * databook will be silently changed to the current size of the used space.
 *
 * [in] bk   An databook handle returned by mdbx_bk_init()
 * [in] size  The size in bytes
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified,
 *                    or the databook has an active write transaction. */
LIBMDBX_API int mdbx_set_mapsize(MDBX_milieu *bk, size_t size);
/* FIXME: Describe */
LIBMDBX_API int mdbx_set_geometry(MDBX_milieu *bk, intptr_t size_lower,
                                  intptr_t size_now, intptr_t size_upper,
                                  intptr_t growth_step,
                                  intptr_t shrink_threshold, intptr_t pagesize);

/* Set the maximum number of threads/reader slots for the databook.
 *
 * This defines the number of slots in the lock table that is used to track
 * readers in the the databook. The default is 61.
 * Starting a read-only transaction normally ties a lock table slot to the
 * current thread until the databook closes or the thread exits. If
 * MDBX_NOTLS is in use, mdbx_tn_begin() instead ties the slot to the
 * MDBX_txn object until it or the MDBX_milieu object is destroyed.
 * This function may only be called after mdbx_bk_init() and before
 * mdbx_bk_open().
 *
 * [in] bk        An databook handle returned by mdbx_bk_init()
 * [in] readers   The maximum number of reader lock table slots
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified,
 *                    or the databook is already open. */
LIBMDBX_API int mdbx_set_maxreaders(MDBX_milieu *bk, unsigned readers);

/* Set the maximum number of named associative arrays for the databook.
 *
 * This function is only needed if multiple associative arrays will be used in
 * the databook. Simpler applications that use the databook as a single
 * unnamed associative array can ignore this option. This function may only
 * be called after mdbx_bk_init() and before mdbx_bk_open().
 *
 * Currently a moderate number of slots are cheap but a huge number gets
 * expensive: 7-120 words per transaction, and every mdbx_aa_open()
 * does a linear search of the opened slots.
 *
 * [in] bk    An databook handle returned by mdbx_bk_init()
 * [in] count The maximum number of associative arrays
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified,
 *                    or the databook is already open. */
LIBMDBX_API int mdbx_set_max_handles(MDBX_milieu *bk, MDBX_aah count);

/* Get the maximum size of keys and MDBX_DUPSORT data we can write
 * for given pagesize.
 *
 * Returns The maximum size of a key we can write. */
LIBMDBX_API int mdbx_pagesize2maxkeylen(size_t pagesize);

/* Set application information associated with the MDBX_milieu.
 *
 * [in] bk   An databook handle returned by mdbx_bk_init()
 * [in] ctx  An arbitrary pointer for whatever the application needs.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_set_userctx(MDBX_milieu *bk, void *ctx);

/* Get the application information associated with the MDBX_milieu.
 *
 * [in] bk An databook handle returned by mdbx_bk_init()
 * Returns The pointer set by mdbx_set_userctx(). */
LIBMDBX_API void *mdbx_get_userctx(MDBX_milieu *bk);

/* Set threshold to force flush the data buffers to disk,
 * even of MDBX_NOSYNC, MDBX_NOMETASYNC and MDBX_MAPASYNC flags
 * in the databook.
 *
 * Data is always written to disk when mdbx_tn_commit() is called,
 * but the operating system may keep it buffered. MDBX always flushes
 * the OS buffers upon commit as well, unless the databook was
 * opened with MDBX_NOSYNC or in part MDBX_NOMETASYNC.
 *
 * The default is 0, than mean no any threshold checked, and no additional
 * flush will be made.
 *
 * [in] bk      An databook handle returned by mdbx_bk_init()
 * [in] bytes   The size in bytes of summary changes when a synchronous
 *              flush would be made.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_set_syncbytes(MDBX_milieu *bk, size_t bytes);

/* Set the "Reclaiming Blocked by Reader" callback.
 *
 * Callback will be called on no-free-pages condition when reclaiming of GACO
 * pages is blocked by at least one read transaction for killing or waiting this
 * slow reader.
 *
 * [in] bk        An databook handle returned by mdbx_bk_init().
 * [in] cb        A MDBX_rbr_callback function or NULL to disable.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int rbr_set(MDBX_milieu *bk, MDBX_rbr_callback *cb);

/* Set or reset the assert() callback of the databook.
 *
 * Disabled if libmdbx is buillt with MDBX_DEBUG=0.
 * NOTE: This hack should become obsolete as mdbx's error handling matures.
 *
 * [in] bk   An databook handle returned by mdbx_bk_init().
 * [in] func  An MDBX_assert_func function, or 0.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_set_assert(MDBX_milieu *bk, MDBX_assert_func *func);

/* FIXME: Describe */
typedef int MDBX_walk_func(uint64_t pgno, unsigned pgnumber, void *ctx,
                           const char *aah, const char *type, size_t nentries,
                           size_t payload_bytes, size_t header_bytes,
                           size_t unused_bytes);
/* FIXME: Describe */
LIBMDBX_API int mdbx_walk(MDBX_txn *txn, MDBX_walk_func *visitor, void *ctx);

/* A callback function used to print a message from the library.
 *
 * [in] msg   The string to be printed.
 * [in] ctx   An arbitrary context pointer for the callback.
 *
 * Returns < 0 on failure, >= 0 on success. */
typedef int(MDBX_msg_func)(const char *msg, void *ctx);

/* Dump the entries in the reader lock table.
 *
 * [in] bk    An databook handle returned by mdbx_bk_init()
 * [in] func  A MDBX_msg_func function
 * [in] ctx   Anything the message function needs
 *
 * Returns < 0 on failure, >= 0 on success. */
LIBMDBX_API int mdbx_reader_list(MDBX_milieu *bk, MDBX_msg_func *func,
                                 void *ctx);

/* Check for stale entries in the reader lock table.
 *
 * [in] bk      An databook handle returned by mdbx_bk_init()
 * [out] dead   Number of stale slots that were cleared
 *
 * Returns 0 on success, non-zero on failure. */
LIBMDBX_API int mdbx_check_readers(MDBX_milieu *bk, int *dead);

/* Copy Flags */
enum MDBX_copy_flags_t
#if defined(__cplusplus) && __cplusplus >= 201103L
    : unsigned
#endif /* C++11 */
{ MDBX_CP_COMPACT = 1 /* Compacting copy: Omit free space from copy,
                       * and renumber all pages sequentially. */,
  MDBX_CP_NOSUBDIR = 2 /* no-subdir mode for destination */,
};

/* Copy an MDBX databook to the specified path, with options.
 *
 * This function may be used to make a backup of an existing databook.
 * No lockfile is created, since it gets recreated at need.
 * NOTE: This call can trigger significant file size growth if run in
 * parallel with write transactions, because it employs a read-only
 * transaction. See long-lived transactions under "Caveats" section.
 *
 * [in] bk     An databook handle returned by mdbx_bk_init(). It must
 *             have already been opened successfully.
 * [in] path   The directory in which the copy will reside. This directory
 *             must already exist and be writable but must otherwise be empty.
 * [in] flags  Special options for this operation. This parameter must be set
 *             to 0 or by bitwise OR'ing together one or more of the values
 *             described here:
 *
 *  - MDBX_CP_COMPACT
 *      Perform compaction while copying: omit free pages and sequentially
 *      renumber all pages in output. This option consumes little bit more
 *      CPU for processing, but may running quickly than the default, on
 *      account skipping free pages.
 *
 *  - MDBX_CP_NOSUBDIR
 *      NOTE: It NOT fails if the databook has suffered a page leak (FIXME?).
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_bk_copy(MDBX_milieu *bk, const char *pathname,
                             enum MDBX_copy_flags_t flags);

/* Copy an MDBX databook to the specified file descriptor with options.
 *
 * This function may be used to make a backup of an existing databook.
 * No lockfile is created, since it gets recreated at need. See
 * mdbx_bk_copy() for further details.
 *
 * NOTE: This call can trigger significant file size growth if run in
 * parallel with write transactions, because it employs a read-only
 * transaction. See long-lived transactions under "Caveats" section.
 *
 * [in] bk      An databook handle returned by mdbx_bk_init(). It must
 *              have already been opened successfully.
 * [in] fd      The filedescriptor to write the copy to. It must have already
 *              been opened for Write access.
 * [in] flags   Special options for this operation. See mdbx_bk_copy() for
 *              options.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_bk_copy2fd(MDBX_milieu *bk, MDBX_filehandle_t fd,
                                enum MDBX_copy_flags_t flags);

/*------------------------------------------------------------------------------
 * Transaction Control functions */

/* Create a transaction for use with the databook.
 *
 * The transaction handle may be discarded using mdbx_tn_abort()
 * or mdbx_tn_commit().
 * NOTE: A transaction and its cursors must only be used by a single
 * thread, and a thread may only have a single transaction at a time.
 * If MDBX_NOTLS is in use, this does not apply to read-only transactions.
 * NOTE: Cursors may not span transactions.
 *
 * [in] bk      An databook handle returned by mdbx_bk_init()
 * [in] parent  If this parameter is non-NULL, the new transaction will be
 *              a nested transaction, with the transaction indicated by parent
 *              as its parent. Transactions may be nested to any level.
 *              A parent transaction and its cursors may not issue any other
 *              operations than mdbx_tn_commit and mdbx_tn_abort while it
 *              has active child transactions.
 * [in] flags   Special options for this transaction. This parameter
 *              must be set to 0 or by bitwise OR'ing together one or more
 *              of the values described here.
 *
 *  - MDBX_RDONLY
 *      This transaction will not perform any write operations.
 *
 * [out] txn Address where the new MDBX_txn handle will be stored
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_PANIC         - a fatal error occurred earlier and the databook
  *                        must be shut down.
 *  - MDBX_MAP_RESIZED   - another process wrote data beyond this MDBX_milieu's
 *                         mapsize and this databook's map must be resized
 *                         as well. See mdbx_set_mapsize().
 *  - MDBX_READERS_FULL  - a read-only transaction was requested and the reader
 *                         lock table is full. See mdbx_set_maxreaders().
 *  - MDBX_ENOMEM        - out of memory. */
LIBMDBX_API int mdbx_tn_begin(MDBX_milieu *bk, MDBX_txn *parent, unsigned flags,
                              MDBX_txn **txn);

/* Returns the transaction's MDBX_milieu
 *
 * [in] txn  A transaction handle returned by mdbx_tn_begin() */
LIBMDBX_API MDBX_milieu *mdbx_tn_book(MDBX_txn *txn);

/* Return the transaction's ID.
 *
 * This returns the identifier associated with this transaction. For a
 * read-only transaction, this corresponds to the snapshot being read;
 * concurrent readers will frequently have the same transaction ID.
 *
 * [in] txn A transaction handle returned by mdbx_tn_begin()
 *
 * Returns A transaction ID, valid if input is an active transaction. */
LIBMDBX_API uint64_t mdbx_tn_id(MDBX_txn *txn);

/* Commit all the operations of a transaction into the databook.
 *
 * The transaction handle is freed. It and its cursors must not be used
 * again after this call, except with mdbx_cr_renew().
 *
 * A cursor must be closed explicitly always, before
 * or after its transaction ends. It can be reused with
 * mdbx_cr_renew() before finally closing it.
 *
 * [in] txn  A transaction handle returned by mdbx_tn_begin()
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified.
 *  - MDBX_ENOSPC   - no more disk space.
 *  - MDBX_EIO      - a low-level I/O error occurred while writing.
 *  - MDBX_ENOMEM   - out of memory. */
LIBMDBX_API int mdbx_tn_commit(MDBX_txn *txn);

/* Abandon all the operations of the transaction instead of saving them.
 *
 * The transaction handle is freed. It and its cursors must not be used
 * again after this call, except with mdbx_cr_renew().
 *
 * Opened AA handles will be closed. To save ones for
 *
 * A cursor must be closed explicitly always, before or after its transaction
 * ends. It can be reused with mdbx_cr_renew() before finally closing it.
 *
 * [in] txn  A transaction handle returned by mdbx_tn_begin(). */
LIBMDBX_API int mdbx_tn_abort(MDBX_txn *txn);

/* Reset a read-only transaction.
 *
 * Abort the transaction like mdbx_tn_abort(), but keep the transaction
 * handle. Therefore mdbx_tn_renew() may reuse the handle. This saves
 * allocation overhead if the process will start a new read-only transaction
 * soon, and also locking overhead if MDBX_NOTLS is in use. The reader table
 * lock is released, but the table slot stays tied to its thread or
 * MDBX_txn. Use mdbx_tn_abort() to discard a reset handle, and to free
 * its lock table slot if MDBX_NOTLS is in use.
 *
 * Cursors opened within the transaction must not be used
 * again after this call, except with mdbx_cr_renew().
 *
 * Reader locks generally don't interfere with writers, but they keep old
 * versions of databook pages allocated. Thus they prevent the old pages
 * from being reused when writers commit new data, and so under heavy load
 * the databook size may grow much more rapidly than otherwise.
 *
 * [in] txn  A transaction handle returned by mdbx_tn_begin() */
LIBMDBX_API int mdbx_tn_reset(MDBX_txn *txn);

/* Renew a read-only transaction.
 *
 * This acquires a new reader lock for a transaction handle that had been
 * released by mdbx_tn_reset(). It must be called before a reset transaction
 * may be used again.
 *
 * [in] txn  A transaction handle returned by mdbx_tn_begin()
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_PANIC     - a fatal error occurred earlier and the databook
 *                     must be shut down.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_tn_renew(MDBX_txn *txn);

/* Returns a lag of the reading for the given transaction.
 *
 * Returns an information for estimate how much given read-only
 * transaction is lagging relative to the latest version.
 *
 * [in] txn       A transaction handle returned by mdbx_tn_begin()
 * [out] percent  Percentage of page allocation in the databook.
 *
 * Returns Number of transactions committed after the given was started for
 * read, or -1 on failure. */
LIBMDBX_API int mdbx_tn_lag(MDBX_txn *txn, int *percent);

/* FIXME: Describe */
typedef struct MDBX_canary { uint64_t x, y, z, v; } MDBX_canary_t;

/* FIXME: Describe */
LIBMDBX_API int mdbx_canary_put(MDBX_txn *txn, const MDBX_canary_t *canary);
/* FIXME: Describe */
LIBMDBX_API int mdbx_canary_get(MDBX_txn *txn, MDBX_canary_t *canary);

/* FIXME: Describe */
LIBMDBX_API int mdbx_is_dirty(const MDBX_txn *txn, const void *ptr);

/* Compare two data items according to a particular associative array.
 *
 * This returns a comparison as if the two data items were keys in the
 * specified associative array.
 *
 * [in] txn   A transaction handle returned by mdbx_tn_begin()
 * [in] aah   A associative array handle returned by mdbx_aa_open()
 * [in] a     The first item to compare
 * [in] b     The second item to compare
 *
 * Returns < 0 if a < b, 0 if a == b, > 0 if a > b */
LIBMDBX_API int mdbx_cmp(MDBX_txn *txn, MDBX_aah aah, const MDBX_iov *a,
                         const MDBX_iov *b);

/* Compare two data items according to a particular associative array.
 *
 * This returns a comparison as if the two items were data items of
 * the specified associative array. The associative array must have the
 * MDBX_DUPSORT flag.
 *
 * [in] txn   A transaction handle returned by mdbx_tn_begin()
 * [in] aah   A associative array handle returned by mdbx_aa_open()
 * [in] a     The first item to compare
 * [in] b     The second item to compare
 *
 * Returns < 0 if a < b, 0 if a == b, > 0 if a > b */
LIBMDBX_API int mdbx_dcmp(MDBX_txn *txn, MDBX_aah aah, const MDBX_iov *a,
                          const MDBX_iov *b);

/*------------------------------------------------------------------------------
 * Associative Array Control functions */

/* Open a table in the databook.
 *
 * A table handle denotes the name and parameters of a table, independently
 * of whether such a table exists. The table handle may be discarded by
 * calling mdbx_aa_close(). The old table handle is returned if the table
 * was already open. The handle may only be closed once.
 *
 * The table handle will be private to the current transaction until
 * the transaction is successfully committed. If the transaction is
 * aborted the handle will be closed automatically.
 * After a successful commit the handle will reside in the shared
 * environment, and may be used by other transactions.
 *
 * FIXME: This function must not be called from multiple concurrent
 * transactions in the same process. A transaction that uses
 * this function must finish (either commit or abort) before
 * any other transaction in the process may use this function.
 *
 * To use named table (with name != NULL), mdbx_set_max_handles()
 * must be called before opening the databook. Table names are
 * keys in the internal unnamed table, and may be read but not written.
 *
 * [in] txn    transaction handle returned by mdbx_tn_begin()
 * [in] name   The name of the table to open. If only a single
 *             table is needed in the databook, this value may be NULL.
 * [in] flags  Special options for this table. This parameter must be set
 *             to 0 or by bitwise OR'ing together one or more of the values
 *             described here:
 *  - MDBX_REVERSEKEY
 *      Keys are strings to be compared in reverse order, from the end
 *      of the strings to the beginning. By default, Keys are treated as
 *      strings and compared from beginning to end.
 *  - MDBX_DUPSORT
 *      Duplicate keys may be used in the table. Or, from another point of
 *      view, keys may have multiple data items, stored in sorted order. By
 *      default keys must be unique and may have only a single data item.
 *  - MDBX_INTEGERKEY
 *      Keys are binary integers in native byte order, either uin32_t or
 *      uint64_t, and will be sorted as such. The keys must all be of the
 *      same size.
 *  - MDBX_DUPFIXED
 *      This flag may only be used in combination with MDBX_DUPSORT. This
 *      option tells the library that the data items for this associative array
 *      are all the same size, which allows further optimizations in storage and
 *      retrieval. When all data items are the same size, the MDBX_GET_MULTIPLE,
 *      MDBX_NEXT_MULTIPLE and MDBX_PREV_MULTIPLE cursor operations may be used
 *      to retrieve multiple items at once.
 *  - MDBX_INTEGERDUP
 *      This option specifies that duplicate data items are binary integers,
 *      similar to MDBX_INTEGERKEY keys.
 *  - MDBX_REVERSEDUP
 *      This option specifies that duplicate data items should be compared as
 *      strings in reverse order (the comparison is performed in the direction
 *      from the last byte to the first).
 *  - MDBX_CREATE
 *      Create the named associative array if it doesn't exist. This option is
 *      not allowed in a read-only transaction or a read-only databook.
 *
 * [out]  aah Address where the new MDBX_handle handle will be stored
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_NOTFOUND  - the specified associative array doesn't exist in the
 *                     databook and MDBX_CREATE was not specified.
 *  - MDBX_DBS_FULL  - too many associative arrays have been opened.
 *                     See mdbx_set_max_handles(). */
LIBMDBX_API MDBX_iov mdbx_str2iov(const char *str);

LIBMDBX_API int mdbx_aa_preopen(MDBX_milieu *bk, const MDBX_iov aa_ident,
                                unsigned flags, MDBX_aah *aah,
                                MDBX_comparer *keycmp, MDBX_comparer *datacmp);
LIBMDBX_API int mdbx_aa_open(MDBX_txn *txn, const MDBX_iov aa_ident,
                             unsigned flags, MDBX_aah *aah,
                             MDBX_comparer *keycmp, MDBX_comparer *datacmp);
LIBMDBX_API int mdbx_aa_addref(MDBX_milieu *bk, MDBX_aah aah);

/* Close an associative array handle. Normally unnecessary.
 *
 * Use with care:
 * FIXME: This call is not mutex protected. Handles should only be closed by
 * a single thread, and only if no other threads are going to reference
 * the associative array handle or one of its cursors any further. Do not close
 * a handle if an existing transaction has modified its associative array.
 * Doing so can cause misbehavior from associative array corruption to errors
 * like MDBX_BAD_VALSIZE (since the AA name is gone).
 *
 * Closing an associative array handle is not necessary, but lets mdbx_aa_open()
 * reuse the handle value.  Usually it's better to set a bigger
 * mdbx_set_max_handles(), unless that value would be large.
 *
 * [in] bk   An databook handle returned by mdbx_bk_init()
 * [in] aah  A associative array handle returned by mdbx_aa_open()
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_aa_close(MDBX_milieu *bk, MDBX_aah aah);

/* Flags for mdbx_aa_drop() */
/* FIXME: Describe */
enum mdbx_drop_flags_t
#if defined(__cplusplus) && __cplusplus >= 201103L
    : uint_least32_t
#endif /* C++11 */
{ MDBX_PURGE_AA = 0u,
  MDBX_DELETE_AA = 1u,
  MDBX_CLOSE_HANDLE = 2u };

/* Empty or delete an associative array.
 *
 * See mdbx_aa_close() for restrictions about closing the associative array
 * handle.
 *
 * [in] txn  A transaction handle returned by mdbx_tn_begin()
 * [in] aah  A associative array handle returned by mdbx_aa_open()
 * [in] del  0 to empty the associative array,
 *           1 to delete it from the databook and close the handle.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_aa_drop(MDBX_txn *txn, MDBX_aah aah,
                             enum mdbx_drop_flags_t flags);

/* Retrieve statistics for an associative array.
 *
 * [in]  txn        A transaction handle returned by mdbx_tn_begin()
 * [in]  aah        A associative arrays handle returned by mdbx_aa_open()
 * [out] stat       The address of an MDBX_aa_info structure where the
 *                  statistics and info will be copied.
 * [in]  info_size  Size in bytes of MDBX_aa_info which the caller expects.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified. */
LIBMDBX_API int mdbx_aa_info(MDBX_txn *txn, MDBX_aah aah, MDBX_aa_info *info,
                             size_t info_size);

/* Associative Array Handle flags returned by mdbx_aa_state(). */
enum MDBX_aah_flags_t {
  MDBX_AAH_STALE = 0 /* AA-record was not loaded */,
  MDBX_AAH_VALID = 1 << 0 /* AA-record is update */,
  MDBX_AAH_ABSENT = 1 << 1,
  MDBX_AAH_BAD = 1 << 2,
  MDBX_AAH_DIRTY = 1 << 3 /* AA was written in this txn */,
  MDBX_AAH_DROPPED = 1 << 4,
  MDBX_AAH_CREATED = 1 << 5,

  /* Internal transaction AA Flags */
  MDBX_AAH_INTERIM = 1 << 8,
  MDBX_AAH_GACO = 1 << 9,
  MDBX_AAH_MAIN = 1 << 10,
  MDBX_AAH_DUPS = 1 << 11
};

/* Retrieve the flags for an associative array handle.
 *
 * [in]  txn    A transaction handle returned by mdbx_tn_begin()
 * [in]  aah    A associative array handle returned by mdbx_aa_open()
 * [out] state  The optional address where the state will be returned.
 *
 * Returns A non-zero error value on failure and 0 on success. */
LIBMDBX_API int mdbx_aa_state(MDBX_txn *txn, MDBX_aah aah,
                              enum MDBX_aah_flags_t *state);

/* FIXME: Describe */
LIBMDBX_API int mdbx_aa_sequence(MDBX_txn *txn, MDBX_aah aah, uint64_t *result,
                                 uint64_t increment);

/*------------------------------------------------------------------------------
 * Direct key-value functions */

/* Insert/Update/Delete Flags */
enum mdbx_iud_flags_t
#if defined(__cplusplus) && __cplusplus >= 201103L
    : uint_least32_t
#endif /* C++11 */
{ MDBX_IUD_NODEFLAGS = 0xff /* lower 8 bits used for node flags */,
  /* For put: Don't write if the key already exists. */
  MDBX_IUD_NOOVERWRITE = 1u << 8,
  /* Only for associative arrays created winth MDBX_DUPSORT:
   * For put: don't write if the key and data pair already exist.
   * For delete: remove all duplicate data items. */
  MDBX_IUD_NODUP = 1u << 9,
  /* For mdbx_cursor_put: overwrite the current KV-pair.
   * For mdbx_put(): don't insert new KV-pair, but update present only. */
  MDBX_IUD_CURRENT = 1u << 10,
  /* For put: Just reserve space for data, don't copy it.
   * Return a pointer to the reserved space. */
  MDBX_IUD_RESERVE = 1u << 11,
  /* Data is being appended, don't split full pages. */
  MDBX_IUD_APPEND = 1u << 12,
  /* Duplicate data is being appended, don't split full pages. */
  MDBX_IUD_APPENDDUP = 1u << 13,
  /* Store multiple data items in one call. Only for MDBX_DUPFIXED. */
  MDBX_IUD_MULTIPLE = 1u << 14,
  /* Do not spill pages to disk if txn is getting full, may fail instead */
  MDBX_IUD_NOSPILL = 1u << 15,

  MDBX_IUD_FLAGS = MDBX_IUD_NOOVERWRITE | MDBX_IUD_NODUP | MDBX_IUD_CURRENT |
                   MDBX_IUD_RESERVE | MDBX_IUD_APPEND | MDBX_IUD_APPENDDUP |
                   MDBX_IUD_MULTIPLE | MDBX_IUD_NOSPILL,
};

/* Get items from an associative array.
 *
 * This function retrieves key/data pairs from the associative array. The
 * address and length of the data associated with the specified key are returned
 * in the structure to which data refers. If the associative array supports
 * duplicate keys (MDBX_DUPSORT) then the first data item for the key will be
 * returned. Retrieval of other items requires the use of mdbx_cursor_get().
 *
 * NOTE: The memory pointed to by the returned values is owned by the
 * associative array. The caller need not dispose of the memory, and may not
 * modify it in any way. For values returned in a read-only transaction
 * any modification attempts will cause a SIGSEGV.
 *
 * NOTE: Values returned from the associative array are valid only until a
 * subsequent update operation, or the end of the transaction.
 *
 * [in] txn       A transaction handle returned by mdbx_tn_begin()
 * [in] aah       A associative array handle returned by mdbx_aa_open()
 * [in] key       The key to search for in the associative array
 * [in,out] data  The data corresponding to the key
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_NOTFOUND  - the key was not in the associative array.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_get(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                         MDBX_iov *data);

/* Store items into an associative array.
 *
 * This function stores key/data pairs in the associative array. The default
 * behavior is to enter the new key/data pair, replacing any previously existing
 * key if duplicates are disallowed, or adding a duplicate data item if
 * duplicates are allowed (MDBX_DUPSORT).
 *
 * [in] txn    A transaction handle returned by mdbx_tn_begin()
 * [in] aah    A associative array handle returned by mdbx_aa_open()
 * [in] key    The key to store in the associative array
 * [in,out]    data The data to store
 * [in] flags  Special options for this operation. This parameter must be
 *             set to 0 or by bitwise OR'ing together one or more of the
 *             values described here.
 *
 *  - MDBX_IUD_NODUP
 *      Enter the new key/data pair only if it does not already appear
 *      in the associative array. This flag may only be specified if the
 *      associative array was opened with MDBX_DUPSORT. The function will return
 *      MDBX_KEYEXIST if the key/data pair already appears in the associative
 *      array.
 *
 *  - MDBX_IUD_NOOVERWRITE
 *      Enter the new key/data pair only if the key does not already appear
 *      in the associative array. The function will return MDBX_KEYEXIST if the
 *      key already appears in the associative array, even if the associative
 *      array supports duplicates (MDBX_DUPSORT). The data parameter will be set
 *      to point to the existing item.
 *
 *  - MDBX_IUD_CURRENT
 *      Update an single existing entry, but not add new ones. The function
 *      will return MDBX_NOTFOUND if the given key not exist in the associative
 *      array. Or the MDBX_EMULTIVAL in case duplicates for the given key.
 *
 *  - MDBX_IUD_RESERVE
 *      Reserve space for data of the given size, but don't copy the given
 *      data. Instead, return a pointer to the reserved space, which the
 *      caller can fill in later - before the next update operation or the
 *      transaction ends. This saves an extra memcpy if the data is being
 *      generated later. MDBX does nothing else with this memory, the caller
 *      is expected to modify all of the space requested. This flag must not
 *      be specified if the associative array was opened with MDBX_DUPSORT.
 *
 *  - MDBX_IUD_APPEND
 *      Append the given key/data pair to the end of the associative array. This
 *      option allows fast bulk loading when keys are already known to be in the
 *      correct order. Loading unsorted keys with this flag will cause
 *      a MDBX_EKEYMISMATCH error.
 *
 *  - MDBX_IUD_APPENDDUP
 *      As above, but for sorted dup data.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_KEYEXIST
 *  - MDBX_MAP_FULL  - the databook is full, see mdbx_set_mapsize().
 *  - MDBX_TXN_FULL  - the transaction has too many dirty pages.
 *  - MDBX_EACCES    - an attempt was made to write in a read-only transaction.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_put(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                         MDBX_iov *data, unsigned flags);

/* Delete items from an associative array.
 *
 * This function removes key/data pairs from the associative array.
 *
 * The data parameter is NOT ignored regardless the associative array does
 * support sorted duplicate data items or not. If the data parameter
 * is non-NULL only the matching data item will be deleted.
 *
 * This function will return MDBX_NOTFOUND if the specified key/data
 * pair is not in the associative array.
 *
 * [in] txn   A transaction handle returned by mdbx_tn_begin()
 * [in] aah   A associative array handle returned by mdbx_aa_open()
 * [in] key   The key to delete from the associative array
 * [in] data  The data to delete
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EACCES   - an attempt was made to write in a read-only transaction.
 *  - MDBX_EINVAL   - an invalid parameter was specified. */
LIBMDBX_API int mdbx_del(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                         MDBX_iov *data);

/* FIXME: Describe */
LIBMDBX_API int mdbx_replace(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                             MDBX_iov *new_data, MDBX_iov *old_data,
                             unsigned flags);
/* FIXME: Describe */
/* Same as mdbx_get(), but:
 * 1) if values_count is not NULL, then returns the count
 *    of multi-values/duplicates for a given key.
 * 2) updates the key for pointing to the actual key's data inside Databook. */
LIBMDBX_API int mdbx_get_ex(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                            MDBX_iov *data, size_t *values_count);

/*------------------------------------------------------------------------------
 * Cursor functions */

/* Create a cursor handle.
 *
 * A cursor is associated with a specific transaction and associative array.
 * A cursor cannot be used when its associative array handle is closed.  Nor
 * when its transaction has ended, except with mdbx_cr_renew().
 * It can be discarded with mdbx_cursor_close().
 *
 * A cursor must be closed explicitly always, before
 * or after its transaction ends. It can be reused with
 * mdbx_cr_renew() before finally closing it.
 *
 * [in] txn      A transaction handle returned by mdbx_tn_begin()
 * [in] aah      A associative array handle returned by mdbx_aa_open()
 * [out] cursor  Address where the new MDBX_cursor handle will be stored
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified. */
LIBMDBX_API MDBX_cursor_result_t mdbx_cursor_open(MDBX_txn *txn, MDBX_aah aah);

/* Close a cursor handle.
 *
 * The cursor handle will be freed and must not be used again after this call.
 * Its transaction must still be live if it is a write-transaction.
 *
 * [in] cursor  A cursor handle returned by mdbx_cursor_open() */
LIBMDBX_API int mdbx_cursor_close(MDBX_cursor *cursor);

/* Renew a cursor handle.
 *
 * A cursor is associated with a specific transaction and associative array.
 * Cursors that are only used in read-only transactions may be re-used,
 * to avoid unnecessary malloc/free overhead. The cursor may be associated
 * with a new read-only transaction, and referencing the same associative array
 * handle as it was created with.
 *
 * This may be done whether the previous transaction is live or dead.
 * [in] txn     A transaction handle returned by mdbx_tn_begin()
 * [in] cursor  A cursor handle returned by mdbx_cursor_open()
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - an invalid parameter was specified. */
LIBMDBX_API int mdbx_cursor_renew(MDBX_txn *txn, MDBX_cursor *cursor);

/* Return the cursor's transaction handle.
 *
 * [in] cursor A cursor handle returned by mdbx_cursor_open() */
LIBMDBX_API MDBX_txn_result_t mdbx_cursor_txn(const MDBX_cursor *cursor);

/* Return the cursor's associative array handle.
 *
 * [in] cursor  A cursor handle returned by mdbx_cursor_open() */
LIBMDBX_API MDBX_aah_result_t mdbx_cursor_aah(const MDBX_cursor *cursor);

/* Cursor Get operations.
 *
 * This is the set of all operations for retrieving data
 * using a cursor. */
enum MDBX_cursor_op {
  MDBX_FIRST /* Position at first key/data item */,
  MDBX_FIRST_DUP /* MDBX_DUPSORT-only: Position at first data item
                       * of current key. */,
  MDBX_GET_BOTH /* MDBX_DUPSORT-only: Position at key/data pair. */,
  MDBX_GET_BOTH_RANGE /* MDBX_DUPSORT-only: position at key, nearest data. */,
  MDBX_GET_CURRENT /* Return key/data at current cursor position */,
  MDBX_GET_MULTIPLE /* MDBX_DUPFIXED-only: Return key and up to a page of
                       * duplicate data items from current cursor position.
                       * Move cursor to prepare for MDBX_NEXT_MULTIPLE.*/,
  MDBX_LAST /* Position at last key/data item */,
  MDBX_LAST_DUP /* MDBX_DUPSORT-only: Position at last data item
                       * of current key. */,
  MDBX_NEXT /* Position at next data item */,
  MDBX_NEXT_DUP /* MDBX_DUPSORT-only: Position at next data item
                       * of current key. */,
  MDBX_NEXT_MULTIPLE /* MDBX_DUPFIXED-only: Return key and up to a page of
                       * duplicate data items from next cursor position.
                       * Move cursor to prepare for MDBX_NEXT_MULTIPLE. */,
  MDBX_NEXT_NODUP /* Position at first data item of next key */,
  MDBX_PREV /* Position at previous data item */,
  MDBX_PREV_DUP /* MDBX_DUPSORT-only: Position at previous data item
                       * of current key. */,
  MDBX_PREV_NODUP /* Position at last data item of previous key */,
  MDBX_SET /* Position at specified key */,
  MDBX_SET_KEY /* Position at specified key, return both key and data */,
  MDBX_SET_RANGE /* Position at first key greater than or equal to
                       * specified key. */,
  MDBX_PREV_MULTIPLE /* MDBX_DUPFIXED-only: Position at previous page and
                     * return key and up to a page of duplicate data items. */
};

/* Retrieve by cursor.
 *
 * This function retrieves key/data pairs from the associative array. The
 * address and length of the key are returned in the object to which key refers
 * (except for the case of the MDBX_SET option, in which the key object is
 * unchanged), and the address and length of the data are returned in the object
 * to which data refers. See mdbx_get() for restrictions on using the output
 * values.
 *
 * [in] cursor    A cursor handle returned by mdbx_cursor_open()
 * [in,out] key   The key for a retrieved item
 * [in,out] data  The data of a retrieved item
 * [in] op        A cursor operation MDBX_cursor_op
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_NOTFOUND  - no matching key found.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_cursor_get(MDBX_cursor *cursor, MDBX_iov *key,
                                MDBX_iov *data, enum MDBX_cursor_op op);

/* Store by cursor.
 *
 * This function stores key/data pairs into the associative array. The cursor is
 * positioned at the new item, or on failure usually near it.
 *
 * [in] cursor  A cursor handle returned by mdbx_cursor_open()
 * [in] key     The key operated on.
 * [in] data    The data operated on.
 * [in] flags   Options for this operation. This parameter
 *              must be set to 0 or one of the values described here:
 *
 *  - MDBX_IUD_CURRENT
 *      Replace the item at the current cursor position. The key parameter
 *      must still be provided, and must match it, otherwise the function
 *      return MDBX_EKEYMISMATCH.
 *
 *      NOTE: MDBX unlike LMDB allows you to change the size of the data and
 *      automatically handles reordering for sorted duplicates (MDBX_DUPSORT).
 *
 *  - MDBX_IUD_NODUP
 *      Enter the new key/data pair only if it does not already appear in the
 *      associative array. This flag may only be specified if the associative
 *      array was opened with MDBX_DUPSORT. The function will return
 *      MDBX_KEYEXIST if the key/data pair already appears in the associative
 *      array.
 *
 *  - MDBX_IUD_NOOVERWRITE
 *      Enter the new key/data pair only if the key does not already appear
 *      in the associative array. The function will return MDBX_KEYEXIST if the
 *      key already appears in the associative array, even if the associative
 *      array supports duplicates (MDBX_DUPSORT).
 *
 *  - MDBX_IUD_RESERVE
 *      Reserve space for data of the given size, but don't copy the given
 *      data. Instead, return a pointer to the reserved space, which the
 *      caller can fill in later - before the next update operation or the
 *      transaction ends. This saves an extra memcpy if the data is being
 *      generated later. This flag must not be specified if the associative
 *      array was opened with MDBX_DUPSORT.
 *
 *  - MDBX_IUD_APPEND
 *      Append the given key/data pair to the end of the associative array.
 *      No key comparisons are performed. This option allows fast bulk loading
 *      when keys are already known to be in the correct order. Loading unsorted
 *      keys with this flag will cause a MDBX_KEYEXIST error.
 *
 *  - MDBX_IUD_APPENDDUP
 *      As above, but for sorted dup data.
 *
 *  - MDBX_IUD_MULTIPLE
 *      Store multiple contiguous data elements in a single request. This flag
 *      may only be specified if the associative array was opened with
 *      MDBX_DUPFIXED. The data argument must be an array of two MDBX_vals. The
 *      iov_len of the first MDBX_iov must be the size of a single data element.
 *      The iov_base of the first MDBX_iov must point to the beginning of the
 *      array of contiguous data elements. The iov_len of the second MDBX_iov
 *      must be the count of the number of data elements to store. On return
 *      this field will be set to the count of the number of elements actually
 *      written. The iov_base of the second MDBX_iov is unused.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EKEYMISMATCH
 *  - MDBX_MAP_FULL  - the databook is full, see mdbx_set_mapsize().
 *  - MDBX_TXN_FULL  - the transaction has too many dirty pages.
 *  - MDBX_EACCES    - an attempt was made to write in a read-only transaction.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_cursor_put(MDBX_cursor *cursor, MDBX_iov *key,
                                MDBX_iov *data, unsigned flags);

/* Delete current key/data pair
 *
 * This function deletes the key/data pair to which the cursor refers.
 *
 * [in] cursor  A cursor handle returned by mdbx_cursor_open()
 * [in] flags   Options for this operation. This parameter must be set to 0
 *              or one of the values described here.
 *
 *  - MDBX_IUD_NODUP
 *      Delete all of the data items for the current key. This flag may only
 *      be specified if the associative array was opened with MDBX_DUPSORT.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EACCES  - an attempt was made to write in a read-only transaction.
 *  - MDBX_EINVAL  - an invalid parameter was specified. */
LIBMDBX_API int mdbx_cursor_delete(MDBX_cursor *cursor, unsigned flags);

/* Return count of duplicates for current key.
 *
 * This call is only valid on associative arrays that support sorted duplicate
 * data items MDBX_DUPSORT.
 *
 * [in] cursor    A cursor handle returned by mdbx_cursor_open()
 * [out] countp   Address where the count will be stored
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EINVAL   - cursor is not initialized, or an invalid parameter
 *                    was specified. */
LIBMDBX_API int mdbx_cursor_count(MDBX_cursor *cursor, size_t *count_ptr);

/* FIXME: Describe */
/* Returns:
 *  - MDBX_RESULT_TRUE
 *      when no more data available or cursor not positioned;
 *  - MDBX_RESULT_FALSE
 *      when data available;
 *  - Otherwise the error code. */
LIBMDBX_API int mdbx_cursor_eof(MDBX_cursor *cursor);

/* FIXME: Describe */
/* Returns: MDBX_RESULT_TRUE, MDBX_RESULT_FALSE or Error code. */
LIBMDBX_API int mdbx_cursor_at_first(MDBX_cursor *cursor);

/* FIXME: Describe */
/* Returns: MDBX_RESULT_TRUE, MDBX_RESULT_FALSE or Error code. */
LIBMDBX_API int mdbx_cursor_at_last(MDBX_cursor *cursor);

/*----------------------------------------------------------------------------*/
/* Attribute support functions for Nexenta */
typedef uint_fast64_t MDBX_attr_t;

/* Store by cursor with attribute.
 *
 * This function stores key/data pairs into the associative array. The cursor is
 * positioned at the new item, or on failure usually near it.
 *
 * NOTE: Internally based on MDBX_IUD_RESERVE feature,
 *       therefore doesn't support MDBX_DUPSORT.
 *
 * [in] cursor  A cursor handle returned by mdbx_cursor_open()
 * [in] key     The key operated on.
 * [in] data    The data operated on.
 * [in] attr    The attribute.
 * [in] flags   Options for this operation. This parameter must be set to 0
 *              or one of the values described here:
 *
 *  - MDBX_IUD_CURRENT
 *      Replace the item at the current cursor position. The key parameter
 *      must still be provided, and must match it, otherwise the function
 *      return MDBX_EKEYMISMATCH.
 *
 *  - MDBX_IUD_APPEND
 *      Append the given key/data pair to the end of the associative array.
 *      No key comparisons are performed. This option allows fast bulk loading
 *      when keys are already known to be in the correct order. Loading unsorted
 *      keys with this flag will cause a MDBX_KEYEXIST error.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_EKEYMISMATCH
 *  - MDBX_MAP_FULL  - the database is full, see mdbx_set_mapsize().
 *  - MDBX_TXN_FULL  - the transaction has too many dirty pages.
 *  - MDBX_EACCES    - an attempt was made to write in a read-only transaction.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_cr_put_attr(MDBX_cursor *cursor, MDBX_iov *key,
                                 MDBX_iov *data, MDBX_attr_t attr,
                                 unsigned flags);

/* Store items and attributes into an associative array.
 *
 * This function stores key/data pairs in the associative array. The default
 * behavior is to enter the new key/data pair, replacing any previously existing
 * key if duplicates are disallowed.
 *
 * NOTE: Internally based on MDBX_IUD_RESERVE feature,
 *       therefore doesn't support MDBX_DUPSORT.
 *
 * [in] txn       A transaction handle returned by mdbx_tn_begin().
 * [in] aah       A associative array handle returned by mdbx_aa_open().
 * [in] key       The key to store in the associative array.
 * [in] attr      The attribute to store in the associative array.
 * [in,out] data  The data to store.
 * [in] flags     Special options for this operation. This parameter must be
 *                set to 0 or by bitwise OR'ing together one or more of the
 *                values described here:
 *
 *  - MDBX_IUD_NOOVERWRITE
 *      Enter the new key/data pair only if the key does not already appear
 *      in the associative array. The function will return MDBX_KEYEXIST
 *      if the key already appears in the associative array. The data parameter
 *      will be set to point to the existing item.
 *
 *  - MDBX_IUD_CURRENT
 *      Update an single existing entry, but not add new ones. The function
 *      will return MDBX_NOTFOUND if the given key not exist in the associative
 *      array. Or the MDBX_EMULTIVAL in case duplicates for the given key.
 *
 *  - MDBX_IUD_APPEND
 *      Append the given key/data pair to the end of the associative array.
 *      This option allows fast bulk loading when keys are already known to be
 *      in the correct order. Loading unsorted keys with this flag will cause
 *      a MDBX_EKEYMISMATCH error.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_KEYEXIST
 *  - MDBX_MAP_FULL  - the database is full, see mdbx_set_mapsize().
 *  - MDBX_TXN_FULL  - the transaction has too many dirty pages.
 *  - MDBX_EACCES    - an attempt was made to write in a read-only transaction.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_put_attr(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                              MDBX_iov *data, MDBX_attr_t attr, unsigned flags);

/* Set items attribute inside an associative array.
 *
 * This function stores key/data pairs attribute to the associative array.
 *
 * NOTE: Internally based on MDBX_IUD_RESERVE feature,
 *       therefore doesn't support MDBX_DUPSORT.
 *
 * [in] txn   A transaction handle returned by mdbx_tn_begin().
 * [in] aah   A associative array handle returned by mdbx_aa_open().
 * [in] key   The key to search for in the associative array.
 * [in] data  The data to be stored or NULL to save previous value.
 * [in] attr  The attribute to be stored.
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *	 - MDBX_NOTFOUND   - the key-value pair was not in the associative
 *array.
 *	 - MDBX_EINVAL     - an invalid parameter was specified. */
LIBMDBX_API int mdbx_set_attr(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                              MDBX_iov *data, MDBX_attr_t attr);

/* Get items attribute from an associative array cursor.
 *
 * This function retrieves key/data pairs from the associative array. The
 * address and  length of the key are returned in the object to which key
 * refers (except for the case of the MDBX_SET option, in which the key object
 * is unchanged), and the address and length of the data are returned in the
 * object to which data refers. See mdbx_get() for restrictions on using the
 * output values.
 *
 * [in] cursor    A cursor handle returned by mdbx_cursor_open()
 * [in,out] key   The key for a retrieved item
 * [in,out] data  The data of a retrieved item
 * [in] op        A cursor operation MDBX_cursor_op
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_NOTFOUND  - no matching key found.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_cr_get_attr(MDBX_cursor *mc, MDBX_iov *key, MDBX_iov *data,
                                 MDBX_attr_t *attrptr, enum MDBX_cursor_op op);

/* Get items attribute from an associative array.
 *
 * This function retrieves key/data pairs from the associative array. The
 * address and length of the data associated with the specified key are returned
 * in the structure to which data refers.
 * If the associative array supports duplicate keys (MDBX_DUPSORT) then the
 * first data item for the key will be returned. Retrieval of other
 * items requires the use of mdbx_cursor_get().
 *
 * NOTE: The memory pointed to by the returned values is owned by the
 * engine. The caller need not dispose of the memory, and may not
 * modify it in any way. For values returned in a read-only transaction
 * any modification attempts will cause a SIGSEGV.
 *
 * NOTE: Values returned from the engine are valid only until a
 * subsequent update operation, or the end of the transaction.
 *
 * [in] txn       A transaction handle returned by mdbx_tn_begin()
 * [in] aah       A associative array handle returned by mdbx_aa_open()
 * [in] key       The key to search for in the associative array
 * [in,out] data  The data corresponding to the key
 *
 * Returns A non-zero error value on failure and 0 on success, some
 * possible errors are:
 *  - MDBX_NOTFOUND  - the key was not in the associative array.
 *  - MDBX_EINVAL    - an invalid parameter was specified. */
LIBMDBX_API int mdbx_get_attr(MDBX_txn *txn, MDBX_aah aah, MDBX_iov *key,
                              MDBX_iov *data, MDBX_attr_t *attrptr);

/*------------------------------------------------------------------------------
 * Misc functions */

/* FIXME: Describe */
enum MDBX_debug_flags_t
#if defined(__cplusplus) && __cplusplus >= 201103L
    : uint_least32_t
#endif /* C++11 */
{ MDBX_DBG_ASSERT = 1,
  MDBX_DBG_PRINT = 2,
  MDBX_DBG_TRACE = 4,
  MDBX_DBG_EXTRA = 8,
  MDBX_DBG_AUDIT = 16,
  MDBX_DBG_JITTER = 32,
  MDBX_DBG_DUMP = 64,
};

/* FIXME: Describe */
LIBMDBX_API int mdbx_set_debug(int flags, MDBX_debug_func *logger);

/* Return a string describing a given error code.
 *
 * This function is a superset of the ANSI C X3.159-1989 (ANSI C) strerror(3)
 * function. If the error code is greater than or equal to 0, then the string
 * returned by the system function strerror(3) is returned. If the error code
 * is less than 0, an error string corresponding to the MDBX library error is
 * returned. See errors for a list of MDBX-specific error codes.
 *
 * [in] err The error code
 *
 * Returns "error message" The description of the error */
LIBMDBX_API const char *mdbx_strerror(int errnum);
LIBMDBX_API const char *mdbx_strerror_r(int errnum, char *buf, size_t buflen);

/* FIXME: Describe */
LIBMDBX_API char *mdbx_dump_iov(const MDBX_iov *iov, char *const buf,
                                const size_t bufsize);

#ifdef __cplusplus
}
#endif

#endif /* LIBMDBX_H */
