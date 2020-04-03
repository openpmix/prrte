/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      UT-Battelle, LLC. All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/dss/dss.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"


void prrte_rmaps_base_map_job(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_node_t *node;
    int rc, i, ppx = 0;
    bool did_map, pernode = false, persocket = false;
    prrte_rmaps_base_selected_module_t *mod;
    prrte_job_t *parent;
    prrte_vpid_t nprocs;
    prrte_app_context_t *app;
    bool inherit = false;
    prrte_process_name_t name, *nptr;

    PRRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    jdata->state = PRRTE_JOB_STATE_MAP;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: mapping job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* if this is a dynamic job launch and they didn't explicitly
     * request inheritance, then don't inherit the launch directives */
    nptr = &name;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_LAUNCH_PROXY, (void**)&nptr, PRRTE_NAME)) {
        if (NULL != (parent = prrte_get_job_data_object(name.jobid)) &&
            !PRRTE_FLAG_TEST(parent, PRRTE_JOB_FLAG_TOOL)) {
            inherit = prrte_rmaps_base.inherit;
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps: dynamic job %s %s inherit launch directives - parent %s is %s",
                                PRRTE_JOBID_PRINT(jdata->jobid),
                                inherit ? "will" : "will not",
                                PRRTE_JOBID_PRINT((parent->jobid)),
                                (NULL == parent) ? "NULL" : ((PRRTE_FLAG_TEST(parent, PRRTE_JOB_FLAG_TOOL) ? "TOOL" : "NON-TOOL")));
        } else {
            inherit = true;
        }
    } else {
        /* initial launch always takes on MCA params */
        inherit = true;
    }

    if (NULL == jdata->map) {
        jdata->map = PRRTE_NEW(prrte_job_map_t);
    }

    if (inherit) {
        if (NULL == jdata->map->ppr && NULL != prrte_rmaps_base.ppr) {
            jdata->map->ppr = strdup(prrte_rmaps_base.ppr);
        }
        if (0 == jdata->map->cpus_per_rank) {
            jdata->map->cpus_per_rank = prrte_rmaps_base.cpus_per_rank;
        }
    }
    if (NULL != jdata->map->ppr) {
        /* get the procs/object */
        ppx = strtoul(jdata->map->ppr, NULL, 10);
        if (NULL != strstr(jdata->map->ppr, "node")) {
            pernode = true;
        } else if (NULL != strstr(jdata->map->ppr, "socket")) {
            persocket = true;
        }
    }

    /* compute the number of procs and check validity */
    nprocs = 0;
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL != (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            if (0 == app->num_procs) {
                prrte_list_t nodes;
                prrte_std_cntr_t slots;
                PRRTE_CONSTRUCT(&nodes, prrte_list_t);
                prrte_rmaps_base_get_target_nodes(&nodes, &slots, app, PRRTE_MAPPING_BYNODE, true, true);
                if (pernode) {
                    slots = ppx * prrte_list_get_size(&nodes);
                } else if (persocket) {
                    /* add in #sockets for each node */
                    PRRTE_LIST_FOREACH(node, &nodes, prrte_node_t) {
                        slots += ppx * prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                          HWLOC_OBJ_SOCKET, 0,
                                                                          PRRTE_HWLOC_AVAILABLE);
                    }
                }
                app->num_procs = slots;
                PRRTE_LIST_DESTRUCT(&nodes);
            }
            nprocs += app->num_procs;
        }
    }


    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: setting mapping policies for job %s nprocs %d",
                        PRRTE_JOBID_PRINT(jdata->jobid), (int)nprocs);

    if (inherit && !jdata->map->display_map) {
        jdata->map->display_map = prrte_rmaps_base.display_map;
    }

    /* set the default mapping policy IFF it wasn't provided */
    if (!PRRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
        if (inherit && (PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps mapping given by MCA param");
            jdata->map->mapping = prrte_rmaps_base.mapping;
            jdata->map->ranking = prrte_rmaps_base.ranking;
        } else {
            /* default based on number of procs */
            if (nprocs <= 2) {
                if (1 < prrte_rmaps_base.cpus_per_rank) {
                    /* assigning multiple cpus to a rank requires that we map to
                     * objects that have multiple cpus in them, so default
                     * to byslot if nothing else was specified by the user.
                     */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given - using byslot", __LINE__);
                    PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                } else if (prrte_hwloc_use_hwthreads_as_cpus) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given - using byhwthread", __LINE__);
                    PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYHWTHREAD);
                } else {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given - using bycore", __LINE__);
                    PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYCORE);
                }
            } else {
                /* if NUMA is available, map by that */
                if (NULL != hwloc_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_NODE, 0)) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not set by user - using bynuma", __LINE__);
                    PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYNUMA);
                } else if (NULL != hwloc_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_SOCKET, 0)) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not set by user and no NUMA - using bysocket", __LINE__);
                    PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSOCKET);
                } else {
                    /* if we have neither, then just do by slot */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given and no NUMA or sockets - using byslot", __LINE__);
                    PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYSLOT);
                }
            }
            PRRTE_SET_RANKING_POLICY(prrte_rmaps_base.ranking, PRRTE_RANK_BY_SLOT);
        }
    }

    /* check for oversubscribe directives */
    if (!(PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (!(PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) {
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }

    /* check for no-use-local directive */
    if (prrte_ras_base.launch_orted_on_hn) {
        /* must override any setting */
        PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_USE_LOCAL);
    } else if (!(PRRTE_MAPPING_LOCAL_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (inherit && (PRRTE_MAPPING_NO_USE_LOCAL & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_USE_LOCAL);
        }
    }

    /* we don't have logic to determine default rank policy, so
     * just inherit it if they didn't give us one */
    if (!PRRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
        jdata->map->ranking = prrte_rmaps_base.ranking;
    }

    /* define the binding policy for this job - if the user specified one
     * already (e.g., during the call to comm_spawn), then we don't
     * override it */
    if (!PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
        if (inherit && PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy)) {
            /* if the user specified a default binding policy via
             * MCA param, then we use it - this can include a directive
             * to overload */
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps[%d] binding policy given", __LINE__);
            jdata->map->binding = prrte_hwloc_binding_policy;
        } else if (0 < jdata->map->cpus_per_rank) {
            /* bind to cpus */
            if (prrte_hwloc_use_hwthreads_as_cpus) {
                /* if we are using hwthread cpus, then bind to those */
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_HWTHREAD);
            } else {
                /* bind to core */
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_CORE);
            }
        } else {
            /* if the user explicitly mapped-by some object, then we default
             * to binding to that object */
            prrte_mapping_policy_t mpol;
            mpol = PRRTE_GET_MAPPING_POLICY(jdata->map->mapping);
            if (PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                if (PRRTE_MAPPING_BYHWTHREAD == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_HWTHREAD);
                } else if (PRRTE_MAPPING_BYCORE == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_CORE);
                } else if (PRRTE_MAPPING_BYL1CACHE == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using byL1", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_L1CACHE);
                } else if (PRRTE_MAPPING_BYL2CACHE == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using byL2", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_L2CACHE);
                } else if (PRRTE_MAPPING_BYL3CACHE == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using byL3", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_L3CACHE);
                } else if (PRRTE_MAPPING_BYSOCKET == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using bysocket", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_SOCKET);
                } else if (PRRTE_MAPPING_BYNUMA == mpol) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using bynuma", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_NUMA);
                } else {
                    /* we are mapping by node or some other non-object method */
                    if (nprocs <= 2) {
                        if (prrte_hwloc_use_hwthreads_as_cpus) {
                            /* if we are using hwthread cpus, then bind to those */
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                            PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_HWTHREAD);
                        } else {
                            /* for performance, bind to core */
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                            "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                            PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_CORE);
                        }
                    } else {
                        if (NULL != hwloc_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_NODE, 0)) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps[%d] binding not given - using bynuma", __LINE__);
                            PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_NUMA);
                        } else if (NULL != hwloc_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_SOCKET, 0)) {
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps[%d] binding not given and no NUMA - using bysocket", __LINE__);
                            PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_SOCKET);
                        } else {
                            /* if we have neither, then just don't bind */
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "mca:rmaps[%d] binding not given and no NUMA or sockets - not binding", __LINE__);
                            PRRTE_SET_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_NONE);
                        }
                    }
                }
            } else if (nprocs <= 2) {
                if (prrte_hwloc_use_hwthreads_as_cpus) {
                    /* if we are using hwthread cpus, then bind to those */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_HWTHREAD);
                } else {
                    /* for performance, bind to core */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_CORE);
                }
            } else {
                /* for performance, bind to NUMA, if available */
                if (NULL != hwloc_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_NODE, 0)) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using bynuma", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_NUMA);
                } else if (NULL != hwloc_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_SOCKET, 0)) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given and no NUMA - using bysocket", __LINE__);
                    PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_SOCKET);
                } else {
                    /* if we have neither, then just don't bind */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given and no NUMA or sockets - not binding", __LINE__);
                    PRRTE_SET_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_NONE);
                }
            }
            if (PRRTE_BIND_OVERLOAD_ALLOWED(prrte_hwloc_binding_policy)) {
                jdata->map->binding |= PRRTE_BIND_ALLOW_OVERLOAD;
            }
        }
    }

    /* if we are not going to launch, then we need to set any
     * undefined topologies to match our own so the mapper
     * can operate
     */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL)) {
        prrte_node_t *node;
        prrte_topology_t *t0;
        int i;
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0))) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            PRRTE_RELEASE(caddy);
            jdata->exit_code = PRRTE_ERR_NOT_FOUND;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            return;
        }
        t0 = node->topology;
        for (i=1; i < prrte_node_pool->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                node->topology = t0;
            }
        }
    }

    /* cycle thru the available mappers until one agrees to map
     * the job
     */
    did_map = false;
    if (1 == prrte_list_get_size(&prrte_rmaps_base.selected_modules)) {
        /* forced selection */
        mod = (prrte_rmaps_base_selected_module_t*)prrte_list_get_first(&prrte_rmaps_base.selected_modules);
        jdata->map->req_mapper = strdup(mod->component->mca_component_name);
    }
    PRRTE_LIST_FOREACH(mod, &prrte_rmaps_base.selected_modules, prrte_rmaps_base_selected_module_t) {
        if (PRRTE_SUCCESS == (rc = mod->module->map_job(jdata)) ||
            PRRTE_ERR_RESOURCE_BUSY == rc) {
            did_map = true;
            break;
        }
        /* mappers return "next option" if they didn't attempt to
         * map the job. anything else is a true error.
         */
        if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }

    if (did_map && PRRTE_ERR_RESOURCE_BUSY == rc) {
        /* the map was done but nothing could be mapped
         * for launch as all the resources were busy
         */
        prrte_show_help("help-prrte-rmaps-base.txt", "cannot-launch", true);
        jdata->exit_code = rc;
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* if we get here without doing the map, or with zero procs in
     * the map, then that's an error
     */
    if (!did_map || 0 == jdata->num_procs || 0 == jdata->map->num_nodes) {
        prrte_show_help("help-prrte-rmaps-base.txt", "failed-map", true,
                       did_map ? "mapped" : "unmapped",
                       jdata->num_procs, jdata->map->num_nodes);
        jdata->exit_code = -PRRTE_JOB_STATE_MAP_FAILED;
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* if any node is oversubscribed, then check to see if a binding
     * directive was given - if not, then we want to clear the default
     * binding policy so we don't attempt to bind */
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_OVERSUBSCRIBED)) {
        if (!PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            /* clear any default binding policy we might have set */
            PRRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_NONE);
        }
    }

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        /* compute the ranks and add the proc objects
         * to the jdata->procs array */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_vpids(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* compute and save local ranks */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_local_ranks(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* compute and save location assignments */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_assign_locations(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* compute and save bindings */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_bindings(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    } else if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
        /* compute and save location assignments */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_assign_locations(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    } else {
        /* compute and save local ranks */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_local_ranks(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }

        /* compute and save bindings */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_bindings(jdata))) {
            PRRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }

    /* set the offset so shared memory components can potentially
     * connect to any spawned jobs
     */
    jdata->offset = prrte_total_procs;
    /* track the total number of procs launched by us */
    prrte_total_procs += jdata->num_procs;

    /* if it is a dynamic spawn, save the bookmark on the parent's job too */
    if (PRRTE_JOBID_INVALID != jdata->originator.jobid) {
        if (NULL != (parent = prrte_get_job_data_object(jdata->originator.jobid))) {
            parent->bookmark = jdata->bookmark;
        }
    }

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        /* display the map */
        prrte_rmaps_base_display_map(jdata);
    }

    /* set the job state to the next position */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP_COMPLETE);

  cleanup:
      /* reset any node map flags we used so the next job will start clean */
       for (i=0; i < jdata->map->nodes->size; i++) {
           if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, i))) {
               PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
           }
       }

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

