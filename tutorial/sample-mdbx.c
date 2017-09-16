/* sample-mdb.txt - MDB toy/sample
 *
 * Do a line-by-line comparison of this and sample-bdb.txt
 */

/*
 * Copyright 2017 Ilya Shipitsin <chipitsine@gmail.com>.
 * Copyright 2015-2017 Leonid Yuriev <leo@yuriev.ru>.
 * Copyright 2012-2015 Howard Chu, Symas Corp.
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

#include "mdbx.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  int rc;
  MDBX_milieu *bk = NULL;
  MDBX_aah aah = 0;
  MDBX_iov key, data;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  char sval[32];

  rc = mdbx_bk_init(&bk);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_bk_init: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }
  rc = mdbx_bk_open(bk, "./example-db", MDBX_COALESCE | MDBX_LIFORECLAIM, 0664);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_bk_open: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }

  rc = mdbx_tn_begin(bk, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_tn_begin: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }
  rc = mdbx_aa_open(txn, NULL, 0, &aah, NULL, NULL);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_aa_open: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }

  key.iov_len = sizeof(int);
  key.iov_base = sval;
  data.iov_len = sizeof(sval);
  data.iov_base = sval;

  sprintf(sval, "%03x %d foo bar", 32, 3141592);
  rc = mdbx_put(txn, aah, &key, &data, 0);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_put: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }
  rc = mdbx_tn_commit(txn);
  if (rc) {
    fprintf(stderr, "mdbx_tn_commit: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }
  txn = NULL;

  rc = mdbx_tn_begin(bk, NULL, MDBX_RDONLY, &txn);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_tn_begin: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }
  rc = mdbx_cursor_open(txn, aah, &cursor);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_cursor_open: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  }

  int found = 0;
  while ((rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT)) == 0) {
    printf("key: %p %.*s, data: %p %.*s\n", key.iov_base, (int)key.iov_len,
           (char *)key.iov_base, data.iov_base, (int)data.iov_len,
           (char *)data.iov_base);
    found += 1;
  }
  if (rc != MDBX_NOTFOUND || found == 0) {
    fprintf(stderr, "mdbx_cursor_get: (%d) %s\n", rc, mdbx_strerror(rc));
    goto bailout;
  } else {
    rc = MDBX_SUCCESS;
  }
bailout:
  if (cursor)
    mdbx_cursor_close(cursor);
  if (txn)
    mdbx_tn_abort(txn);
  if (aah)
    mdbx_aa_close(bk, aah);
  if (bk)
    mdbx_bk_shutdown(bk);
  return (rc != MDBX_SUCCESS) ? EXIT_FAILURE : EXIT_SUCCESS;
}
