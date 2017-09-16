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

static const char *__mdbx_strerr(int errnum) {
  /* Table of descriptions for MDBX errors */
  static const char *const tbl[] = {
      "MDBX_KEYEXIST: Key/data pair already exists",
      "MDBX_NOTFOUND: No matching key/data pair found",
      "MDBX_PAGE_NOTFOUND: Requested page not found",
      "MDBX_CORRUPTED: Databook is corrupted",
      "MDBX_PANIC: Update of meta page failed or databook had fatal error",
      "MDBX_VERSION_MISMATCH: Databook version mismatch libmdbx",
      "MDBX_INVALID: File is not an MDBX file",
      "MDBX_MAP_FULL: Databook mapsize limit reached",
      "MDBX_DBS_FULL: Too may AAH (maxdbs reached)",
      "MDBX_READERS_FULL: Too many readers (maxreaders reached)",
      nullptr /* MDBX_TLS_FULL (-30789): unused in MDBX */,
      "MDBX_TXN_FULL: Transaction has too many dirty pages - transaction too "
      "big",
      "MDBX_CURSOR_FULL: Internal error - cursor stack limit reached",
      "MDBX_PAGE_FULL: Internal error - page has no more space",
      "MDBX_MAP_RESIZED: Databook contents grew beyond mapsize limit",
      "MDBX_INCOMPATIBLE: Operation and AA incompatible, or AA flags changed",
      "MDBX_BAD_RSLOT: Invalid reuse of reader locktable slot",
      "MDBX_BAD_TXN: Transaction must abort, has a child, or is invalid",
      "MDBX_BAD_VALSIZE: Unsupported size of key/AA name/data, or wrong "
      "DUPFIXED size",
      "MDBX_BAD_AAH: The specified AAH handle was closed/changed unexpectedly",
      "MDBX_PROBLEM: Unexpected problem - txn should abort",
  };

  if (errnum >= MDBX_KEYEXIST && errnum <= MDBX_LAST_ERRCODE) {
    int i = errnum - MDBX_KEYEXIST;
    return tbl[i];
  }

  switch (errnum) {
  case MDBX_SUCCESS:
    return "MDBX_SUCCESS: Successful";
  case MDBX_EMULTIVAL:
    return "MDBX_EMULTIVAL: Unable to update multi-value for the given key";
  case MDBX_EBADSIGN:
    return "MDBX_EBADSIGN: Wrong signature of a runtime object(s)";
  case MDBX_WANNA_RECOVERY:
    return "MDBX_WANNA_RECOVERY: Databook should be recovered, but this could "
           "NOT be done in a read-only mode";
  case MDBX_EKEYMISMATCH:
    return "MDBX_EKEYMISMATCH: The given key value is mismatched to the "
           "current cursor position";
  case MDBX_TOO_LARGE:
    return "MDBX_TOO_LARGE: Databook is too large for current system, "
           "e.g. could NOT be mapped into RAM";
  case MDBX_THREAD_MISMATCH:
    return "MDBX_THREAD_MISMATCH: A thread has attempted to use a not "
           "owned object, e.g. a transaction that started by another thread";
  default:
    return nullptr;
  }
}

const char *__cold mdbx_strerror_r(int errnum, char *buf, size_t buflen) {
  const char *msg = __mdbx_strerr(errnum);
  if (!msg) {
    if (!buflen || buflen > INT_MAX)
      return nullptr;
#ifdef _MSC_VER
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        errnum, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, (DWORD)buflen,
        nullptr);
    return size ? buf : nullptr;
#elif defined(_GNU_SOURCE)
    /* GNU-specific */
    msg = strerror_r(errnum, buf, buflen);
#elif (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600)
    /* XSI-compliant */
    int rc = strerror_r(errnum, buf, buflen);
    if (rc) {
      rc = snprintf(buf, buflen, "error %d", errnum);
      assert(rc > 0);
    }
    return buf;
#else
    strncpy(buf, strerror(errnum), buflen);
    buf[buflen - 1] = '\0';
    return buf;
#endif
  }
  return msg;
}

const char *__cold mdbx_strerror(int errnum) {
  const char *msg = __mdbx_strerr(errnum);
  if (!msg) {
#ifdef _MSC_VER
    static __thread char buffer[1024];
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        errnum, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer,
        sizeof(buffer), nullptr);
    if (size)
      msg = buffer;
#else
    msg = strerror(errnum);
#endif
  }
  return msg;
}

/*----------------------------------------------------------------------------*/

/* Count all the pages in each AA and in the GACO and make sure
 * it matches the actual number of pages being used.
 * All named AAs must be open for a correct count. */
static int audit(MDBX_txn *txn) {
  MDBX_cursor mc;
  MDBX_iov key, data;

  pgno_t freecount = 0;
  int rc = cursor_init(&mc, txn, aht_gaco(txn));
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  while ((rc = mdbx_cursor_get(&mc, &key, &data, MDBX_NEXT)) == 0)
    freecount += *(pgno_t *)data.iov_base;
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  pgno_t count = 0;
  aht_t *aht = txn->txn_aht_array;
  for (MDBX_aah aah = 0; aah < txn->txn_ah_num; ++aah, ++aht) {
    if (!(aht->ah.state8 & MDBX_AAH_VALID))
      continue;
    rc = cursor_init(&mc, txn, aht);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    if (aht->aa.root == P_INVALID)
      continue;
    count += aht->aa.branch_pages + aht->aa.leaf_pages + aht->aa.overflow_pages;
    if (aht->aa.flags16 & MDBX_DUPSORT) {
      rc = page_search(&mc.primal, nullptr, MDBX_PS_FIRST);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      while (true) {
        page_t *mp = mc.primal.mc_pg[mc.primal.mc_top];
        for (unsigned n = 0; n < page_numkeys(mp); n++) {
          node_t *leaf = node_ptr(mp, n);
          if (leaf->node_flags8 & NODE_SUBTREE) {
            aatree_t *entry = NODEDATA(leaf);
            count += get_le64_aligned16(&entry->aa_branch_pages) +
                     get_le64_aligned16(&entry->aa_leaf_pages) +
                     get_le64_aligned16(&entry->aa_overflow_pages);
          }
        }
        rc = cursor_sibling(&mc.primal, true);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
      }
    }
  }
  if (freecount + count + NUM_METAS != txn->mt_next_pgno) {
    mdbx_print("audit: %" PRIaTXN " gaco: %" PRIaPGNO " count: %" PRIaPGNO
               " total: %" PRIaPGNO " next_pgno: %" PRIaPGNO "\n",
               txn->mt_txnid, freecount, count + NUM_METAS,
               freecount + count + NUM_METAS, txn->mt_next_pgno);
    return MDBX_RESULT_TRUE;
  }
  return MDBX_RESULT_FALSE;
}
