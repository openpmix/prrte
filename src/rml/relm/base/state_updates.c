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

#include "constants.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

#include "src/rml/rml.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/types.h"
#include "src/rml/relm/util.h"
#include "src/rml/relm/base/state_machine.h"

// Handle a state update started by a downstream's message
static void downstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
);

// Handle a state update started by an upstream's message
static void upstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
);

// Handle a state update requested locally
static void local_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
);

// Callback for cache timeout events. Calls a state update on msg in cb_data
static void evict(int fd, short args, void* cb_data);

int prte_relm_base_pack_state_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg
) {
    int ret = PMIX_SUCCESS;
    if(NULL == buf || NULL == msg) ret = PRTE_ERR_BAD_PARAM;

    if(PMIX_SUCCESS == ret) ret = prte_relm_pack_signature(buf, msg);
    if(PMIX_SUCCESS == ret) ret = prte_relm_pack_uid(buf, msg->prev_uid);
    if(PMIX_SUCCESS == ret) ret = prte_relm_pack_state(buf, msg->state);

    if(PRTE_RELM_STATE_SENDING == msg->state && PMIX_SUCCESS == ret){
        ret = prte_relm_pack_data(buf, msg->data);
    }

    if(PMIX_SUCCESS != ret){
        PRTE_RELM_MSG_ERROR_LOG(msg, ret);
    }
    return ret;
}

void prte_relm_base_update_state(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state,
    pmix_rank_t src
) {
    if(PRTE_PROC_MY_NAME->rank == src){
        local_update(buf, msg, state);
    } else if(prte_relm_sm->downstream_rank(msg) == src){
        downstream_update(buf, msg, state);
    } else if(prte_relm_sm->upstream_rank(msg) == src){
        upstream_update(buf, msg, state);
    }
    //Ignore lingering messages from old links
}

static void downstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
) {
    PRTE_HIDE_UNUSED_PARAMS(buf);
    switch(state){
    case PRTE_RELM_STATE_ACKED:
        if(PRTE_RELM_STATE_INVALID == msg->state){
            // Unknown msg, must have been ACKACKED already
            prte_relm_update_state(msg, PRTE_RELM_STATE_ACKACKED);
            break;
        }
        prte_relm_update_state(msg, PRTE_RELM_STATE_ACKED);
        break;

    case PRTE_RELM_STATE_REQUESTED:
        prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
        break;

    default:
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
    }
}

static void upstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
) {
    switch(state){
    case PRTE_RELM_STATE_SENDING: {
        pmix_byte_object_t bo = prte_relm_unpack_data(buf);
        if(0 == bo.size){
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else if(NULL != msg->data.bytes){
            if(bo.size != msg->data.size){
                PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_OP_IN_PROGRESS);
                // TODO: GUID clash = job failure?
            }
            PMIx_Byte_object_destruct(&bo);
        } else {
            msg->data = bo;
        }
        prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
        break;
    }

    case PRTE_RELM_STATE_ACKACKED:
        prte_relm_update_state(msg, PRTE_RELM_STATE_ACKACKED);
        break;

    // The rest will only be sent from upstream during a link update,
    // which lets us simplify the logic at times
    case PRTE_RELM_STATE_SENT:
        prte_relm_update_state(msg, PRTE_RELM_STATE_SENT);
        break;

    case PRTE_RELM_STATE_ACKED:
        // Must either be invalid or acked currently, so no full update needed
        msg->state = PRTE_RELM_STATE_ACKED;
        break;

    case PRTE_RELM_STATE_REQUESTED:
        if(PRTE_RELM_STATE_ACKED == msg->state){
            PRTE_RELM_MSG_OUTPUT(1, msg, "replaying ack");
            prte_relm_send_state_upstream(msg);
        } else if(PRTE_RELM_STATE_INVALID == msg->state){
            PRTE_RELM_MSG_OUTPUT(1, msg, "requesting replay");
            prte_relm_update_state(msg, PRTE_RELM_STATE_REQUESTED);
        }
        break;

    default:
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
    }
}

