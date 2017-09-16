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
#include "./ualb.h"

#if defined(__GNUC__) && !__GNUC_PREREQ(4, 2)
/* Actualy libmdbx was not tested with compilers older than GCC from RHEL6.
 * But you could remove this #error and try to continue at your own risk.
 * In such case please don't rise up an issues related ONLY to old compilers.
 */
#warning "libmdbx required at least GCC 4.2 compatible C/C++ compiler."
#endif

#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 12)
/* Actualy libmdbx was not tested with something older than glibc 2.12 (from
 * RHEL6).
 * But you could remove this #error and try to continue at your own risk.
 * In such case please don't rise up an issues related ONLY to old systems.
 */
#warning "libmdbx required at least GLIBC 2.12."
#endif

#ifdef __SANITIZE_THREAD__
#warning                                                                       \
    "libmdbx don't compatible with ThreadSanitizer, you will get a LOT of false-positive issues."
#endif /* __SANITIZE_THREAD__ */

#include "aah.c"
#include "api.c"
#include "cmp.c"
#include "cursor.c"
#include "debug.c"
#include "env.c"
#include "list.c"
#include "meta.c"
#include "misc.c"
#include "osal.c"
#include "page.c"
#include "rthc.c"
#include "txn.c"
#include "util.c"

#include "mess.c"

#if defined(_WIN32) || defined(_WIN64)
#include "lck-windows.c"
#else
#include "lck-posix.c"
#endif