static void prrte_print_map(char **output, prrte_job_t *jdata);
static void prrte_print_node(char **output,
                             prrte_job_t *jdata,
                             prrte_node_t *src);
static void prrte_print_proc(char **output,
                             prrte_job_t *jdata,
                             prrte_proc_t *src);

void prrte_rmaps_base_display_map(prrte_job_t *jdata)
{
    /* ignore daemon job */
    char *output=NULL;
    int i, j, cnt;
    prrte_node_t *node;
    prrte_proc_t *proc;
    char tmp1[1024];
    hwloc_obj_t bd=NULL;;
    prrte_hwloc_locality_t locality;
    prrte_proc_t *p0;
    char *p0bitmap, *procbitmap;

    /* only have rank=0 output this */
    if (0 != PRRTE_PROC_MY_NAME->vpid) {
        return;
    }

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        /* intended solely to test mapping methods, this output
         * can become quite long when testing at scale. Rather
         * than enduring all the malloc/free's required to
         * create an arbitrary-length string, custom-generate
         * the output a line at a time here
         */
        /* display just the procs in a diffable format */
        prrte_output(prrte_clean_output, "<map>\n");
        fflush(stderr);
        /* loop through nodes */
        cnt = 0;
        for (i=0; i < jdata->map->nodes->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, i))) {
                continue;
            }
            prrte_output(prrte_clean_output, "\t<host num=%d>", cnt);
            fflush(stderr);
            cnt++;
            for (j=0; j < node->procs->size; j++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                if (proc->job != jdata) {
                    continue;
                }
                memset(tmp1, 0, sizeof(tmp1));
                if (prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_BOUND, (void**)&bd, PRRTE_PTR)) {
                    if (NULL == bd) {
                        (void)prrte_string_copy(tmp1, "UNBOUND", sizeof(tmp1));
                    } else {
                        if (PRRTE_ERR_NOT_BOUND == prrte_hwloc_base_cset2mapstr(tmp1, sizeof(tmp1), node->topology->topo, bd->cpuset)) {
                            (void)prrte_string_copy(tmp1, "UNBOUND", sizeof(tmp1));
                        }
                    }
                } else {
                    (void)prrte_string_copy(tmp1, "UNBOUND", sizeof(tmp1));
                }
                prrte_output(prrte_clean_output, "\t\t<process rank=%s app_idx=%ld local_rank=%lu node_rank=%lu binding=%s>",
                            PRRTE_VPID_PRINT(proc->name.vpid),  (long)proc->app_idx,
                            (unsigned long)proc->local_rank,
                            (unsigned long)proc->node_rank, tmp1);
            }
            prrte_output(prrte_clean_output, "\t</host>");
            fflush(stderr);
        }

         /* test locality - for the first node, print the locality of each proc relative to the first one */
        node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, 0);
        p0 = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, 0);
        if (NULL == p0) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            return;
        }
        p0bitmap = NULL;
        if (prrte_get_attribute(&p0->attributes, PRRTE_PROC_CPU_BITMAP, (void**)&p0bitmap, PRRTE_STRING) &&
            NULL != p0bitmap) {
            prrte_output(prrte_clean_output, "\t<locality>");
            for (j=1; j < node->procs->size; j++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                if (proc->job != jdata) {
                    continue;
                }
                procbitmap = NULL;
                if (prrte_get_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP, (void**)&procbitmap, PRRTE_STRING) &&
                    NULL != procbitmap) {
                    locality = prrte_hwloc_base_get_relative_locality(node->topology->topo,
                                                                     p0bitmap,
                                                                     procbitmap);
                    prrte_output(prrte_clean_output, "\t\t<rank=%s rank=%s locality=%s>",
                                PRRTE_VPID_PRINT(p0->name.vpid),
                                PRRTE_VPID_PRINT(proc->name.vpid),
                                prrte_hwloc_base_print_locality(locality));
                }
            }
            prrte_output(prrte_clean_output, "\t</locality>\n</map>");
            fflush(stderr);
            if (NULL != p0bitmap) {
                free(p0bitmap);
            }
            if (NULL != procbitmap) {
                free(procbitmap);
            }
        }
    } else {
        prrte_output(prrte_clean_output, " Data for JOB %s offset %s Total slots allocated %lu",
                    PRRTE_JOBID_PRINT(jdata->jobid), PRRTE_VPID_PRINT(jdata->offset),
                    (long unsigned)jdata->total_slots_alloc);
        prrte_print_map(&output, jdata);
        if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_XML_OUTPUT, NULL, PRRTE_BOOL)) {
            fprintf(prrte_xml_fp, "%s\n", output);
            fflush(prrte_xml_fp);
        } else {
            prrte_output(prrte_clean_output, "%s", output);
        }
        free(output);
    }
}

