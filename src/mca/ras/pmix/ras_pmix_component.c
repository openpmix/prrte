/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_base.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/ras/base/base.h"
#include "ras_pmix.h"

/*
 * Local functions
 */
static int ras_pmix_component_open(void);
static int ras_pmix_register(void);
static int ras_pmix_component_query(pmix_mca_base_module_t **module, int *priority);

prte_ras_pmix_component_t prte_mca_ras_pmix_component = {
    .super = {
        PRTE_RAS_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "pmix",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),
        .pmix_mca_open_component = ras_pmix_component_open,
        .pmix_mca_query_component = ras_pmix_component_query,
        .pmix_mca_register_component_params = ras_pmix_register
    },
    .connect_to_system_scheduler = false
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, ras, pmix)

static char *nspace = NULL;

static int ras_pmix_register(void)
{
    pmix_mca_base_component_t *component = &prte_mca_ras_pmix_component.super;

    prte_mca_ras_pmix_component.uri = NULL;
    (void) pmix_mca_base_component_var_register(component, "uri",
                                                "Specify the URI of the scheduler to which we are to connect, "
                                                "or the name of the file (specified as file:filename) that "
                                                "contains that info",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_ras_pmix_component.uri);

    nspace = NULL;
    (void) pmix_mca_base_component_var_register(component, "nspace",
                                                "Specify the namespace of the scheduler to which we are to connect",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &nspace);
    if (NULL != nspace) {
        PMIx_Load_nspace(prte_mca_ras_pmix_component.server.nspace, nspace);
    }

    prte_mca_ras_pmix_component.server.rank = PMIX_RANK_INVALID;
    (void) pmix_mca_base_component_var_register(component, "rank",
                                                "Specify the rank of the scheduler to which we are to connect",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_ras_pmix_component.server.rank);

    prte_mca_ras_pmix_component.connect_to_system_scheduler = false;
    (void) pmix_mca_base_component_var_register(component, "system_scheduler",
                                                "Connect to system scheduler, if available",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_ras_pmix_component.connect_to_system_scheduler);

    prte_mca_ras_pmix_component.connection_order = NULL;
    (void) pmix_mca_base_component_var_register(component, "connection_order",
                                                "Comma-delimited list of attributes defining the order in which "
                                                "connections should be attempted, from first to last.",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_ras_pmix_component.connection_order);

    prte_mca_ras_pmix_component.server_pid = 0;
    (void) pmix_mca_base_component_var_register(component, "server_pid",
                                                "Specify the pid of the scheduler to which we are to connect",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_ras_pmix_component.server_pid);

    prte_mca_ras_pmix_component.server_host = NULL;
    (void) pmix_mca_base_component_var_register(component, "server_host",
                                                "Host where target scheduler can be found",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_ras_pmix_component.server_host);

    prte_mca_ras_pmix_component.max_retries = 5;
    (void) pmix_mca_base_component_var_register(component, "max_retries",
                                                "Max number of retries for connection (default: 5)",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_ras_pmix_component.max_retries);


    prte_mca_ras_pmix_component.retry_delay = 1;
    (void) pmix_mca_base_component_var_register(component, "retry_delay",
                                                "Time in seconds between connection attempts (default: 1)",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_ras_pmix_component.retry_delay);

    return PRTE_SUCCESS;
}

static int ras_pmix_component_open(void)
{
    // initialize the globals
    prte_mca_ras_pmix_component.connect_to_system_scheduler = false;
    PMIx_Load_procid(&prte_mca_ras_pmix_component.server, NULL, PMIX_RANK_INVALID);
    prte_mca_ras_pmix_component.uri = NULL;
    prte_mca_ras_pmix_component.connection_order = NULL;
    prte_mca_ras_pmix_component.server_pid = 0;
    prte_mca_ras_pmix_component.server_host = NULL;
    prte_mca_ras_pmix_component.max_retries = 5;
    prte_mca_ras_pmix_component.retry_delay = 1;
    return PRTE_SUCCESS;
}

static int ras_pmix_component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* always make ourselves available in case the system includes a
     * scheduler that supports PMIx operations
     */
    *module = (pmix_mca_base_module_t *) &prte_ras_pmix_module;
    *priority = 20;
    return PRTE_SUCCESS;
}
