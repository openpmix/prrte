/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/pmix_list.h"

#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "ras_hosts.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);
static pmix_status_t modify(prte_pmix_server_req_t *req);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_hosts_module = {
    .init = NULL,
    .allocate = allocate,
    .modify = modify,
    .finalize = finalize
};

static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    int rc, i, j;
    char *hosts, **hostlist = NULL;
    bool check;
    prte_app_context_t *app;

    /* We first see if we were given a rank/seqfile - if so, use it
     * as the hosts will be taken from the mapping */
    hosts = NULL;
    check = prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING);
    if (check && NULL != hosts) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:hosts:allocate parsing rank/seqfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

        /* a rank/seqfile was provided - parse it */
        rc = prte_util_add_hostfile_nodes(nodes, hosts);
        if (PRTE_SUCCESS != rc) {
            free(hosts);
            return rc;
        }
        free(hosts);
    }

    /* if something was found in the rankfile, we add those resources to
     * our global pool, use that as our global
     * pool - set it and we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        rc = prte_ras_base_node_insert(nodes, jdata);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* Record that the rankfile mapping policy has been selected */
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
        }
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_GIVEN);
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYUSER);
        /* rankfile is considered equivalent to an RM allocation */
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        }
        return PRTE_SUCCESS;
    }

    /* if a dash-host has been provided, aggregate across all the
     * app_contexts. Any hosts the user wants to add via comm_spawn
     * can be done so using the add_host option */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL ==  app) {
            continue;
        }
        hosts = NULL;
        check = prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts, PMIX_STRING);
        if (check && NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding dash_hosts",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            rc = prte_util_add_dash_host_nodes(nodes, hosts, true);
            if (PRTE_SUCCESS != rc) {
                free(hosts);
                return rc;
            }
            free(hosts);
        }
    }

    /* if something was found in the dash-host(s), we use that as our global
     * pool - set it and we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        }
        return rc;
    }

    /* Our next option is to look for a hostfile and assign our global
     * pool from there.
     *
     * Individual hostfile names, if given, are included
     * in the app_contexts for this job. We therefore need to
     * retrieve the app_contexts for the job, and then cycle
     * through them to see if anything is there. The parser will
     * add the nodes found in each hostfile to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * in hosthosts from across all app_contexts
     *
     * Note that any relative node syntax found in the hosthosts will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding hostfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            hostlist = PMIx_Argv_split(hosts, ',');
            free(hosts);
            for (j=0; NULL != hostlist[j]; j++) {
                if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(nodes, hostlist[j]))) {
                    PMIx_Argv_free(hostlist);
                    /* set an error event */
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
                    return rc;
                }
            }
            PMIx_Argv_free(hostlist);
        }
    }

    /* if something was found in the hosthosts(s), we use that as our global
     * pool - set it and we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        }
        return rc;
    }

    /* if nothing was found so far, then look for a default hostfile */
    if (NULL != prte_default_hostfile) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing default hostfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_default_hostfile));

        /* a default hostfile was provided - parse it */
        if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(nodes, prte_default_hostfile))) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            return rc;
        }
    }

    /* if something was found in the default hostfile, we use that as our global
     * pool - set it and we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        }
        return rc;
    }

    PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:hosts:allocate nothing found in hosts",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}

static pmix_status_t modify(prte_pmix_server_req_t *req)
{
    req->status = PMIX_ERR_NOT_SUPPORTED;
    return PMIX_ERR_TAKE_NEXT_OPTION;
}
