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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

/* Message types carried in the TCP header. The three AUTH types, then IDENT
 * (and PROBE), make up the connection handshake; USER marks a normal RML
 * message, whether it is destined for us or is being relayed on to the next
 * hop.
 */
typedef uint8_t prte_oob_tcp_msg_type_t;

#define MCA_OOB_TCP_IDENT 1
#define MCA_OOB_TCP_PROBE 2
#define MCA_OOB_TCP_USER  4
/* Before anything else crosses a new connection, each end proves it holds the
 * DVM key (src/util/prte_dvm_key.h).  The dialer sends HELLO with a fresh
 * nonce; the listener answers CHALLENGE with its own nonce and its proof; the
 * dialer checks that proof and answers RESPONSE with its own.  Each proof is
 * an HMAC, under the key, of both nonces, both ranks and the namespace, with
 * a byte saying which end made it (prte_oob_tcp_auth_mac) - so neither end's
 * proof can be replayed, reflected back at it, or presented on another
 * connection.  Only then does the IDENT exchange that has always opened a
 * connection take place, untouched: everything it decides - including which
 * of two simultaneous connections survives - is decided between daemons that
 * have already shown they belong to the DVM. */
#define MCA_OOB_TCP_AUTH_HELLO     8
#define MCA_OOB_TCP_AUTH_CHALLENGE 16
#define MCA_OOB_TCP_AUTH_RESPONSE  32

/* header for tcp msgs
 *
 * Only the first PRTE_OOB_TCP_HDR_LEN() bytes of this struct go on the wire,
 * and for a data message that is just the fixed part: **the nspace is sent
 * only by the connect handshake**.  This header rides *every* RML message, so
 * what it does not carry matters.  It began as two whole `pmix_proc_t` - 552
 * bytes, of which 512 were two fixed 256-byte nspace arrays holding the same
 * short string - which on a launch-message cpuset slice for an eight-process
 * node was 85% of a 101-byte message.
 *
 * A data message needs no nspace at all, because there is only ever one:
 *
 *  - **Every OOB endpoint belongs to a daemon of this DVM.**  The transport is
 *    opened by `ess/hnp` and by the standard prted path, and by nothing else -
 *    no tool and no application process has one.  So a peer is always a daemon
 *    of our own job.
 *  - **Every send entry point takes a rank**, and `send_buffer()` builds the
 *    destination as that rank in `PRTE_PROC_MY_NAME->nspace`
 *    (`prte_oob_base_send_nb` stamps the hop the same way).  A foreign
 *    namespace is not merely unused, it is unrepresentable.
 *  - A **relay** forwards traffic of its own job, so both names stay ours.
 *
 * The receiver therefore reconstructs both procids with its own namespace, and
 * the handshake is what makes that safe rather than merely true: an IDENT
 * carries the connecting daemon's nspace and `tcp_peer_read_handshake`
 * **refuses a peer whose nspace is not ours**.  The invariant is checked once
 * per connection instead of being restated on every message - and instead of
 * being left for a reader to derive.
 *
 * That leaves `nslen` describing the wire length by itself: a handshake header
 * sets it, a data header sets it to zero, and both are read the same way.
 *
 * (One thing that historically wanted to cross a namespace here was a data
 * server hosted by another DVM.  It never worked over the RML - the namespace
 * was silently discarded - and it now uses a PMIx tool connection instead.  See
 * `src/runtime/data_server/AGENTS.md`.)
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
     * sent.  ZERO on a data message, which carries no nspace at all; non-zero
     * only on the handshake.  A uint8_t holds every legal length because
     * PMIX_MAX_NSLEN is 255, which is also why no bound check is needed on
     * the receiving side. */
    uint8_t nslen;
    /* the nspace the two ranks above belong to, sent by the handshake alone.
     * Sent as nslen characters; the receiver terminates it itself. */
    char nspace[PMIX_MAX_NSLEN + 1];
} prte_oob_tcp_hdr_t;

/* How far a connection has got in proving that the far end holds the DVM
 * key.  A dialer goes NONE -> HELLO_SENT -> DONE, a listener NONE ->
 * CHALLENGE_SENT -> DONE; nothing but the next authentication message is
 * accepted on a connection until it reads DONE. */
#define PRTE_OOB_TCP_AUTH_NONE           0
#define PRTE_OOB_TCP_AUTH_HELLO_SENT     1
#define PRTE_OOB_TCP_AUTH_CHALLENGE_SENT 2
#define PRTE_OOB_TCP_AUTH_DONE           3

/* the length of a nonce and of a proof - both are HMAC-SHA256 outputs */
#define PRTE_OOB_TCP_AUTH_LEN 32

typedef struct {
    int phase;
    /* the far end's rank: the one we dialed, or the one a dialer claimed in
     * its HELLO.  Once phase is DONE this is the rank the far end has proved
     * it may speak for, and its IDENT must name the same one. */
    pmix_rank_t rank;
    uint8_t dialer_nonce[PRTE_OOB_TCP_AUTH_LEN];
    uint8_t listener_nonce[PRTE_OOB_TCP_AUTH_LEN];
} prte_oob_tcp_auth_t;

/* A connect handshake part way through being read.
 *
 * A handshake message is the fixed header, the nspace it names and a small
 * payload, and nothing promises the three arrive together - or at
 * all, since the listening port answers anyone who connects to it.  It is
 * read on the progress thread, so it is read as the bytes arrive: each read
 * event takes what the socket has and records here how far it got, and the
 * next one carries on.  Waiting for the rest instead is what let one stray
 * byte on the port stop a daemon dead.
 *
 * `hdr` holds network byte order until `sized` is set, which happens once the
 * header and its nspace are complete and have been checked; from then on it is
 * host order and `payload` is allocated.  prte_oob_tcp_handshake_reset()
 * readies one of these for the next message of the same handshake;
 * prte_oob_tcp_handshake_clear() empties it for a new connection. */
typedef struct {
    prte_oob_tcp_hdr_t hdr;
    size_t hdr_rcvd;     // bytes of hdr - fixed part, then nspace - read so far
    bool sized;          // header complete, checked, converted; payload allocated
    char *payload;       // hdr.nbytes long, plus a terminator the reader adds
    size_t payload_rcvd;
    /* Spans the several messages of one connection's handshake, so it
     * survives prte_oob_tcp_handshake_reset() - which readies the record for
     * the next message - and is emptied only by prte_oob_tcp_handshake_clear()
     * when the connection itself is new or gone. */
    prte_oob_tcp_auth_t auth;
} prte_oob_tcp_handshake_t;

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
 * characters have been read, and before anything reads the field.  With no
 * nspace on the wire this makes the field an empty string, which is what the
 * accessor below tests for by way of nslen. */
#define PRTE_OOB_TCP_HDR_END_NSPACE(h)  ((h)->nspace[(h)->nslen] = '\0')

/* The namespace the header's ranks belong to: the one the handshake sent, or
 * - for a data message, which sends none - our own.  Those are the same
 * namespace; see the note above for why that is guaranteed rather than
 * assumed. */
#define PRTE_OOB_TCP_HDR_NSPACE(h) \
    ((0 == (h)->nslen) ? PRTE_PROC_MY_NAME->nspace : (h)->nspace)

/* rebuild one of the two procids the header no longer carries whole */
#define PRTE_OOB_TCP_HDR_PROC(h, r, p) \
    PMIX_LOAD_PROCID((p), PRTE_OOB_TCP_HDR_NSPACE(h), (r))

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
