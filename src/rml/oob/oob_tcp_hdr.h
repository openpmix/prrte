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

#include "types.h"

/* Message types carried in the TCP header. IDENT and PROBE are used
 * during the connection handshake; USER marks a normal RML message,
 * whether it is destined for us or is being relayed on to the next hop.
 */
typedef uint8_t prte_oob_tcp_msg_type_t;

#define MCA_OOB_TCP_IDENT 1
#define MCA_OOB_TCP_PROBE 2
#define MCA_OOB_TCP_USER  4

/* header for tcp msgs */
typedef struct {
    /* the originator of the message - when relaying, this is the
     * process that first sent the message, not necessarily our peer
     */
    pmix_proc_t origin;
    /* the intended final recipient. If it is not us, we relay the
     * message onward toward that process using the routing tree
     */
    pmix_proc_t dst;
    /* the rml tag where this message is headed */
    prte_rml_tag_t tag;
    /* the seq number of this message */
    uint32_t seq_num;
    /* number of bytes in message */
    uint32_t nbytes;
    /* boot epoch (incarnation) of the origin. A daemon that departs and reboots
     * into the same rank comes back with a strictly-greater epoch, so a hop can
     * drop late traffic stamped with the stale incarnation's epoch. */
    uint64_t epoch;
    /* type of message */
    prte_oob_tcp_msg_type_t type;
} prte_oob_tcp_hdr_t;
/**
 * Convert the message header to host byte order
 */
#define MCA_OOB_TCP_HDR_NTOH(h)                 \
    (h)->origin.rank = ntohl((h)->origin.rank); \
    (h)->dst.rank = ntohl((h)->dst.rank);       \
    (h)->tag = PRTE_RML_TAG_NTOH((h)->tag);     \
    (h)->nbytes = ntohl((h)->nbytes);           \
    (h)->epoch = prte_ntoh64((h)->epoch);

/**
 * Convert the message header to network byte order
 */
#define MCA_OOB_TCP_HDR_HTON(h)                 \
    (h)->origin.rank = htonl((h)->origin.rank); \
    (h)->dst.rank = htonl((h)->dst.rank);       \
    (h)->tag = PRTE_RML_TAG_HTON((h)->tag);     \
    (h)->nbytes = htonl((h)->nbytes);           \
    (h)->epoch = prte_hton64((h)->epoch);

#endif /* _MCA_OOB_TCP_HDR_H_ */