static void prrte_print_map(char **output, prrte_job_t *jdata)
{
    char *tmp=NULL, *tmp2, *tmp3;
    int32_t i, j;
    prrte_node_t *node;
    prrte_proc_t *proc;
    prrte_job_map_t *src = jdata->map;

    /* set default result */
    *output = NULL;

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_XML_OUTPUT, NULL, PRRTE_BOOL)) {
        /* need to create the output in XML format */
        prrte_asprintf(&tmp, "<map>\n");
        /* loop through nodes */
        for (i=0; i < src->nodes->size; i++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(src->nodes, i))) {
                continue;
            }
            prrte_print_node(&tmp2, jdata, node);
            prrte_asprintf(&tmp3, "%s%s", tmp, tmp2);
            free(tmp2);
            free(tmp);
            tmp = tmp3;
            /* for each node, loop through procs and print their rank */
            for (j=0; j < node->procs->size; j++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                if (proc->job != jdata) {
                    continue;
                }
                prrte_print_proc(&tmp2, jdata, proc);
                prrte_asprintf(&tmp3, "%s%s", tmp, tmp2);
                free(tmp2);
                free(tmp);
                tmp = tmp3;
            }
            prrte_asprintf(&tmp3, "%s\t</host>\n", tmp);
            free(tmp);
            tmp = tmp3;
        }
        prrte_asprintf(&tmp2, "%s</map>\n", tmp);
        free(tmp);
        *output = tmp2;
        return;

    }

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL)) {
        prrte_asprintf(&tmp, "\n========================   JOB MAP   ========================\n"
                       "Mapper requested: %s  Last mapper: %s  Mapping policy: %s  Ranking policy: %s\n"
                       "Binding policy: %s  Cpu set: %s  PPR: %s  Cpus-per-rank: %d",
                       (NULL == src->req_mapper) ? "NULL" : src->req_mapper,
                       (NULL == src->last_mapper) ? "NULL" : src->last_mapper,
                       prrte_rmaps_base_print_mapping(src->mapping),
                       prrte_rmaps_base_print_ranking(src->ranking),
                       prrte_hwloc_base_print_binding(src->binding),
                       (NULL == prrte_hwloc_base_cpu_list) ? "NULL" : prrte_hwloc_base_cpu_list,
                       (NULL == src->ppr) ? "NULL" : src->ppr,
                       (int)src->cpus_per_rank);

        if (PRRTE_VPID_INVALID == src->daemon_vpid_start) {
            prrte_asprintf(&tmp2, "%s\nNum new daemons: %ld\tNew daemon starting vpid INVALID\nNum nodes: %ld",
                           tmp, (long)src->num_new_daemons, (long)src->num_nodes);
        } else {
            prrte_asprintf(&tmp2, "%s\nNum new daemons: %ld\tNew daemon starting vpid %ld\nNum nodes: %ld",
                           tmp, (long)src->num_new_daemons, (long)src->daemon_vpid_start,
                           (long)src->num_nodes);
        }
        free(tmp);
        tmp = tmp2;
    } else {
        /* this is being printed for a user, so let's make it easier to see */
        prrte_asprintf(&tmp, "\n========================   JOB MAP   ========================");
    }


    for (i=0; i < src->nodes->size; i++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(src->nodes, i))) {
            continue;
        }
        prrte_print_node(&tmp2, jdata, node);
        prrte_asprintf(&tmp3, "%s\n%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    /* let's make it easier to see */
    prrte_asprintf(&tmp2, "%s\n\n=============================================================\n", tmp);
    free(tmp);
    tmp = tmp2;

    /* set the return */
    *output = tmp;

    return;
}

