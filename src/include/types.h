/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file */

#ifndef PRTE_TYPES_H
#define PRTE_TYPES_H

#include "prte_config.h"

#include <stdint.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#    include <sys/select.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif

/** rank on node, used for both local and node rank. We
 * don't send these around on their own, so don't create
 * dedicated type support for them - we are defining them
 * here solely for readability in the code and so we have
 * one place where any future changes can be made
 */
typedef uint16_t prte_local_rank_t;
typedef uint16_t prte_node_rank_t;
#define PRTE_LOCAL_RANK_MAX     (UINT16_MAX - 1)
#define PRTE_NODE_RANK_MAX      (UINT16_MAX - 1)
#define PRTE_LOCAL_RANK_INVALID UINT16_MAX
#define PRTE_NODE_RANK_INVALID  UINT16_MAX

/* index for app_contexts */
typedef uint32_t prte_app_idx_t;

/*
 * handle differences in socklen_t
 */

#if defined(HAVE_SOCKLEN_T)
typedef socklen_t prte_socklen_t;
#else
typedef int prte_socklen_t;
#endif

/*
 * Convert a 64 bit value to network byte order.
 *
 * These two are the OOB's 64-bit wire conversion - oob_tcp_hdr.h runs the
 * message header's origin epoch through them - so they are the one thing in
 * this header a heterogeneous DVM depends on being right.  Note the
 * #ifdef: HAVE_UNIX_BYTESWAP is AC_DEFINE'd only when the check succeeds
 * (it is absent, not 0, otherwise), so #ifdef is correct here and #if would
 * be wrong.  Where it is absent, prte_config_bottom.h supplies identity
 * htonl/htons stubs and these follow suit.
 */
static inline uint64_t prte_hton64(uint64_t val) __prte_attribute_const__;
static inline uint64_t prte_hton64(uint64_t val)
{
#ifdef HAVE_UNIX_BYTESWAP
    union {
        uint64_t ll;
        uint32_t l[2];
    } w, r;

    /* platform already in network byte order? */
    if (htonl(1) == 1L)
        return val;
    w.ll = val;
    r.l[0] = htonl(w.l[1]);
    r.l[1] = htonl(w.l[0]);
    return r.ll;
#else
    return val;
#endif
}

/*
 * Convert a 64 bit value from network to host byte order.
 */

static inline uint64_t prte_ntoh64(uint64_t val) __prte_attribute_const__;
static inline uint64_t prte_ntoh64(uint64_t val)
{
#ifdef HAVE_UNIX_BYTESWAP
    union {
        uint64_t ll;
        uint32_t l[2];
    } w, r;

    /* platform already in network byte order? */
    if (htonl(1) == 1L)
        return val;
    w.ll = val;
    r.l[0] = ntohl(w.l[1]);
    r.l[1] = ntohl(w.l[0]);
    return r.ll;
#else
    return val;
#endif
}

#endif
