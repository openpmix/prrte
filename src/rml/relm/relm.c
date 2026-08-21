/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

#include "src/rml/rml.h"
#include "src/rml/relm/relm.h"
#include "src/rml/relm/state_machine.h"

prte_relm_base_t prte_relm_base = {
    .output = -1,
    .verbosity = 0,
    .cache_ms = 500,
    .cache_max_count = 30,
};

static void recv_msg(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status, cbdata);
    if(PRTE_RML_TAG_RELM_STATE == tag){
        prte_relm_message_handler(sender->rank, buf);
    } else {
        prte_relm_link_update_handler(sender->rank, buf);
    }
}

void prte_relm_register(void){
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

void prte_relm_open(void){
    if(0 < prte_relm_base.verbosity) {
        prte_relm_base.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(
            prte_relm_base.output, prte_relm_base.verbosity
        );
    }

    prte_relm_sm = PMIX_NEW(prte_relm_state_machine_t);

    PRTE_RML_RECV(
        PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_STATE, PRTE_RML_PERSISTENT,
        recv_msg, NULL
    );
    PRTE_RML_RECV(
        PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_LINK, PRTE_RML_PERSISTENT,
        recv_msg, NULL
    );
}

void prte_relm_close(void){
    /* prte_rml_open() can fail before it ever reaches prte_relm_open(), and
     * prte_rml_close() runs regardless - so there may be nothing to tear
     * down */
    if(NULL == prte_relm_sm) return;

    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_STATE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RELM_LINK);

    /* the state machine goes before the output channel it reports through:
     * releasing it evicts every cached message, and eviction is one of the
     * transitions that traces */
    PMIX_RELEASE(prte_relm_sm);
    prte_relm_sm = NULL;

    if(0 <= prte_relm_base.output){
        pmix_output_close(prte_relm_base.output);
        prte_relm_base.output = -1;
    }
}
