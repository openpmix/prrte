/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <ctype.h>
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "rmaps_likwid.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int prte_rmaps_likwid_map(prte_job_t *jdata,
                                 prte_rmaps_options_t *options);

/* define the module */
prte_rmaps_base_module_t prte_rmaps_likwid_module = {
    .map_job = prte_rmaps_likwid_map
};

static int prte_rmaps_likwid_map(prte_job_t *jdata,
                                 prte_rmaps_options_t *options)
{
    prte_app_context_t *app;
    int i;
    pmix_list_t node_list;
    int32_t num_slots;
    int rc;
    pmix_mca_base_component_t *c = &prte_mca_rmaps_likwid_component;
    bool initial_map = true;
    char **tmp;

    PMIX_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:likwid called on job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* this mapper can only handle initial launch
     * when likwid mapping is desired - allow
     * restarting of failed apps
     */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:likwid: job %s is being restarted - likwid cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper) {
        if (0 != strcasecmp(jdata->map->req_mapper, c->pmix_mca_component_name)) {
            /* a mapper has been specified, and it isn't me */
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:likwid: job %s not using likwiduential mapper",
                                PRTE_JOBID_PRINT(jdata->nspace));
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
    }
    if (PRTE_MAPPING_LIKWID != PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* I don't know how to do these - defer */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:likwid: job %s not using likwid mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:likwid: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->pmix_mca_component_name);

    /* start at the beginning... */
    jdata->num_procs = 0;

    /* cycle through the app_contexts, mapping them using likwid */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }

        /* setup the nodelist here in case we jump to error */
        PMIX_CONSTRUCT(&node_list, pmix_list_t);

        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        rc = prte_rmaps_base_get_target_nodes(&node_list, &num_slots, jdata, app,
                                              jdata->map->mapping, initial_map, false);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* Make assignments - both assigning procs to locations and specifying
         * their binding. See the rankfile mapper for how to do the binding */

        /* aborting here for now */
        rc = PRTE_ERR_NOT_SUPPORTED;
        PRTE_ERROR_LOG(rc);
        goto error;


        /* track the total number of processes we mapped - must update
         * this value AFTER we compute vpids so that computation
         * is done correctly
         */
        jdata->num_procs += app->num_procs;

        /* cleanup the node list - it can differ from one app_context
         * to another, so we have to get it every time
         */
        PMIX_LIST_DESTRUCT(&node_list);
    }
    /* calculate the ranks for this job */
    rc = prte_rmaps_base_compute_vpids(jdata, options);

    return rc;

error:
    PMIX_LIST_DESTRUCT(&node_list);

    return rc;
}
