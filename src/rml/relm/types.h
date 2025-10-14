/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file:
 *
 * RELM types
 */

#ifndef PRTE_RELM_TYPES_H
#define PRTE_RELM_TYPES_H

#include <stdint.h>
#include "src/pmix/pmix-internal.h"
#include "src/class/pmix_hash_table.h"

BEGIN_C_DECLS

typedef uint8_t prte_relm_state_t;
#define PRTE_RELM_STATE PMIX_UINT8
enum {
    // Lasting states are possibly saved as a message's state
    PRTE_RELM_STATE_INVALID,   // Default constructed message state
    PRTE_RELM_STATE_SENT,      // Msg sent downstream
    PRTE_RELM_STATE_REQUESTED, // Msg replay requested
    PRTE_RELM_STATE_SENDING,   // Msg queued for send in OOB
    PRTE_RELM_STATE_PENDING,   // Msg post pending prior msg's post
    PRTE_RELM_STATE_ACKED,     // Msg ACK sent upstream

    // Ephemeral states are used to invoke state updates, but should never be
    // saved as a message's state.
    PRTE_RELM_EPHEMERAL_STATES_START,
    PRTE_RELM_STATE_NEW,       // Msg locally launched
    PRTE_RELM_STATE_ACKACKED,  // Msg ACK-ACK received from upstream
    PRTE_RELM_STATE_CACHED,    // Msg data being cached
    PRTE_RELM_STATE_EVICTED,   // Msg data being destroyed
};

// Locally unique ID type. Allowed to wrap around, as we assume old messages
// will be globally completed and have no more references to them by the time
// we would re-use the UID.
typedef uint32_t prte_relm_uid_t;
#define PRTE_RELM_UID          PMIX_UINT32
#define PRTE_RELM_UID_UNKNOWN  UINT32_MAX
#define PRTE_RELM_UID_NONE     (PRTE_RELM_UID_UNKNOWN-1)
#define PRTE_RELM_UID_INVALID  (PRTE_RELM_UID_NONE-1)
#define PRTE_RELM_UID_MAX      (PRTE_RELM_UID_INVALID-1)

// Hold state and (optionally) data for a message
typedef struct {
    pmix_list_item_t super;

    // The pair <src,uid> forms a globally unique id
    pmix_rank_t src;
    prte_relm_uid_t uid;
    // The set <src,uid,dst> forms a message's signature
    pmix_rank_t dst;

    // Ensure correct message ordering
    prte_relm_uid_t prev_uid;
    prte_relm_uid_t next_uid;

    // Current message state
    prte_relm_state_t state;

    // Possibly null!
    pmix_byte_object_t data;

    // Evict cached data with timer
    bool cached;
    prte_event_t eviction_ev;
} prte_relm_msg_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_relm_msg_t);

// Signature can be used to locally find a message
typedef struct {
    pmix_rank_t src;
    pmix_rank_t dst;
    prte_relm_uid_t uid;
} prte_relm_signature_t;
#define PRTE_RELM_SIGNATURE_STATIC_INIT { \
    .src = PMIX_RANK_INVALID, \
    .dst = PMIX_RANK_INVALID, \
    .uid = PRTE_RELM_UID_INVALID \
}

// Globally unique ID that serves as a hash
typedef uint64_t prte_relm_guid_t;
#define PRTE_RELM_GUID(s) \
 ( (((prte_relm_guid_t)((s)->src)) << 32) | (s)->uid )

// Hold each active destination's state
typedef struct {
    pmix_object_t super;

    // prte_relm_guid_t -> prte_relm_msg_t
    pmix_hash_table_t msgs;

    // UID of the last locally-started ongoing message to this rank
    prte_relm_uid_t my_last_msg;
} prte_relm_rank_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_relm_rank_t);

END_C_DECLS

#endif
