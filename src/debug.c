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

int mdbx_runtime_flags = MDBX_DBG_PRINT
#if MDBX_DEBUG
                         | MDBX_DBG_ASSERT
#endif
#if MDBX_DEBUG > 1
                         | MDBX_DBG_TRACE
#endif
#if MDBX_DEBUG > 2
                         | MDBX_DBG_AUDIT
#endif
#if MDBX_DEBUG > 3
                         | MDBX_DBG_EXTRA
#endif
    ;

MDBX_debug_func *mdbx_debug_logger;

void __cold mdbx_debug_log(int type, const char *function, int line,
                           const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  if (mdbx_debug_logger)
    mdbx_debug_logger(type, function, line, fmt, args);
  else {
    if (function && line > 0)
      fprintf(stderr, "%s:%d ", function, line);
    else if (function)
      fprintf(stderr, "%s: ", function);
    else if (line > 0)
      fprintf(stderr, "%d: ", line);
    vfprintf(stderr, fmt, args);
  }
  va_end(args);
}

#if 0  /* LY: debug stuff */
static const char *leafnode_type(node_t *n) {
  static char *const tp[2][2] = {{"", ": AA"}, {": sub-page", ": sub-AA"}};
  return unlikely(n->node_flags8 & NODE_BIG) ? ": overflow page"
                                         : tp[F_ISSET(n->node_flags8, NODE_DUP)]
                                             [F_ISSET(n->node_flags8, NODE_SUBTREE)];
}

/* Display all the keys in the page. */
static void mdbx_page_list(page_t *mp) {
  pgno_t pgno = mp->mp_pgno;
  const char *type, *state = (mp->mp_flags & P_DIRTY) ? ", dirty" : "";
  node_t *node;
  unsigned i, nkeys, nsize, total = 0;
  MDBX_iov key;
  DKBUF;

  switch (mp->mp_flags &
          (P_BRANCH | P_LEAF | P_DFL | P_META | P_OVERFLOW | P_SUBP)) {
  case P_BRANCH:
    type = "Branch page";
    break;
  case P_LEAF:
    type = "Leaf page";
    break;
  case P_LEAF | P_SUBP:
    type = "Sub-page";
    break;
  case P_LEAF | P_DFL:
    type = "DFL page";
    break;
  case P_LEAF | P_DFL | P_SUBP:
    type = "DFL sub-page";
    break;
  case P_OVERFLOW:
    mdbx_print("Overflow page %" PRIu64 " pages %u%s\n", pgno, mp->mp_pages,
               state);
    return;
  case P_META:
    mdbx_print("Meta-page %" PRIu64 " txnid %" PRIu64 "\n", pgno,
               ((meta_t *)page_data(mp))->mm_txnid);
    return;
  default:
    mdbx_print("Bad page %" PRIu64 " flags 0x%X\n", pgno, mp->mp_flags);
    return;
  }

  nkeys = page_numkeys(mp);
  mdbx_print("%s %" PRIu64 " page_numkeys %u%s\n", type, pgno, nkeys, state);

  for (i = 0; i < nkeys; i++) {
    if (IS_DFL(mp)) { /* DFL pages have no mp_ptrs[] or node headers */
      key.iov_len = nsize = mp->mp_leaf2_ksize;
      key.iov_base = DFLKEY(mp, i, nsize);
      total += nsize;
      mdbx_print("key %u: nsize %u, %s\n", i, nsize, DKEY(&key));
      continue;
    }
    node = node_ptr(mp, i);
    key.iov_len = node->mn_ksize16;
    key.iov_base = node->mn_data;
    nsize = NODESIZE + key.iov_len;
    if (IS_BRANCH(mp)) {
      mdbx_print("key %u: page %" PRIu64 ", %s\n", i, node_get_pgno(node),
                 DKEY(&key));
      total += nsize;
    } else {
      if (unlikely(node->node_flags8 & NODE_BIG))
        nsize += sizeof(pgno_t);
      else
        nsize += node_get_datasize(node);
      total += nsize;
      nsize += sizeof(indx_t);
      mdbx_print("key %u: nsize %u, %s%s\n", i, nsize, DKEY(&key),
                 leafnode_type(node));
    }
    total = EVEN(total);
  }
  mdbx_print("Total: header %u + contents %u + unused %u\n",
             IS_DFL(mp) ? PAGEHDRSZ : PAGEHDRSZ + mp->mp_lower, total,
             page_spaceleft(mp));
}

static void cursor_check(MDBX_cursor *mc) {
  unsigned i;
  node_t *node;
  page_t *mp;

  if (!mc->mc_snum || !(mc->mc_state8 & C_INITIALIZED))
    return;
  for (i = 0; i < mc->mc_top; i++) {
    mp = mc->mc_pg[i];
    node = node_ptr(mp, mc->mc_ki[i]);
    if (unlikely(node_get_pgno(node) != mc->mc_pg[i + 1]->mp_pgno))
      mdbx_print("oops!\n");
  }
  if (unlikely(mc->mc_ki[i] >= page_numkeys(mc->mc_pg[i])))
    mdbx_print("ack!\n");
  if (XCURSOR_INITED(mc)) {
    node = node_ptr(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
    if (((node->node_flags8 & (NODE_DUP | NODE_SUBTREE)) == NODE_DUP) &&
        mc->subordinate.mx_cursor.mc_pg[0] != NODEDATA(node)) {
      mdbx_print("blah!\n");
    }
  }
}
#endif /* 0 */
