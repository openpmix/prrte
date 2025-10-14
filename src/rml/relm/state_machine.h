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
 * RELM state machine object and base functions
 */

#ifndef PRTE_RELM_STATE_MACHINE_H
#define PRTE_RELM_STATE_MACHINE_H

#include <stdint.h>

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/pmix/pmix-internal.h"
#include "src/rml/relm/types.h"

BEGIN_C_DECLS

// Default construct a rank's state
typedef prte_relm_rank_t* (*prte_relm_new_rank_fn_t)(void);
// Pack msg information for a new link after a failure
typedef int (*prte_relm_pack_link_update_fn_t)(
    pmix_data_buffer_t* buf, pmix_rank_t link
);
// Recv msg information from a new link after a failure
typedef void (*prte_relm_update_link_fn_t)(
    pmix_data_buffer_t* buf, pmix_rank_t link
);

// Default construct a message's state
typedef prte_relm_msg_t* (*prte_relm_new_msg_fn_t)(void);
// Pack any state information and/or data for a msg state update
typedef int (*prte_relm_pack_state_update_fn_t)(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg
);
// Handle a single msg's state update.
// buf is NULL unless this update is associated with a recvd message buffer.
typedef void (*prte_relm_update_state_fn_t)(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state,
    pmix_rank_t src
);

// Get the next rank in this msg's path from src->dst or dst->src
typedef pmix_rank_t (*prte_relm_upstream_rank_fn_t)(prte_relm_msg_t* msg);
typedef pmix_rank_t (*prte_relm_downstream_rank_fn_t)(prte_relm_msg_t* msg);

typedef void (*prte_relm_fault_handler_fn_t)(
    const prte_rml_recovery_status_t* status
);

typedef struct {
    pmix_object_t super;

    // Behavior customization points for different implementations
    // Most can be reused from the base implementation
    prte_relm_new_rank_fn_t          new_rank;
    prte_relm_pack_link_update_fn_t  pack_link_update;
    prte_relm_update_link_fn_t       update_link;
    prte_relm_new_msg_fn_t           new_msg;
    prte_relm_pack_state_update_fn_t pack_state_update;
    prte_relm_update_state_fn_t      update_state;
    prte_relm_upstream_rank_fn_t     upstream_rank;
    prte_relm_downstream_rank_fn_t   downstream_rank;
    prte_relm_fault_handler_fn_t     fault_handler;

    // Messages bound for a given rank, if any are currently in progress.
    pmix_hash_table_t ranks; // pmix_rank_t -> prte_relm_rank_t

    // Same message objects as in the ranks above. Don't release these when
    // they are evicted, so we can keep the state - but do release the msg data
    pmix_list_t cached_messages; // prte_relm_msg_t
    uint32_t max_cache_count;    // Remove first msg if caching n+1th msg
    struct timeval cache_tv;     // Remove msg from cache after time

    // The next UID to use for a locally-generated message
    prte_relm_uid_t next_uid;

    // Links that I have received any expected updates from after promotions
    pmix_bitmap_t upstream_links_updated;
    // Links that I have sent any expected updates to after faults
    pmix_bitmap_t downstream_links_updated;
} prte_relm_state_machine_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_relm_state_machine_t);

PRTE_EXPORT extern prte_relm_state_machine_t* prte_relm_sm;


// Find a state object if it exists, or return NULL
prte_relm_msg_t* prte_relm_find_msg(prte_relm_signature_t* sig);
// Prev/Next msgs are for a given src->dst
prte_relm_msg_t* prte_relm_find_next_msg(prte_relm_msg_t* msg);
prte_relm_msg_t* prte_relm_find_prev_msg(prte_relm_msg_t* msg);
prte_relm_rank_t* prte_relm_find_rank(pmix_rank_t dst);

// Find or construct a state object. Return NULL only if inputs invalid.
prte_relm_msg_t* prte_relm_get_msg(prte_relm_signature_t* sig);
prte_relm_msg_t* prte_relm_get_prev_msg(prte_relm_msg_t* msg);
prte_relm_rank_t* prte_relm_get_rank(pmix_rank_t dst);

// Create a new message from a local reliable_send call and start it
int prte_relm_start_msg(
    pmix_rank_t dst, pmix_data_buffer_t* buf, prte_rml_tag_t tag
);

// Release a msg and recursively release any prev_msgs.
// Sets msg->next_uid's prev_uid to NONE
void prte_relm_release_msg(prte_relm_msg_t* msg);

// Update msg's local state through the state handler
void prte_relm_update_state(prte_relm_msg_t* msg, prte_relm_state_t state);
// Send msg's current state toward its destination
void prte_relm_send_state_downstream(prte_relm_msg_t* msg);
// Send msg's current state toward its source
// Skips if expecting an upstream link update
void prte_relm_send_state_upstream(prte_relm_msg_t* msg);

void prte_relm_send_link_update(pmix_rank_t link);

// Handle a received relm message's buffer, converting to correct sm calls
void prte_relm_message_handler(pmix_rank_t src, pmix_data_buffer_t* buf);
void prte_relm_link_update_handler(pmix_rank_t src, pmix_data_buffer_t* buf);


END_C_DECLS

#endif