static void local_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
) {
    switch (state){
    case PRTE_RELM_STATE_SENT:
        if(PRTE_PROC_MY_NAME->rank == msg->dst){
            // Can't have sent the message downstream if I'm the destination
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else if(PRTE_RELM_STATE_SENDING == msg->state){
            msg->state = state;
            pmix_rank_t r = PRTE_PROC_MY_NAME->rank;
            if(r != msg->src && r != msg->dst){
                prte_relm_update_state(msg, PRTE_RELM_STATE_CACHED);
            }
        } else if(PRTE_RELM_STATE_ACKED == msg->state
                  || PRTE_RELM_STATE_REQUESTED == msg->state){
            prte_relm_send_state_upstream(msg);
        } else if(PRTE_RELM_STATE_INVALID == msg->state) {
            msg->state = state;
        }
        break;

    case PRTE_RELM_STATE_REQUESTED:
        if(PRTE_RELM_STATE_SENT == msg->state
           || PRTE_RELM_STATE_INVALID == msg->state){
            if(NULL != msg->data.bytes){
                PRTE_RELM_MSG_OUTPUT(1, msg, "replaying");
                prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
            } else {
                msg->state = state;
                prte_relm_send_state_upstream(msg);
            }
        } else if(PRTE_RELM_STATE_ACKED == msg->state){
            prte_relm_send_state_upstream(msg);
        }
        break;

    case PRTE_RELM_STATE_SENDING:
        if(PRTE_RELM_STATE_ACKED == msg->state) {
            prte_relm_send_state_upstream(msg);
            prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);
        } else if(NULL == msg->data.bytes){
            prte_relm_update_state(msg, PRTE_RELM_STATE_REQUESTED);
        } else if(PRTE_PROC_MY_NAME->rank == msg->dst) {
            if(prte_relm_prev_is_posted(msg)){
                msg->state = PRTE_RELM_STATE_SENT;
                prte_relm_post(msg);
                prte_relm_update_state(msg, PRTE_RELM_STATE_ACKED);
            } else {
                prte_relm_update_state(msg, PRTE_RELM_STATE_PENDING);
            }
        } else if(PRTE_RELM_STATE_SENDING != msg->state) {
            msg->state = state;
            prte_relm_send_state_downstream(msg);
        }
        break;

    case PRTE_RELM_STATE_PENDING:
        if(PRTE_RELM_STATE_ACKED == msg->state){
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else {
            msg->state = state;
        }
        break;

    case PRTE_RELM_STATE_ACKED: {
        if(PRTE_PROC_MY_NAME->rank == msg->src){
            msg->state = PRTE_RELM_STATE_ACKED;
            prte_relm_update_state(msg, PRTE_RELM_STATE_ACKACKED);
            break;
        } else if(state == msg->state) break;

        msg->state = state;
        prte_relm_send_state_upstream(msg);
        prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);

        // Previous messages are implicitly acked
        prte_relm_msg_t* prev = msg;
        while(NULL != (prev = prte_relm_find_prev_msg(prev))){
            if(state == prev->state) break;
            prev->state = state;
            prte_relm_update_state(prev, PRTE_RELM_STATE_EVICTED);
        }

        prte_relm_msg_t* next = prte_relm_find_next_msg(msg);
        if(NULL != next && PRTE_RELM_STATE_PENDING == next->state){
            prte_relm_update_state(next, PRTE_RELM_STATE_SENDING);
        }
        break;
    }

    case PRTE_RELM_STATE_NEW:
        if(PRTE_RELM_STATE_INVALID != msg->state){
            // Either two attempts to start the same msg, or somehow a new
            // msg was given the same uid as an existing msg
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_OP_IN_PROGRESS);
            // TODO: GUID clash = job failure?
        } else if(NULL == buf){
            // We need the msg data for this state update
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else {
            PMIx_Data_unload(buf, &msg->data);

            prte_relm_rank_t* rank = prte_relm_get_rank(msg->dst);
            msg->prev_uid = rank->my_last_msg;
            rank->my_last_msg = msg->uid;

            prte_relm_msg_t* prev_msg = prte_relm_find_prev_msg(msg);
            if(NULL != prev_msg){
                prev_msg->next_uid = msg->uid;
            }

            prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
        }
        break;

    case PRTE_RELM_STATE_ACKACKED:
        if(PRTE_PROC_MY_NAME->rank != msg->dst){
            msg->state = state;
            prte_relm_send_state_downstream(msg);
        }
        if(PRTE_PROC_MY_NAME->rank == msg->src){
            prte_relm_rank_t* rank = prte_relm_get_rank(msg->dst);
            if(rank->my_last_msg == msg->uid){
                rank->my_last_msg = PRTE_RELM_UID_NONE;
            }
        }
        prte_relm_release_msg(msg);
        break;

    case PRTE_RELM_STATE_CACHED: {
        if(NULL == msg->data.bytes){
            // Don't cache without the data
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
            break;
        }

        if(msg->cached){
            pmix_list_remove_item(&prte_relm_sm->cached_messages, &msg->super);
            prte_event_evtimer_del(&msg->eviction_ev);
        }

        msg->cached = true;
        pmix_list_append(&prte_relm_sm->cached_messages, &msg->super);
        // Cache timeout event calls update to evicted state
        prte_event_evtimer_set(prte_event_base, &msg->eviction_ev, evict, msg);
        prte_event_evtimer_add(&msg->eviction_ev, &prte_relm_sm->cache_tv);

        size_t n = pmix_list_get_size(&prte_relm_sm->cached_messages);
        if(n > prte_relm_sm->max_cache_count){
            prte_relm_msg_t* first = (prte_relm_msg_t*)
                pmix_list_get_first(&prte_relm_sm->cached_messages);
            prte_relm_update_state(first, PRTE_RELM_STATE_EVICTED);
        }
        break;
    }

    case PRTE_RELM_STATE_EVICTED:
        if(msg->cached){
            msg->cached = false;
            pmix_list_remove_item(&prte_relm_sm->cached_messages, &msg->super);
            prte_event_evtimer_del(&msg->eviction_ev);
        }
        if(NULL != msg->data.bytes) {
            PMIx_Byte_object_destruct(&msg->data);
        }
        break;

    default:
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
    }
}

static void evict(int fd, short args, void* cb_data){
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
    prte_relm_msg_t* msg = (prte_relm_msg_t*) cb_data;
    prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);
}
