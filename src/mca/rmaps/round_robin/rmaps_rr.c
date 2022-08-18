/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
#include <string.h>

#include "src/mca/errmgr/errmgr.h"
#include "src/util/error_strings.h"
#include "src/util/pmix_show_help.h"

#include "rmaps_rr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

/*
 * Create a round-robin mapping for the job.
 */
static int prte_rmaps_rr_map(prte_job_t *jdata,
                             prte_rmaps_options_t *options)
{
    prte_app_context_t *app;
    int i;
    pmix_list_t node_list;
    pmix_list_item_t *item;
    int32_t num_slots;
    int rc;
    prte_mca_base_component_t *c = &prte_rmaps_round_robin_component.base_version;
    bool initial_map = true;
    char **tmp;

    /* this mapper can only handle initial launch
     * when rr mapping is desired - allow
     * restarting of failed apps
     */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s is being restarted - rr cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper
        && 0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s not using rr mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRTE_MAPPING_RR < PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* I don't know how to do these - defer */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s not using rr mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* start at the beginning... */
    jdata->num_procs = 0;

    /* cycle through the app_contexts, mapping them sequentially */
    for (i = 0; i < jdata->apps->size; i++) {
        hwloc_obj_type_t target;
        unsigned cache_level;
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }

        /* setup the nodelist here in case we jump to error */
        PMIX_CONSTRUCT(&node_list, pmix_list_t);

        /* if the number of processes wasn't specified, then we know there can be only
         * one app_context allowed in the launch, and that we are to launch it across
         * all available slots. We'll double-check the single app_context rule first
         */
        if (0 == app->num_procs && 1 < jdata->num_apps) {
            pmix_show_help("help-prte-rmaps-rr.txt", "prte-rmaps-rr:multi-apps-and-zero-np", true,
                           jdata->num_apps, NULL);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

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

        if (0 == app->num_procs) {
            if (NULL != options->cpuset && !options->overload) {
                tmp = pmix_argv_split(options->cpuset, ',');
                app->num_procs = pmix_argv_count(tmp);
                pmix_argv_free(tmp);
            } else {
                /* set the num_procs to equal the number of slots on these
                 * mapped nodes, taking into account the number of cpus/rank
                 */
                app->num_procs = num_slots / options->cpus_per_rank;
                /* sometimes, we have only one "slot" assigned, but may
                 * want more than one cpu/rank - so ensure we always wind
                 * up with at least one proc */
                if (0 == app->num_procs) {
                    app->num_procs = 1;
                }
            }
        }

        /* Make assignments */
        if (PRTE_MAPPING_BYNODE == options->map) {
            rc = prte_rmaps_rr_bynode(jdata, app, &node_list,
                                      num_slots, app->num_procs,
                                      options);
        } else if (PRTE_MAPPING_BYSLOT == options->map) {
            rc = prte_rmaps_rr_byslot(jdata, app, &node_list,
                                      num_slots, app->num_procs,
                                      options);
        } else if (PRTE_MAPPING_PELIST == options->map) {
            rc = prte_rmaps_rr_bycpu(jdata, app, &node_list,
                                     num_slots, app->num_procs,
                                     options);
        } else {
            rc = prte_rmaps_rr_byobj(jdata, app, &node_list,
                                     num_slots, app->num_procs,
                                     options);
            if (PRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                options->map = PRTE_MAPPING_BYSLOT;
                rc = prte_rmaps_rr_byslot(jdata, app, &node_list,
                                          num_slots, app->num_procs,
                                          options);
            }
        }
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }

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

prte_rmaps_base_module_t prte_rmaps_round_robin_module = {
    .map_job = prte_rmaps_rr_map
};
