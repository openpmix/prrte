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
 * RELM base state machine functions
 */

#ifndef PRTE_RELM_BASE_STATE_MACHINE_H
#define PRTE_RELM_BASE_STATE_MACHINE_H

#include "src/pmix/pmix-internal.h"
#include "src/rml/relm/types.h"

int prte_relm_base_pack_state_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg
);
void prte_relm_base_update_state(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state,
    pmix_rank_t src
);


int prte_relm_base_pack_link_update(
    pmix_data_buffer_t* buf, pmix_rank_t link
);
void prte_relm_base_update_link(
    pmix_data_buffer_t* buf, pmix_rank_t link
);
void prte_relm_base_fault_handler(const prte_rml_recovery_status_t* status);


static inline prte_relm_rank_t* prte_relm_base_new_rank(void){
    return PMIX_NEW(prte_relm_rank_t);
}
static inline prte_relm_msg_t* prte_relm_base_new_msg(void){
    return PMIX_NEW(prte_relm_msg_t);
}

static inline pmix_rank_t prte_relm_base_upstream_rank(prte_relm_msg_t* msg){
    return prte_rml_get_route(msg->src);
}
static inline pmix_rank_t prte_relm_base_downstream_rank(prte_relm_msg_t* msg){
    return prte_rml_get_route(msg->dst);
}


END_C_DECLS

#endif
