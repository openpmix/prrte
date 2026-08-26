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

#include <string.h>

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
        PRTE_MCA_BASE_VERSION(ras),

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

/* MCA variables bind to the storage they are given and the base writes an
 * int through it, so a parameter whose home is not an int needs a proxy of
 * the right type here and a copy after registration.  ras_pmix_rank lands in
 * a pmix_rank_t, ras_pmix_server_pid in a pid_t and the retry pair in
 * uint32_t; handing the base those addresses had it write four bytes of int
 * through a pointer of another type, which the platforms PRRTE builds on
 * happen to survive and the language does not promise. */
static char *nspace = NULL;
static int rank = (int) PMIX_RANK_INVALID;
static int server_pid = 0;
static int max_retries = 5;
static int retry_delay = 1;

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
        /* PMIx_Load_nspace truncates at PMIX_MAX_NSLEN without saying so, and
         * a truncated name is a different scheduler - most likely one that
         * does not exist.  A name that does not fit a pmix_nspace_t cannot be
         * any PMIx namespace, so say so and leave the parameter unapplied
         * rather than attaching to whatever the prefix happens to match.  An
         * unset nspace does not count towards the query gate below, so with
         * nothing else configured the component declines and the ordinary
         * allocator runs. */
        if (PMIX_MAX_NSLEN < strlen(nspace)) {
            pmix_output(0, "ras/pmix: ignoring ras_pmix_nspace \"%s\" - a PMIx "
                           "namespace is at most %d characters", nspace,
                        (int) PMIX_MAX_NSLEN);
        } else {
            PMIx_Load_nspace(prte_mca_ras_pmix_component.server.nspace, nspace);
        }
    }

    /* the cast is not decoration: PMIX_RANK_INVALID is UINT32_MAX, and
     * initializing an int with it is a narrowing conversion some compilers
     * refuse under the warnings-as-errors build */
    rank = (int) PMIX_RANK_INVALID;
    (void) pmix_mca_base_component_var_register(component, "rank",
                                                "Specify the rank of the scheduler to which we are to connect",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &rank);
    prte_mca_ras_pmix_component.server.rank = (pmix_rank_t) rank;

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

    server_pid = 0;
    (void) pmix_mca_base_component_var_register(component, "server_pid",
                                                "Specify the pid of the scheduler to which we are to connect",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &server_pid);
    prte_mca_ras_pmix_component.server_pid = (pid_t) server_pid;

    prte_mca_ras_pmix_component.server_host = NULL;
    (void) pmix_mca_base_component_var_register(component, "server_host",
                                                "Host where target scheduler can be found",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_ras_pmix_component.server_host);

    max_retries = 5;
    (void) pmix_mca_base_component_var_register(component, "max_retries",
                                                "Max number of retries for connection (default: 5)",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &max_retries);
    /* a negative count is not "retry forever" to PMIx, it is four billion
     * attempts once it lands in the uint32_t the attribute is defined on */
    prte_mca_ras_pmix_component.max_retries = (0 > max_retries) ? 0
                                                                : (uint32_t) max_retries;

    retry_delay = 1;
    (void) pmix_mca_base_component_var_register(component, "retry_delay",
                                                "Time in seconds between connection attempts (default: 1)",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &retry_delay);
    prte_mca_ras_pmix_component.retry_delay = (0 > retry_delay) ? 0
                                                               : (uint32_t) retry_delay;

    return PRTE_SUCCESS;
}

static int ras_pmix_component_open(void)
{
    /* Deliberately empty.  This used to re-initialize every field below,
     * which are precisely the storage locations ras_pmix_register() binds to
     * MCA variables - and a framework registers before it opens
     * (pmix_mca_base_framework_open calls _register first), so those
     * assignments ran AFTER the user's values had been read into them.  Every
     * ras_pmix_* parameter was therefore discarded: a scheduler URI, an
     * nspace, a pid, a host, the retry counts, all of them reset to the
     * defaults the registration had already applied.  Nothing here needs
     * initializing that registration does not already do; the component is a
     * static global, so anything it does not bind starts zeroed. */
    return PRTE_SUCCESS;
}

static int ras_pmix_component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* Be available only when someone has actually pointed us at a scheduler.
     *
     * This used to answer unconditionally, "in case the system includes a
     * scheduler that supports PMIx operations".  That was harmless only while
     * the framework kept every module that answered: our allocate() always
     * returns TAKE_NEXT_OPTION - this component forwards requests to a
     * scheduler, it never discovers nodes itself - so the next module down
     * did the allocating.  With one module selected there is no next module,
     * and answering here on spec would shadow ras/hosts (priority 1) in every
     * unmanaged environment, leaving nothing to read a hostfile.
     *
     * The connection parameters are the statement of intent: a URI, a
     * scheduler nspace, a pid or host to find it at, an explicit connection
     * order, or the system-scheduler switch.  With none of them set there is
     * no scheduler to forward anything to, and saying so is the honest
     * answer. */
    if (NULL == prte_mca_ras_pmix_component.uri &&
        NULL == prte_mca_ras_pmix_component.connection_order &&
        NULL == prte_mca_ras_pmix_component.server_host &&
        0 == prte_mca_ras_pmix_component.server_pid &&
        !prte_mca_ras_pmix_component.connect_to_system_scheduler &&
        0 == strlen(prte_mca_ras_pmix_component.server.nspace)) {
        *module = NULL;
        *priority = 0;
        return PRTE_ERROR;
    }

    *module = (pmix_mca_base_module_t *) &prte_ras_pmix_module;
    *priority = 20;
    return PRTE_SUCCESS;
}
