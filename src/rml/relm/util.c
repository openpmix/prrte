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
#include "src/rml/relm/types.h"
#include "src/rml/relm/util.h"

void prte_relm_post(prte_relm_msg_t* msg){
    PRTE_RELM_MSG_OUTPUT_TRACE(2, msg);
    if(PRTE_PROC_MY_NAME->rank != msg->dst){
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }

    int count = 1, ret;

    pmix_data_buffer_t* data = PMIx_Data_buffer_create();
    ret = PMIx_Data_load(data, &msg->data);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(data);
        return;
    }

    prte_rml_tag_t tag;
    ret = PMIx_Data_unpack(NULL, data, &tag, &count, PRTE_RML_TAG);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(data);
        return;
    }

    pmix_byte_object_t bytes = PMIX_BYTE_OBJECT_STATIC_INIT;
    ret = PMIx_Data_unpack(NULL, data, &bytes, &count, PMIX_BYTE_OBJECT);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(data);
        PMIx_Byte_object_destruct(&bytes);
        return;
    }
    PMIx_Data_buffer_release(data);

    pmix_proc_t src;
    PMIx_Xfer_procid(&src, PRTE_PROC_MY_NAME);
    src.rank = msg->src;

    PRTE_RML_POST_MESSAGE(&src, tag, 0, bytes.bytes, bytes.size);
}

bool prte_relm_prev_is_posted(prte_relm_msg_t* msg){
    return PRTE_RELM_UID_NONE == msg->prev_uid
        || PRTE_RELM_STATE_ACKED == prte_relm_get_prev_msg(msg)->state;
}

#define RELM_STATE_CASE(s) case PRTE_RELM_STATE_ ## s: return #s
char* prte_relm_state_name(prte_relm_state_t state){
    switch(state){
    RELM_STATE_CASE(INVALID);
    RELM_STATE_CASE(SENT);
    RELM_STATE_CASE(REQUESTED);
    RELM_STATE_CASE(SENDING);
    RELM_STATE_CASE(PENDING);
    RELM_STATE_CASE(ACKED);
    RELM_STATE_CASE(NEW);
    RELM_STATE_CASE(ACKACKED);
    RELM_STATE_CASE(CACHED);
    RELM_STATE_CASE(EVICTED);
    case PRTE_RELM_EPHEMERAL_STATES_START:
        return "EPHEMERAL_STATES_START";
    default: return "<unknown>";
    }
}

#define PRTE_RELM_SAFE_PACK(ret, buf, data, datatype)                      \
    do {                                                                   \
        if(PMIX_SUCCESS == ret){                                           \
            ret = PMIx_Data_pack(NULL, buf, data, 1, datatype);            \
            if(PMIX_SUCCESS != ret){                                       \
                PMIX_ERROR_LOG(ret);                                       \
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT); \
            }                                                              \
        }                                                                  \
    } while (false);
#define PRTE_RELM_SAFE_UNPACK(ret, buf, data, datatype)                    \
    do {                                                                   \
        if(PMIX_SUCCESS == ret){                                           \
            int _c = 1;                                                    \
            ret = PMIx_Data_unpack(NULL, buf, data, &_c, datatype);        \
            if(PMIX_SUCCESS != ret){                                       \
                PMIX_ERROR_LOG(ret);                                       \
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT); \
            }                                                              \
        }                                                                  \
    } while (false);


int prte_relm_pack_signature(pmix_data_buffer_t* buf, prte_relm_msg_t* msg){
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_PACK(ret, buf, &msg->src, PMIX_PROC_RANK);
    PRTE_RELM_SAFE_PACK(ret, buf, &msg->dst, PMIX_PROC_RANK);
    PRTE_RELM_SAFE_PACK(ret, buf, &msg->uid, PRTE_RELM_UID);
    return ret;
}
prte_relm_signature_t prte_relm_unpack_signature(pmix_data_buffer_t* buf){
    prte_relm_signature_t sig = PRTE_RELM_SIGNATURE_STATIC_INIT;
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_UNPACK(ret, buf, &sig.src, PMIX_PROC_RANK);
    PRTE_RELM_SAFE_UNPACK(ret, buf, &sig.dst, PMIX_PROC_RANK);
    PRTE_RELM_SAFE_UNPACK(ret, buf, &sig.uid, PRTE_RELM_UID);
    return sig;
}

int prte_relm_pack_state(pmix_data_buffer_t* buf, prte_relm_state_t state){
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_PACK(ret, buf, &state, PRTE_RELM_STATE);
    return ret;
}
prte_relm_state_t prte_relm_unpack_state(pmix_data_buffer_t* buf){
    prte_relm_state_t state = PRTE_RELM_STATE_INVALID;
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_UNPACK(ret, buf, &state, PRTE_RELM_STATE);
    return state;
}

int prte_relm_pack_data(pmix_data_buffer_t* buf, pmix_byte_object_t data){
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_PACK(ret, buf, &data, PMIX_BYTE_OBJECT);
    return ret;
}
pmix_byte_object_t prte_relm_unpack_data(pmix_data_buffer_t* buf){
    pmix_byte_object_t data = PMIX_BYTE_OBJECT_STATIC_INIT;
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_UNPACK(ret, buf, &data, PMIX_BYTE_OBJECT);
    return data;
}

int prte_relm_pack_uid(pmix_data_buffer_t* buf, prte_relm_uid_t uid){
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_PACK(ret, buf, &uid, PRTE_RELM_UID);
    return ret;
}
prte_relm_uid_t prte_relm_unpack_uid(pmix_data_buffer_t* buf){
    prte_relm_uid_t uid = PRTE_RELM_UID_INVALID;
    int ret = PMIX_SUCCESS;
    PRTE_RELM_SAFE_UNPACK(ret, buf, &uid, PRTE_RELM_UID);
    return uid;
}
