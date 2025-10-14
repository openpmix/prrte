/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
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

#include "src/rml/relm/relm.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/base/base.h"
#include "src/rml/relm/base/state_machine.h"

static void init(void);
static void finalize(void);
static void fault_handler(const prte_rml_recovery_status_t* status);
static void recv_msg(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
);

prte_relm_module_t prte_relm_base_module = {
    .init = init,
    .finalize = finalize,
    .fault_handler = fault_handler,
    .reliable_send = prte_relm_start_msg,
};

prte_relm_base_t prte_relm_base = {
    .output = -1,
    .verbosity = 0,
    .cache_ms = 500,
    .cache_max_count = 30,
};

void prte_relm_base_register(void){
    prte_relm_base.verbosity = 0;
    pmix_mca_base_var_register(
        "prte", "relm", "base", "verbose",
        "Debug verbosity of the RELM subsytem",
        PMIX_MCA_BASE_VAR_TYPE_INT, &prte_relm_base.verbosity
    );

    prte_relm_base.cache_ms = 500;
    pmix_mca_base_var_register(
        "prte", "relm", "base", "cache_ms",
        "Max time to cache a reliable message, in milliseconds",
        PMIX_MCA_BASE_VAR_TYPE_INT, &prte_relm_base.cache_ms
    );

    prte_relm_base.cache_max_count = 30;
    pmix_mca_base_var_register(
        "prte", "relm", "base", "cache_max_count",
        "Max number of reliable message to cache at once",
        PMIX_MCA_BASE_VAR_TYPE_INT, &prte_relm_base.cache_max_count
    );
}

static void init(void){
    if(0 < prte_relm_base.verbosity) {
        prte_relm_base.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(
            prte_relm_base.output, prte_relm_base.verbosity
        );
    }

    prte_relm_sm = PMIX_NEW(prte_relm_state_machine_t);
    prte_relm_sm->new_rank = prte_relm_base_new_rank;
    prte_relm_sm->pack_link_update = prte_relm_base_pack_link_update;
    prte_relm_sm->update_link = prte_relm_base_update_link;
    prte_relm_sm->new_msg = prte_relm_base_new_msg;
    prte_relm_sm->pack_state_update = prte_relm_base_pack_state_update;
    prte_relm_sm->update_state = prte_relm_base_update_state;
    prte_relm_sm->upstream_rank = prte_relm_base_upstream_rank;
    prte_relm_sm->downstream_rank = prte_relm_base_downstream_rank;
    prte_relm_sm->fault_handler = prte_relm_base_fault_handler;

    PRTE_RML_RECV(
        PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_STATE, PRTE_RML_PERSISTENT,
        recv_msg, NULL
    );
    PRTE_RML_RECV(
        PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_LINK, PRTE_RML_PERSISTENT,
        recv_msg, NULL
    );
}

static void finalize(void){
    if(0 <= prte_relm_base.output){
        pmix_output_close(prte_relm_base.output);
    }

    PMIX_RELEASE(prte_relm_sm);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_STATE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_LINK);
}

static void fault_handler(const prte_rml_recovery_status_t* status){
    prte_relm_sm->fault_handler(status);
}

static void recv_msg(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
){
    PRTE_HIDE_UNUSED_PARAMS(status, cbdata);
    if(PRTE_RML_TAG_RELM_STATE == tag){
        prte_relm_message_handler(sender->rank, buf);
    } else {
        prte_relm_link_update_handler(sender->rank, buf);
    }
}
