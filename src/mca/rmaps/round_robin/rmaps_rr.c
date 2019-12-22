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
 * Copyright (c) 2006-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/util/error_strings.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
#include "rmaps_rr.h"

/*
 * Create a round-robin mapping for the job.
 */
static int prrte_rmaps_rr_map(prrte_job_t *jdata)
{
    prrte_app_context_t *app;
    int i;
    prrte_list_t node_list;
    prrte_list_item_t *item;
    prrte_std_cntr_t num_slots;
    int rc;
    prrte_mca_base_component_t *c = &prrte_rmaps_round_robin_component.base_version;
    bool initial_map=true;

    /* this mapper can only handle initial launch
     * when rr mapping is desired - allow
     * restarting of failed apps
     */
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s is being restarted - rr cannot map",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper &&
        0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s not using rr mapper",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRRTE_MAPPING_RR < PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* I don't know how to do these - defer */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s not using rr mapper",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: mapping job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* start at the beginning... */
    jdata->num_procs = 0;

    /* cycle through the app_contexts, mapping them sequentially */
    for(i=0; i < jdata->apps->size; i++) {
        hwloc_obj_type_t target;
        unsigned cache_level;
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }

        /* setup the nodelist here in case we jump to error */
        PRRTE_CONSTRUCT(&node_list, prrte_list_t);

        /* if the number of processes wasn't specified, then we know there can be only
         * one app_context allowed in the launch, and that we are to launch it across
         * all available slots. We'll double-check the single app_context rule first
         */
        if (0 == app->num_procs && 1 < jdata->num_apps) {
            prrte_show_help("help-prrte-rmaps-rr.txt", "prrte-rmaps-rr:multi-apps-and-zero-np",
                           true, jdata->num_apps, NULL);
            rc = PRRTE_ERR_SILENT;
            goto error;
        }

        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        if(PRRTE_SUCCESS != (rc = prrte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                                  jdata->map->mapping, initial_map, false))) {
            PRRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* if a bookmark exists from some prior mapping, set us to start there */
        jdata->bookmark = prrte_rmaps_base_get_starting_point(&node_list, jdata);

        if (0 == app->num_procs) {
            /* set the num_procs to equal the number of slots on these
             * mapped nodes, taking into account the number of cpus/rank
             */
            app->num_procs = num_slots;
            /* sometimes, we have only one "slot" assigned, but may
             * want more than one cpu/rank - so ensure we always wind
             * up with at least one proc */
            if (0 == app->num_procs) {
                app->num_procs = 1;
            }
        }

        /* Make assignments */
        if (PRRTE_MAPPING_BYNODE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            rc = prrte_rmaps_rr_bynode(jdata, app, &node_list, num_slots,
                                      app->num_procs);
        } else if (PRRTE_MAPPING_BYSLOT == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                      app->num_procs);
        } else if (PRRTE_MAPPING_BYHWTHREAD == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots,
                                     app->num_procs, HWLOC_OBJ_PU, 0);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else if (PRRTE_MAPPING_BYCORE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots,
                                     app->num_procs, HWLOC_OBJ_CORE, 0);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else if (PRRTE_MAPPING_BYL1CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            PRRTE_HWLOC_MAKE_OBJ_CACHE(1, target, cache_level);
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots, app->num_procs,
                                     target, cache_level);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else if (PRRTE_MAPPING_BYL2CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            PRRTE_HWLOC_MAKE_OBJ_CACHE(2, target, cache_level);
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots, app->num_procs,
                                     target, cache_level);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else if (PRRTE_MAPPING_BYL3CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            PRRTE_HWLOC_MAKE_OBJ_CACHE(3, target, cache_level);
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots, app->num_procs,
                                     target, cache_level);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else if (PRRTE_MAPPING_BYSOCKET == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots,
                                     app->num_procs, HWLOC_OBJ_SOCKET, 0);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else if (PRRTE_MAPPING_BYNUMA == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            rc = prrte_rmaps_rr_byobj(jdata, app, &node_list, num_slots,
                                     app->num_procs, HWLOC_OBJ_NODE, 0);
            if (PRRTE_ERR_NOT_FOUND == rc) {
                /* if the mapper couldn't map by this object because
                 * it isn't available, but the error allows us to try
                 * byslot, then do so
                 */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                rc = prrte_rmaps_rr_byslot(jdata, app, &node_list, num_slots,
                                          app->num_procs);
            }
        } else {
            /* unrecognized mapping directive */
            prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy",
                           true, "mapping",
                           prrte_rmaps_base_print_mapping(jdata->map->mapping));
            rc = PRRTE_ERR_SILENT;
            goto error;
        }
        if (PRRTE_SUCCESS != rc) {
            PRRTE_ERROR_LOG(rc);
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
        while (NULL != (item = prrte_list_remove_first(&node_list))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&node_list);
    }

    return PRRTE_SUCCESS;

 error:
    while(NULL != (item = prrte_list_remove_first(&node_list))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&node_list);

    return rc;
}