static void prrte_print_node(char **output,
                             prrte_job_t *jdata,
                             prrte_node_t *src)
{
    char *tmp, *tmp2, *tmp3, *pfx2 = "\t", *pfx3;
    int32_t i;
    prrte_proc_t *proc;
    char **alias;

    /* set default result */
    *output = NULL;

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_XML_OUTPUT, NULL, PRRTE_BOOL)) {
        /* need to create the output in XML format */
        prrte_asprintf(&tmp, "%s<host name=\"%s\" slots=\"%d\" max_slots=\"%d\">\n", pfx2,
                       (NULL == src->name) ? "UNKNOWN" : src->name,
                       (int)src->slots, (int)src->slots_max);
        /* does this node have any aliases? */
        tmp3 = NULL;
        if (prrte_get_attribute(&src->attributes, PRRTE_NODE_ALIAS, (void**)&tmp3, PRRTE_STRING)) {
            alias = prrte_argv_split(tmp3, ',');
            for (i=0; NULL != alias[i]; i++) {
                prrte_asprintf(&tmp2, "%s%s\t<noderesolve resolved=\"%s\"/>\n", tmp, pfx2, alias[i]);
                free(tmp);
                tmp = tmp2;
            }
            prrte_argv_free(alias);
        }
        if (NULL != tmp3) {
            free(tmp3);
        }
        *output = tmp;
        return;
    }

    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL)) {
        /* just provide a simple output for users */
        if (0 == src->num_procs) {
            /* no procs mapped yet, so just show allocation */
            prrte_asprintf(&tmp, "\n%sData for node: %s\tNum slots: %ld\tMax slots: %ld",
                           pfx2, (NULL == src->name) ? "UNKNOWN" : src->name,
                           (long)src->slots, (long)src->slots_max);
            /* does this node have any aliases? */
            tmp3 = NULL;
            if (prrte_get_attribute(&src->attributes, PRRTE_NODE_ALIAS, (void**)&tmp3, PRRTE_STRING)) {
                alias = prrte_argv_split(tmp3, ',');
                for (i=0; NULL != alias[i]; i++) {
                    prrte_asprintf(&tmp2, "%s%s\tresolved from %s\n", tmp, pfx2, alias[i]);
                    free(tmp);
                    tmp = tmp2;
                }
                prrte_argv_free(alias);
            }
            if (NULL != tmp3) {
                free(tmp3);
            }
            *output = tmp;
            return;
        }
        prrte_asprintf(&tmp, "\n%sData for node: %s\tNum slots: %ld\tMax slots: %ld\tNum procs: %ld",
                       pfx2, (NULL == src->name) ? "UNKNOWN" : src->name,
                       (long)src->slots, (long)src->slots_max, (long)src->num_procs);
        /* does this node have any aliases? */
        tmp3 = NULL;
        if (prrte_get_attribute(&src->attributes, PRRTE_NODE_ALIAS, (void**)&tmp3, PRRTE_STRING)) {
            alias = prrte_argv_split(tmp3, ',');
            for (i=0; NULL != alias[i]; i++) {
                prrte_asprintf(&tmp2, "%s%s\tresolved from %s\n", tmp, pfx2, alias[i]);
                free(tmp);
                tmp = tmp2;
            }
            prrte_argv_free(alias);
        }
        if (NULL != tmp3) {
            free(tmp3);
        }
        goto PRINT_PROCS;
    }

    prrte_asprintf(&tmp, "\n%sData for node: %s\tState: %0x\tFlags: %02x",
             pfx2, (NULL == src->name) ? "UNKNOWN" : src->name, src->state, src->flags);
    /* does this node have any aliases? */
    tmp3 = NULL;
    if (prrte_get_attribute(&src->attributes, PRRTE_NODE_ALIAS, (void**)&tmp3, PRRTE_STRING)) {
        alias = prrte_argv_split(tmp3, ',');
        for (i=0; NULL != alias[i]; i++) {
            prrte_asprintf(&tmp2, "%s%s\tresolved from %s\n", tmp, pfx2, alias[i]);
            free(tmp);
            tmp = tmp2;
        }
        prrte_argv_free(alias);
    }
    if (NULL != tmp3) {
        free(tmp3);
    }

    if (NULL == src->daemon) {
        prrte_asprintf(&tmp2, "%s\n%s\tDaemon: %s\tDaemon launched: %s", tmp, pfx2,
                 "Not defined", PRRTE_FLAG_TEST(src, PRRTE_NODE_FLAG_DAEMON_LAUNCHED) ? "True" : "False");
    } else {
        prrte_asprintf(&tmp2, "%s\n%s\tDaemon: %s\tDaemon launched: %s", tmp, pfx2,
                 PRRTE_NAME_PRINT(&(src->daemon->name)),
                 PRRTE_FLAG_TEST(src, PRRTE_NODE_FLAG_DAEMON_LAUNCHED) ? "True" : "False");
    }
    free(tmp);
    tmp = tmp2;

    prrte_asprintf(&tmp2, "%s\n%s\tNum slots: %ld\tSlots in use: %ld\tOversubscribed: %s", tmp, pfx2,
             (long)src->slots, (long)src->slots_inuse,
             PRRTE_FLAG_TEST(src, PRRTE_NODE_FLAG_OVERSUBSCRIBED) ? "TRUE" : "FALSE");
    free(tmp);
    tmp = tmp2;

    prrte_asprintf(&tmp2, "%s\n%s\tNum slots allocated: %ld\tMax slots: %ld", tmp, pfx2,
             (long)src->slots, (long)src->slots_max);
    free(tmp);
    tmp = tmp2;

    tmp3 = NULL;
    if (prrte_get_attribute(&src->attributes, PRRTE_NODE_USERNAME, (void**)&tmp3, PRRTE_STRING)) {
        prrte_asprintf(&tmp2, "%s\n%s\tUsername on node: %s", tmp, pfx2, tmp3);
        free(tmp3);
        free(tmp);
        tmp = tmp2;
    }

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_TOPO, NULL, PRRTE_BOOL)
        && NULL != src->topology) {
        prrte_asprintf(&tmp2, "%s\n%s\tDetected Resources:\n", tmp, pfx2);
        free(tmp);
        tmp = tmp2;

        tmp2 = NULL;
        prrte_asprintf(&pfx3, "%s\t\t", pfx2);
        prrte_dss.print(&tmp2, pfx3, src->topology, PRRTE_HWLOC_TOPO);
        free(pfx3);
        prrte_asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    prrte_asprintf(&tmp2, "%s\n%s\tNum procs: %ld\tNext node_rank: %ld", tmp, pfx2,
             (long)src->num_procs, (long)src->next_node_rank);
    free(tmp);
    tmp = tmp2;

 PRINT_PROCS:
    for (i=0; i < src->procs->size; i++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(src->procs, i))) {
            continue;
        }
        if (proc->job != jdata) {
            continue;
        }
        prrte_print_proc(&tmp2, jdata, proc);
        prrte_asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    /* set the return */
    *output = tmp;

    return;
}

