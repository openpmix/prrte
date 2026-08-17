/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_OOB_TCP_HDR_H_
#define _MCA_OOB_TCP_HDR_H_

#include "prte_config.h"

#include <stddef.h>
#include <string.h>

#include "types.h"

/* Message types carried in the TCP header. IDENT and PROBE are used
 * during the connection handshake; USER marks a normal RML message,
 * whether it is destined for us or is being relayed on to the next hop.
 */
typedef uint8_t prte_oob_tcp_msg_type_t;

#define MCA_OOB_TCP_IDENT 1
#define MCA_OOB_TCP_PROBE 2
#define MCA_OOB_TCP_USER  4

/* header for tcp msgs
 *
 * Only the first PRTE_OOB_TCP_HDR_LEN() bytes of this struct go on the wire.
 * The nspace is last, and only the characters it actually uses are sent - the
 * rest of the array is scratch space for the receiver to land them in.  That
 * is the whole reason for the layout: this header rides *every* RML message,
 * and holding the names as two `pmix_proc_t` made it 552 bytes, of which 512
 * were two fixed 256-byte nspace arrays holding the same short string.  On a
 * launch-message cpuset slice for an eight-process node - 101 bytes of
 * payload - that header was 85% of the message.
 *
 * One nspace covers both ends, because they have never differed on a message
 * that carries data: every send entry point takes a *rank*
 * (prte_rml_send_buffer_nb and friends), so the destination is always a peer
 * of the sender's own job, the origin is the sender itself, and a relay
 * copies both through unchanged.  The connect handshake is the one place the
 * two names can differ - there `dst` is the peer being introduced to - and
 * the receiver there reads only the origin, so the field carries the origin's
 * nspace in that case too.
 */
typedef struct {
    /* boot epoch (incarnation) of the origin. A daemon that departs and reboots
     * into the same rank comes back with a strictly-greater epoch, so a hop can
     * drop late traffic stamped with the stale incarnation's epoch. Leading,
     * so that the 8-byte field needs no padding ahead of it. */
    uint64_t epoch;
    /* the rank of the originator of the message - when relaying, this is the
     * process that first sent the message, not necessarily our peer
     */
    pmix_rank_t origin;
    /* the rank of the intended final recipient. If it is not us, we relay the
     * message onward toward that process using the routing tree
     */
    pmix_rank_t dst;
    /* the rml tag where this message is headed */
    prte_rml_tag_t tag;
    /* the seq number of this message */
    uint32_t seq_num;
    /* number of bytes in message */
    uint32_t nbytes;
    /* type of message */
    prte_oob_tcp_msg_type_t type;
    /* characters of nspace that follow, NOT counting a terminator - none is
     * sent.  A uint8_t holds every legal length because PMIX_MAX_NSLEN is
     * 255, which is also why no bound check is needed on the receiving side. */
    uint8_t nslen;
    /* the nspace both ranks above belong to.  Sent as nslen characters; the
     * receiver terminates it itself. */
    char nspace[PMIX_MAX_NSLEN + 1];
} prte_oob_tcp_hdr_t;

/* the part of the header that is always present, and the length of a
 * particular header on the wire.  Both take the nslen in *host* order, which
 * is every order: it is a single byte. */
#define PRTE_OOB_TCP_HDR_FIXED   ((size_t) offsetof(prte_oob_tcp_hdr_t, nspace))
#define PRTE_OOB_TCP_HDR_LEN(h)  (PRTE_OOB_TCP_HDR_FIXED + (size_t) (h)->nslen)

/* load the nspace the two ranks share */
#define PRTE_OOB_TCP_HDR_LOAD_NSPACE(h, ns)                 \
    do {                                                    \
        size_t _l = strlen(ns);                             \
        if (PMIX_MAX_NSLEN < _l) {                          \
            _l = PMIX_MAX_NSLEN;                            \
        }                                                   \
        memcpy((h)->nspace, (ns), _l);                      \
        (h)->nspace[_l] = '\0';                             \
        (h)->nslen = (uint8_t) _l;                          \
    } while (0)

/* terminate the nspace a peer just handed us.  Called once the nslen
 * characters have been read, and before anything reads the field. */
#define PRTE_OOB_TCP_HDR_END_NSPACE(h)  ((h)->nspace[(h)->nslen] = '\0')

/* rebuild one of the two procids the header no longer carries whole */
#define PRTE_OOB_TCP_HDR_PROC(h, r, p)  PMIX_LOAD_PROCID((p), (h)->nspace, (r))

/**
 * Convert the message header to host byte order
 */
#define MCA_OOB_TCP_HDR_NTOH(h)                     \
    do {                                            \
        (h)->origin = ntohl((h)->origin);           \
        (h)->dst = ntohl((h)->dst);                 \
        (h)->tag = PRTE_RML_TAG_NTOH((h)->tag);     \
        (h)->seq_num = ntohl((h)->seq_num);         \
        (h)->nbytes = ntohl((h)->nbytes);           \
        (h)->epoch = prte_ntoh64((h)->epoch);       \
    } while (0)

/**
 * Convert the message header to network byte order
 */
#define MCA_OOB_TCP_HDR_HTON(h)                     \
    do {                                            \
        (h)->origin = htonl((h)->origin);           \
        (h)->dst = htonl((h)->dst);                 \
        (h)->tag = PRTE_RML_TAG_HTON((h)->tag);     \
        (h)->seq_num = htonl((h)->seq_num);         \
        (h)->nbytes = htonl((h)->nbytes);           \
        (h)->epoch = prte_hton64((h)->epoch);       \
    } while (0)

#endif /* _MCA_OOB_TCP_HDR_H_ */