static int prrte_rmaps_rr_assign_locations(prrte_job_t *jdata)
{
    prrte_mca_base_component_t *c = &prrte_rmaps_round_robin_component.base_version;
    hwloc_obj_type_t target;
    unsigned cache_level;
    int rc;

    if (NULL == jdata->map->last_mapper ||
        0 != strcasecmp(jdata->map->last_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr: job %s not using rr mapper",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: assign locations for job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* if the mapping directive was byslot or bynode, then we
     * assign locations to the root object level */
    if (PRRTE_MAPPING_BYNODE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping) ||
        PRRTE_MAPPING_BYSLOT == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        return prrte_rmaps_rr_assign_root_level(jdata);
    }

    /* otherwise, assign by object */
    if (PRRTE_MAPPING_BYHWTHREAD == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        rc = prrte_rmaps_rr_assign_byobj(jdata, HWLOC_OBJ_PU, 0);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't assign by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else if (PRRTE_MAPPING_BYCORE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        rc = prrte_rmaps_rr_assign_byobj(jdata, HWLOC_OBJ_CORE, 0);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't map by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else if (PRRTE_MAPPING_BYL1CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        PRRTE_HWLOC_MAKE_OBJ_CACHE(1, target, cache_level);
        rc = prrte_rmaps_rr_assign_byobj(jdata, target, cache_level);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't map by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else if (PRRTE_MAPPING_BYL2CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        PRRTE_HWLOC_MAKE_OBJ_CACHE(2, target, cache_level);
        rc = prrte_rmaps_rr_assign_byobj(jdata, target, cache_level);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't map by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else if (PRRTE_MAPPING_BYL3CACHE == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        PRRTE_HWLOC_MAKE_OBJ_CACHE(3, target, cache_level);
        rc = prrte_rmaps_rr_assign_byobj(jdata, target, cache_level);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't map by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else if (PRRTE_MAPPING_BYSOCKET == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        rc = prrte_rmaps_rr_assign_byobj(jdata, HWLOC_OBJ_SOCKET, 0);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't map by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else if (PRRTE_MAPPING_BYNUMA == PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        rc = prrte_rmaps_rr_assign_byobj(jdata, HWLOC_OBJ_NODE, 0);
        if (PRRTE_ERR_NOT_FOUND == rc) {
            /* if the mapper couldn't map by this object because
             * it isn't available, but the error allows us to try
             * byslot, then do so
             */
            PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
            rc = prrte_rmaps_rr_assign_root_level(jdata);
        }
    } else {
        /* unrecognized mapping directive */
        prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy",
                       true, "mapping",
                       prrte_rmaps_base_print_mapping(jdata->map->mapping));
        rc = PRRTE_ERR_SILENT;
    }
    return rc;
}

prrte_rmaps_base_module_t prrte_rmaps_round_robin_module = {
    .map_job = prrte_rmaps_rr_map,
    .assign_locations = prrte_rmaps_rr_assign_locations
};
