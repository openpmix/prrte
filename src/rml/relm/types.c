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

#include "src/rml/rml.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/util.h"
#include "src/rml/relm/types.h"

static void msg_cons(prte_relm_msg_t* msg){
    msg->src = PMIX_RANK_INVALID;
    msg->dst = PMIX_RANK_INVALID;
    msg->uid = PRTE_RELM_UID_INVALID;
    msg->prev_uid = PRTE_RELM_UID_INVALID;
    msg->next_uid = PRTE_RELM_UID_NONE;
    msg->state = PRTE_RELM_STATE_INVALID;
    msg->data = (pmix_byte_object_t) PMIX_BYTE_OBJECT_STATIC_INIT;
    msg->cached = false;
}
static void msg_des(prte_relm_msg_t* msg){
    if(msg->cached){
        prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);
    }
    PMIx_Byte_object_destruct(&msg->data);
}
PMIX_CLASS_INSTANCE(prte_relm_msg_t, pmix_list_item_t, msg_cons, msg_des);

static void rank_cons(prte_relm_rank_t* rank){
    PMIX_CONSTRUCT(&rank->msgs, pmix_hash_table_t);
    pmix_hash_table_init(&rank->msgs, 20);
    rank->my_last_msg = PRTE_RELM_UID_NONE;
}
static void rank_des(prte_relm_rank_t* rank){
    prte_relm_guid_t guid;
    prte_relm_msg_t* msg;
    PMIX_HASH_TABLE_FOREACH(guid, uint64, msg, &rank->msgs){
        PMIX_DESTRUCT(msg);
    }
    PMIX_DESTRUCT(&rank->msgs);
}
PMIX_CLASS_INSTANCE(prte_relm_rank_t, pmix_object_t, rank_cons, rank_des);