static void prrte_print_proc(char **output,
                             prrte_job_t *jdata,
                             prrte_proc_t *src)
{
    char *tmp, *tmp3, *pfx2 = "\t\t";
    hwloc_obj_t loc=NULL;
    char locale[1024], tmp1[1024], tmp2[1024];
    hwloc_cpuset_t mycpus;
    char *str, *cpu_bitmap=NULL;


    /* set default result */
    *output = NULL;

    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_XML_OUTPUT, NULL, PRRTE_BOOL)) {
        /* need to create the output in XML format */
        if (0 == src->pid) {
            prrte_asprintf(output, "%s<process rank=\"%s\" status=\"%s\"/>\n", pfx2,
                           PRRTE_VPID_PRINT(src->name.vpid), prrte_proc_state_to_str(src->state));
        } else {
            prrte_asprintf(output, "%s<process rank=\"%s\" pid=\"%d\" status=\"%s\"/>\n", pfx2,
                           PRRTE_VPID_PRINT(src->name.vpid), (int)src->pid, prrte_proc_state_to_str(src->state));
        }
        return;
    }

    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL)) {
        if (prrte_get_attribute(&src->attributes, PRRTE_PROC_CPU_BITMAP, (void**)&cpu_bitmap, PRRTE_STRING) &&
            NULL != cpu_bitmap && NULL != src->node->topology && NULL != src->node->topology->topo) {
            mycpus = hwloc_bitmap_alloc();
            hwloc_bitmap_list_sscanf(mycpus, cpu_bitmap);
            if (PRRTE_ERR_NOT_BOUND == prrte_hwloc_base_cset2str(tmp1, sizeof(tmp1), src->node->topology->topo, mycpus)) {
                str = strdup("UNBOUND");
            } else {
                prrte_hwloc_base_cset2mapstr(tmp2, sizeof(tmp2), src->node->topology->topo, mycpus);
                prrte_asprintf(&str, "%s:%s", tmp1, tmp2);
            }
            hwloc_bitmap_free(mycpus);
            prrte_asprintf(&tmp, "\n%sProcess jobid: %s App: %ld Process rank: %s Bound: %s", pfx2,
                     PRRTE_JOBID_PRINT(src->name.jobid), (long)src->app_idx,
                     PRRTE_VPID_PRINT(src->name.vpid), str);
            free(str);
            free(cpu_bitmap);
        } else {
            /* just print a very simple output for users */
            prrte_asprintf(&tmp, "\n%sProcess jobid: %s App: %ld Process rank: %s Bound: N/A", pfx2,
                           PRRTE_JOBID_PRINT(src->name.jobid), (long)src->app_idx,
                           PRRTE_VPID_PRINT(src->name.vpid));
        }

        /* set the return */
        *output = tmp;
        return;
    }

    prrte_asprintf(&tmp, "\n%sData for proc: %s", pfx2, PRRTE_NAME_PRINT(&src->name));

    prrte_asprintf(&tmp3, "%s\n%s\tPid: %ld\tLocal rank: %lu\tNode rank: %lu\tApp rank: %d", tmp, pfx2,
             (long)src->pid, (unsigned long)src->local_rank, (unsigned long)src->node_rank, src->app_rank);
    free(tmp);
    tmp = tmp3;

    if (prrte_get_attribute(&src->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&loc, PRRTE_PTR)) {
        if (NULL != loc) {
            if (PRRTE_ERR_NOT_BOUND == prrte_hwloc_base_cset2mapstr(locale, sizeof(locale), src->node->topology->topo, loc->cpuset)) {
                strcpy(locale, "NODE");
            }
        } else {
            strcpy(locale, "UNKNOWN");
        }
    } else {
        strcpy(locale, "UNKNOWN");
    }
    if (prrte_get_attribute(&src->attributes, PRRTE_PROC_CPU_BITMAP, (void**)&cpu_bitmap, PRRTE_STRING) &&
        NULL != src->node->topology && NULL != src->node->topology->topo) {
        mycpus = hwloc_bitmap_alloc();
        hwloc_bitmap_list_sscanf(mycpus, cpu_bitmap);
        prrte_hwloc_base_cset2mapstr(tmp2, sizeof(tmp2), src->node->topology->topo, mycpus);
        hwloc_bitmap_free(mycpus);
    } else {
        snprintf(tmp2, sizeof(tmp2), "UNBOUND");
    }
    prrte_asprintf(&tmp3, "%s\n%s\tState: %s\tApp_context: %ld\n%s\tLocale:  %s\n%s\tBinding: %s", tmp, pfx2,
                   prrte_proc_state_to_str(src->state), (long)src->app_idx, pfx2, locale, pfx2,  tmp2);
    free(tmp);
    if (NULL != cpu_bitmap) {
        free(cpu_bitmap);
    }

    /* set the return */
    *output = tmp3;

    return;
}
