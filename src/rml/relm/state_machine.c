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
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"

#include "src/rml/rml.h"
#include "src/rml/oob/oob.h"
#include "src/rml/oob/oob_tcp_common.h"

#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/types.h"
#include "src/rml/relm/util.h"
#include "src/rml/relm/base/base.h"

prte_relm_state_machine_t* prte_relm_sm = NULL;

prte_relm_rank_t* prte_relm_find_rank(pmix_rank_t r){
    if(r >= prte_rml_base.n_dmns){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    prte_relm_rank_t* rank = NULL;
    int ret = pmix_hash_table_get_value_uint32(
        &prte_relm_sm->ranks, r, (void**)&rank
    );
    if(PMIX_SUCCESS != ret && PMIX_ERR_NOT_FOUND != ret){
        PMIX_ERROR_LOG(ret);
    }
    return rank;
}

prte_relm_msg_t* prte_relm_find_msg(prte_relm_signature_t* sig){
    if(sig->src >= prte_rml_base.n_dmns){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    } else if(sig->uid > PRTE_RELM_UID_MAX){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    prte_relm_rank_t* dst = prte_relm_find_rank(sig->dst);
    if(NULL == dst) return NULL;

    prte_relm_msg_t* msg = NULL;
    int ret = pmix_hash_table_get_value_uint64(
        &dst->msgs, PRTE_RELM_GUID(sig), (void**)&msg
    );
    if(PMIX_SUCCESS != ret && PMIX_ERR_NOT_FOUND != ret){
        PMIX_ERROR_LOG(ret);
    }
    return msg;
}

prte_relm_rank_t* prte_relm_get_rank(pmix_rank_t r){
    if(r >= prte_rml_base.n_dmns){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    prte_relm_rank_t* rank = prte_relm_find_rank(r);
    if(NULL != rank) return rank;

    rank = prte_relm_sm->new_rank();
    int ret = pmix_hash_table_set_value_uint32(&prte_relm_sm->ranks, r, rank);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(rank);
        return NULL;
    }
    return rank;
}

prte_relm_msg_t* prte_relm_get_msg(prte_relm_signature_t* sig){
    if(sig->src >= prte_rml_base.n_dmns){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    } else if(sig->uid > PRTE_RELM_UID_MAX){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    prte_relm_msg_t* msg = prte_relm_find_msg(sig);
    if(NULL != msg) return msg;

    prte_relm_rank_t* rank = prte_relm_get_rank(sig->dst);
    if(rank == NULL) return NULL;

    msg = prte_relm_sm->new_msg();
    msg->src = sig->src;
    msg->uid = sig->uid;
    msg->dst = sig->dst;

    int ret = pmix_hash_table_set_value_uint64(
        &rank->msgs, PRTE_RELM_GUID(sig), msg
    );
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(rank);
        return NULL;
    }
    return msg;
}

prte_relm_msg_t* prte_relm_find_next_msg(prte_relm_msg_t* msg){
    if(PRTE_RELM_UID_NONE == msg->next_uid) return NULL;
    if(PRTE_RELM_UID_MAX < msg->next_uid){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }
    prte_relm_signature_t sig = {
        .src = msg->src,
        .uid = msg->next_uid,
        .dst = msg->dst
    };
    return prte_relm_find_msg(&sig);
}

prte_relm_msg_t* prte_relm_find_prev_msg(prte_relm_msg_t* msg){
    if(PRTE_RELM_UID_MAX < msg->prev_uid){
        return NULL;
    }
    prte_relm_signature_t sig = {
        .src = msg->src,
        .uid = msg->prev_uid,
        .dst = msg->dst
    };
    return prte_relm_find_msg(&sig);
}

prte_relm_msg_t* prte_relm_get_prev_msg(prte_relm_msg_t* msg){
    if(PRTE_RELM_UID_INVALID == msg->prev_uid){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
    }
    if(PRTE_RELM_UID_MAX < msg->prev_uid) return NULL;
    prte_relm_signature_t sig = {
        .src = msg->src,
        .uid = msg->prev_uid,
        .dst = msg->dst
    };
    return prte_relm_get_msg(&sig);
}

typedef struct {
    pmix_object_t super;
    pmix_rank_t dst;
    pmix_data_buffer_t* data;
    prte_event_t ev;
} start_msg_caddy_t;
static void con_msg_cd(start_msg_caddy_t* ptr){
    ptr->dst = PMIX_RANK_INVALID;
    ptr->data = NULL;
}
static void des_msg_cd(start_msg_caddy_t* ptr){
    if(ptr->data){
        PMIx_Data_buffer_release(ptr->data);
    }
}
PMIX_CLASS_INSTANCE(start_msg_caddy_t, pmix_object_t, con_msg_cd, des_msg_cd);

static void prte_relm_start_msg_cb(int fd, short argn, void* cbdata){
    PRTE_HIDE_UNUSED_PARAMS(fd, argn);
    start_msg_caddy_t* cd = (start_msg_caddy_t*) cbdata;

    prte_relm_signature_t sig = {
        .src = PRTE_PROC_MY_NAME->rank,
        .uid = prte_relm_sm->next_uid++,
        .dst = cd->dst
    };
    prte_relm_msg_t* msg = prte_relm_get_msg(&sig);
    PRTE_RELM_MSG_OUTPUT(2, msg, "updating state to NEW");

    prte_relm_sm->update_state(
        cd->data, msg, PRTE_RELM_STATE_NEW, PRTE_PROC_MY_NAME->rank
    );

    PMIX_RELEASE(cd);
}

int prte_relm_start_msg(
    pmix_rank_t dst, pmix_data_buffer_t* buf, prte_rml_tag_t tag
) {
    if(NULL == buf){
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    } else if(dst > prte_rml_base.n_dmns){
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    } else if(!prte_rml_is_node_up(dst)){
        PRTE_ERROR_LOG(PRTE_ERR_NODE_DOWN);
        return PRTE_ERR_NODE_DOWN;
    }

    if(PRTE_PROC_MY_HNP->rank == dst){
        // Special handling for messages addressed to HNP when we haven't yet
        // done wireup - let OOB send it directly.
        if(NULL == prte_oob_tcp_peer_lookup(PRTE_PROC_MY_PARENT)){
            return prte_rml_send_buffer_nb(dst, buf, tag);
        } else if (!prte_routing_is_enabled) {
            return prte_rml_send_buffer_nb(dst, buf, tag);
        }

    }

    pmix_data_buffer_t* data = PMIx_Data_buffer_create();
    pmix_byte_object_t bo = PMIX_BYTE_OBJECT_STATIC_INIT;

    int ret = PMIX_SUCCESS;
    if(PMIX_SUCCESS == ret){
        ret = PMIx_Data_pack(NULL, data, &tag, 1, PRTE_RML_TAG);
        if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    }
    if(PMIX_SUCCESS == ret){
        ret = PMIx_Data_unload(buf, &bo);
        if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    }
    if(PMIX_SUCCESS == ret){
        ret = PMIx_Data_pack(NULL, data, &bo, 1, PMIX_BYTE_OBJECT);
        if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    }
    PMIx_Byte_object_destruct(&bo);
    if(PMIX_SUCCESS != ret){
        PMIx_Data_buffer_release(data);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return ret;
    }

    start_msg_caddy_t* msg_cd = PMIX_NEW(start_msg_caddy_t);
    msg_cd->dst = dst;
    msg_cd->data = data;
    PRTE_PMIX_THREADSHIFT(msg_cd, prte_event_base, prte_relm_start_msg_cb);

    return PMIX_SUCCESS;
}

void prte_relm_release_msg(prte_relm_msg_t* msg){
    if(NULL == msg) return;

    prte_relm_rank_t* rank = prte_relm_get_rank(msg->dst);
    prte_relm_msg_t* prev = prte_relm_find_prev_msg(msg);
    while(NULL != prev){
        prte_relm_msg_t* p = prte_relm_find_prev_msg(prev);
        pmix_hash_table_remove_value_uint64(&rank->msgs, PRTE_RELM_GUID(prev));
        PMIX_RELEASE(prev);
        prev = p;
    }

    prte_relm_msg_t* next = prte_relm_find_next_msg(msg);
    if(NULL != next){
        next->prev_uid = PRTE_RELM_UID_NONE;
    }

    pmix_hash_table_remove_value_uint64(&rank->msgs, PRTE_RELM_GUID(msg));
    if(0 == pmix_hash_table_get_size(&rank->msgs)){
        pmix_hash_table_remove_value_uint32(&prte_relm_sm->ranks, msg->dst);
        PMIX_RELEASE(rank);
    }

    PMIX_RELEASE(msg);
}

void prte_relm_update_state(prte_relm_msg_t* msg, prte_relm_state_t state){
    PRTE_RELM_MSG_OUTPUT_VERBOSE(
        2, msg, "updating state to %s\n", prte_relm_state_name(state)
    );
    if(PRTE_RELM_EPHEMERAL_STATES_START <= msg->state){
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }
    prte_relm_sm->update_state(NULL, msg, state, PRTE_PROC_MY_NAME->rank);
    // Can't check for ephemeral states after, msg may have been released
}

static void sending_to_sent_cb(
    int status, pmix_proc_t* peer, pmix_data_buffer_t* buf, prte_rml_tag_t tag,
    void *cbdata
) {
    // We don't care about the status, update to sent and we'll repair during
    // the link update steps if someone died
    PRTE_HIDE_UNUSED_PARAMS(status, peer, tag);
    PMIx_Data_buffer_release(buf);
    prte_relm_msg_t* msg = (prte_relm_msg_t*) cbdata;
    PRTE_RELM_MSG_OUTPUT_TRACE(3, msg);
    prte_relm_update_state(msg, PRTE_RELM_STATE_SENT);
    PMIX_RELEASE(msg); // msg was retained for this callback
}

void prte_relm_send_state_downstream(prte_relm_msg_t* msg){
    PRTE_RELM_MSG_OUTPUT_TRACE(2, msg);
    bool valid_state =
        PRTE_RELM_STATE_SENDING == msg->state ||
        PRTE_RELM_STATE_ACKACKED == msg->state;
    if(!valid_state) PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);

    pmix_rank_t hop = prte_relm_sm->downstream_rank(msg);
    bool valid_dst = PRTE_PROC_MY_NAME->rank != hop;
    if(!valid_dst) PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);

    if(!(valid_state && valid_dst)){
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    pmix_data_buffer_t* buf = PMIx_Data_buffer_create();
    int ret = prte_relm_sm->pack_state_update(buf, msg);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIx_Data_buffer_release(buf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    // Send manually to update the callback so we know when the send is done
    prte_rml_send_t *send = PMIX_NEW(prte_rml_send_t);
    PMIX_LOAD_PROCID(&send->dst, PRTE_PROC_MY_NAME->nspace, hop);
    send->origin = *PRTE_PROC_MY_NAME;
    send->tag = PRTE_RML_TAG_RELM_STATE;
    send->dbuf = buf;
    if(PRTE_RELM_STATE_SENDING == msg->state){
        send->cbfunc = sending_to_sent_cb;
        send->cbdata = msg;
        PMIX_RETAIN(msg);
    }
    PRTE_OOB_SEND(send);
}

void prte_relm_send_state_upstream(prte_relm_msg_t* msg){
    PRTE_RELM_MSG_OUTPUT_TRACE(2, msg);
    bool valid_state =
        PRTE_RELM_STATE_ACKED == msg->state ||
        PRTE_RELM_STATE_REQUESTED == msg->state;
    if(!valid_state) PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);

    pmix_rank_t hop = prte_relm_sm->upstream_rank(msg);
    bool valid_dst = PRTE_PROC_MY_NAME->rank != hop;
    if(!valid_dst) PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);

    if(!(valid_state && valid_dst)){
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    pmix_data_buffer_t* buf = PMIx_Data_buffer_create();
    int ret = prte_relm_sm->pack_state_update(buf, msg);
    if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    if(PMIX_SUCCESS == ret) {
        ret = prte_rml_send_buffer_nb(hop, buf, PRTE_RML_TAG_RELM_STATE);
        if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    }
    if(PMIX_SUCCESS != ret){
        PMIx_Data_buffer_release(buf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
    }
}

void prte_relm_send_link_update(pmix_rank_t link){
    PRTE_RELM_OUTPUT_VERBOSE(1, "sending link update to %d", link);
    if(link >= prte_rml_base.n_dmns){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    pmix_data_buffer_t* buf = PMIx_Data_buffer_create();
    int ret = PMIX_SUCCESS;
    if(PMIX_SUCCESS == ret){
        ret = prte_relm_sm->pack_link_update(buf, link);
        if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    }
    if(PMIX_SUCCESS == ret){
        ret = prte_rml_send_buffer_nb(link, buf, PRTE_RML_TAG_RELM_LINK);
        if(PMIX_SUCCESS != ret) PMIX_ERROR_LOG(ret);
    }
    if(PMIX_SUCCESS != ret){
        PMIx_Data_buffer_release(buf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
    }
}

void prte_relm_message_handler(pmix_rank_t src, pmix_data_buffer_t* buf){
    prte_relm_signature_t sig = prte_relm_unpack_signature(buf);
    // Some error happened, unpack fn reported and set job state for us
    if(!PMIx_Rank_valid(sig.src)) return;

    prte_relm_msg_t* msg = prte_relm_get_msg(&sig);
    PRTE_RELM_MSG_OUTPUT_TRACE(4, msg);
    if(!msg){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    msg->prev_uid = prte_relm_unpack_uid(buf);
    if(msg->prev_uid == PRTE_RELM_UID_INVALID){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    } else if(msg->prev_uid <= PRTE_RELM_UID_MAX){
        prte_relm_get_prev_msg(msg)->next_uid = msg->uid;
    }

    prte_relm_state_t state = prte_relm_unpack_state(buf);
    if(PRTE_RELM_STATE_INVALID == state){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    prte_relm_sm->update_state(buf, msg, state, src);
}

void prte_relm_link_update_handler(pmix_rank_t src, pmix_data_buffer_t* buf){
    PRTE_RELM_OUTPUT_VERBOSE(1, "received link update from %d", src);
    prte_relm_sm->update_link(buf, src);
}

static void sm_cons(prte_relm_state_machine_t* sm){
    sm->new_rank = NULL;
    sm->pack_link_update = NULL;
    sm->update_link = NULL;
    sm->new_msg = NULL;
    sm->pack_state_update = NULL;
    sm->update_state = NULL;
    sm->upstream_rank = NULL;
    sm->downstream_rank = NULL;
    sm->fault_handler = NULL;

    PMIX_CONSTRUCT(&sm->ranks, pmix_hash_table_t);
    pmix_hash_table_init(&sm->ranks, 20);

    PMIX_CONSTRUCT(&sm->cached_messages, pmix_list_t);

    sm->max_cache_count = prte_relm_base.cache_max_count;

    sm->cache_tv = (struct timeval) {0};
    if(prte_relm_base.cache_ms > 0){
        sm->cache_tv.tv_sec = prte_relm_base.cache_ms / 1000;
        sm->cache_tv.tv_usec =
            (prte_relm_base.cache_ms - sm->cache_tv.tv_sec*1000)*1000;
    }

    sm->next_uid = 0;

    PMIX_CONSTRUCT(&sm->upstream_links_updated, pmix_bitmap_t);
    pmix_bitmap_init(&sm->upstream_links_updated, prte_rml_base.radix+1);
    pmix_bitmap_set_all_bits(&sm->upstream_links_updated);

    PMIX_CONSTRUCT(&sm->downstream_links_updated, pmix_bitmap_t);
    pmix_bitmap_init(&sm->downstream_links_updated, prte_rml_base.radix+1);
    pmix_bitmap_set_all_bits(&sm->downstream_links_updated);
}
static void sm_dest(prte_relm_state_machine_t* sm){
    pmix_rank_t key;
    prte_relm_rank_t* val;
    PMIX_HASH_TABLE_FOREACH(key, uint32, val, &sm->ranks){
        PMIX_DESTRUCT(val);
    }
    PMIX_DESTRUCT(&sm->ranks);
    PMIX_DESTRUCT(&sm->cached_messages);
    PMIX_DESTRUCT(&sm->upstream_links_updated);
    PMIX_DESTRUCT(&sm->downstream_links_updated);
}
PMIX_CLASS_INSTANCE(prte_relm_state_machine_t, pmix_object_t, sm_cons, sm_dest);
