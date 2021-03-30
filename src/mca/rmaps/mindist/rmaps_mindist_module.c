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
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/mca/base/prte_mca_base_var.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/error_strings.h"
#include "src/util/show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/mindist/rmaps_mindist.h"

static int mindist_map(prte_job_t *jdata);
static int assign_locations(prte_job_t *jdata);

prte_rmaps_base_module_t prte_rmaps_mindist_module = {.map_job = mindist_map,
                                                      .assign_locations = assign_locations};

/*
 * Create a round-robin mapping for the job.
 */
static int mindist_map(prte_job_t *jdata)
{
    prte_app_context_t *app;
    int i, j;
    unsigned int k;
    hwloc_obj_t obj = NULL;
    prte_list_t node_list;
    prte_list_t numa_list;
    prte_list_item_t *item;
    prte_list_item_t *numa_item;
    prte_rmaps_numa_node_t *numa;
    prte_node_t *node;
    prte_proc_t *proc;
    int nprocs_mapped;
    int navg = 0, nextra = 0;
    int32_t num_nodes, num_slots;
    unsigned int npus, total_npus, num_procs_to_assign = 0, required;
    int rc;
    prte_mca_base_component_t *c = &prte_rmaps_mindist_component.base_version;
    bool initial_map = true;
    bool bynode = false;
    float balance;
    int extra_procs_to_assign = 0, nxtra_nodes = 0;
    bool add_one = false;
    bool oversubscribed = false;
    int ret;
    bool use_hwthread_cpus;
    char *job_cpuset;
    hwloc_cpuset_t available, mycpus;
    prte_hwloc_topo_data_t *rdata;
    char *device;

    /* this mapper can only handle initial launch
     * when mindist mapping is desired
     */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:mindist: job %s is being restarted - mindist cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper
        && 0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:mindist: job %s not using mindist mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRTE_MAPPING_BYDIST != PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* not me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:mindist: job %s not using mindist mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* there are two modes for mapping by dist: span and not-span. The
     * span mode essentially operates as if there was just a single
     * "super-node" in the system - i.e., it balances the load across
     * all objects of the indicated type regardless of their location.
     * In essence, it acts as if we placed one proc on each object, cycling
     * across all objects on all nodes, and then wrapped around to place
     * another proc on each object, doing so until all procs were placed.
     *
     * In contrast, the non-span mode operates similar to byslot mapping.
     * All slots on each node are filled, assigning each proc to an object
     * on that node in a balanced fashion, and then the mapper moves on
     * to the next node. Thus, procs tend to be "front loaded" onto the
     * list of nodes, as opposed to being "load balanced" in the span mode
     */

    if (PRTE_MAPPING_SPAN & jdata->map->mapping) {
        /* do a bynode mapping */
        bynode = true;
    } else {
        /* do a byslot mapping */
        bynode = false;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:mindist: mapping job %s", PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* start at the beginning... */
    jdata->num_procs = 0;

    /* check for type of cpu being used */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING);

    /* get the target device */
    device = NULL;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DIST_DEVICE, (void **) device, PMIX_STRING)
        || NULL == device) {
        if (NULL == job_cpuset) {
            free(job_cpuset);
        }
        return PRTE_ERR_BAD_PARAM;
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }

        /* setup the nodelist here in case we jump to error */
        PRTE_CONSTRUCT(&node_list, prte_list_t);

        /* if the number of processes wasn't specified, then we know there can be only
         * one app_context allowed in the launch, and that we are to launch it across
         * all available slots. We'll double-check the single app_context rule first
         */
        if (0 == app->num_procs && 1 < jdata->num_apps) {
            prte_show_help("help-prte-rmaps-md.txt", "multi-apps-and-zero-np", true,
                           jdata->num_apps, NULL);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        if (PRTE_SUCCESS
            != (rc = prte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                      jdata->map->mapping, initial_map, false))) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }

        /* quick check to see if we can map all the procs */
        if (num_slots < (int) app->num_procs) {
            if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                               app->num_procs, app->app, prte_process_info.nodename);
                PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                return PRTE_ERR_SILENT;
            }
            oversubscribed = true;
        }

        num_nodes = (int32_t) prte_list_get_size(&node_list);
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* if a bookmark exists from some prior mapping, set us to start there */
        jdata->bookmark = prte_rmaps_base_get_starting_point(&node_list, jdata);

        if (0 == app->num_procs) {
            /* set the num_procs to equal the number of slots on these mapped nodes */
            app->num_procs = num_slots;
        }

        nprocs_mapped = 0;
        if (!num_nodes) {
            rc = PRTE_ERR_SILENT;
            goto error;
        }
        do {
            if (bynode || (app->num_procs > num_slots)) {
                /* if there is oversubscribe then uses bynode case */
                bynode = true;
                /* calculate num_procs_to_assign for bynode case */
                navg = ((int) app->num_procs - nprocs_mapped) / num_nodes;
                nextra = app->num_procs - navg * num_nodes;
                num_procs_to_assign = navg;
                if (nextra > 0) {
                    num_procs_to_assign++;
                }
                /* compute how many extra procs to put on each node */
                balance = (float) (((int) app->num_procs - nprocs_mapped) - (navg * num_nodes))
                          / (float) num_nodes;
                extra_procs_to_assign = (int) balance;
                nxtra_nodes = 0;
                add_one = false;
                if (0 < (balance - (float) extra_procs_to_assign)) {
                    /* compute how many nodes need an extra proc */
                    nxtra_nodes = ((int) app->num_procs - nprocs_mapped)
                                  - ((navg + extra_procs_to_assign) * num_nodes);
                    /* add one so that we add an extra proc to the first nodes
                     * until all procs are mapped
                     */
                    extra_procs_to_assign++;
                    /* flag that we added one */
                    add_one = true;
                }
            }

            num_nodes = 0;
            /* iterate through the list of nodes */
            for (item = prte_list_get_first(&node_list); item != prte_list_get_end(&node_list);
                 item = prte_list_get_next(item)) {
                node = (prte_node_t *) item;

                if (NULL == node->topology || NULL == node->topology->topo) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-topology", true,
                                   node->name);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                /* get the root object as we are not assigning
                 * locale except at the node level
                 */
                obj = hwloc_get_root_obj(node->topology->topo);
                if (NULL == obj) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-topology", true,
                                   node->name);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }

                num_nodes++;
                /* get the available processors on this node */
                if (NULL == obj->userdata) {
                    /* incorrect */
                    PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                    return PRTE_ERR_BAD_PARAM;
                }
                rdata = (prte_hwloc_topo_data_t *) obj->userdata;
                available = hwloc_bitmap_dup(rdata->available);
                if (NULL != job_cpuset) {
                    mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo,
                                                             use_hwthread_cpus, job_cpuset);
                    hwloc_bitmap_and(available, mycpus, available);
                    hwloc_bitmap_free(mycpus);
                }

                /* get the number of available pus */
                total_npus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                      available, obj);

                if (bynode) {
                    if (oversubscribed) {
                        /* compute the number of procs to go on this node */
                        if (add_one) {
                            if (0 == nxtra_nodes) {
                                --extra_procs_to_assign;
                                add_one = false;
                            } else {
                                --nxtra_nodes;
                            }
                        }
                        /* everybody just takes their share */
                        num_procs_to_assign = navg + extra_procs_to_assign;
                    } else if (node->slots <= node->slots_inuse) {
                        /* since we are not oversubcribed, ignore this node */
                        continue;
                    } else {
                        /* if we are not oversubscribed, then there are enough
                         * slots to handle all the procs. However, not every
                         * node will have the same number of slots, so we
                         * have to track how many procs to "shift" elsewhere
                         * to make up the difference
                         */

                        /* compute the number of procs to go on this node */
                        if (add_one) {
                            if (0 == nxtra_nodes) {
                                --extra_procs_to_assign;
                                add_one = false;
                            } else {
                                --nxtra_nodes;
                            }
                        }
                        /* if slots < avg + extra (adjusted for cpus/proc), then try to take all */
                        if (node->slots_available < (navg + extra_procs_to_assign)) {
                            num_procs_to_assign = node->slots_available;
                            /* if we can't take any proc, skip following steps */
                            if (num_procs_to_assign == 0) {
                                continue;
                            }
                        } else {
                            /* take the avg + extra */
                            num_procs_to_assign = navg + extra_procs_to_assign;
                        }
                        prte_output_verbose(
                            5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:mindist: %s node %s avg %d assign %d extra %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, navg,
                            num_procs_to_assign, extra_procs_to_assign);
                    }
                } else {
                    num_procs_to_assign = ((int) app->num_procs - nprocs_mapped)
                                                  > node->slots_available
                                              ? node->slots_available
                                              : ((int) app->num_procs - nprocs_mapped);
                }

                if (bynode) {
                    if (total_npus < num_procs_to_assign) {
                        /* check if oversubscribing is allowed */
                        if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                            & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                            prte_show_help("help-prte-rmaps-base.txt",
                                           "prte-rmaps-base:alloc-error", true, app->num_procs,
                                           app->app);
                            rc = PRTE_ERR_SILENT;
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                            goto error;
                        } else {
                            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                        }
                    }
                }
                /* first we need to fill summary object for root with information about nodes
                 * so we call prte_hwloc_base_get_nbobjs_by_type */
                prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, HWLOC_OBJ_NODE, 0);
                PRTE_CONSTRUCT(&numa_list, prte_list_t);
                ret = prte_hwloc_get_sorted_numa_list(node->topology->topo, prte_rmaps_base.device,
                                                      &numa_list);
                if (ret > 1) {
                    prte_show_help("help-prte-rmaps-md.txt", "prte-rmaps-mindist:several-devices",
                                   true, prte_rmaps_base.device, ret, node->name);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                    rc = PRTE_ERR_TAKE_NEXT_OPTION;
                    goto error;
                } else if (ret < 0) {
                    prte_show_help("help-prte-rmaps-md.txt", "prte-rmaps-mindist:device-not-found",
                                   true, prte_rmaps_base.device, node->name);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                    rc = PRTE_ERR_TAKE_NEXT_OPTION;
                    goto error;
                }
                if (prte_list_get_size(&numa_list) > 0) {
                    j = 0;
                    required = 0;
                    PRTE_LIST_FOREACH(numa, &numa_list, prte_rmaps_numa_node_t)
                    {
                        /* get the hwloc object for this numa */
                        if (NULL
                            == (obj = prte_hwloc_base_get_obj_by_type(node->topology->topo,
                                                                      HWLOC_OBJ_NODE, 0,
                                                                      numa->index))) {
                            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                            return PRTE_ERR_NOT_FOUND;
                        }
                        npus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                        available, obj);
                        if (bynode) {
                            required = num_procs_to_assign;
                        } else {
                            required = (num_procs_to_assign - j) > npus ? npus
                                                                        : (num_procs_to_assign - j);
                        }
                        for (k = 0; (k < required) && (nprocs_mapped < app->num_procs); k++) {
                            if (NULL == (proc = prte_rmaps_base_setup_proc(jdata, node, i))) {
                                rc = PRTE_ERR_OUT_OF_RESOURCE;
                                goto error;
                            }
                            nprocs_mapped++;
                            j++;
                            prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE,
                                               PRTE_ATTR_LOCAL, obj, PMIX_POINTER);
                        }
                        if ((nprocs_mapped == (int) app->num_procs)
                            || ((int) num_procs_to_assign == j)) {
                            break;
                        }
                    }
                    if (0 != j) {
                        /* add the node to the map, if needed */
                        if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                            PRTE_RETAIN(node); /* maintain accounting on object */
                            jdata->map->num_nodes++;
                            prte_pointer_array_add(jdata->map->nodes, node);
                        }
                        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps:mindist: assigned %d procs to node %s", j,
                                            node->name);
                    }
                } else {
                    if (hwloc_get_nbobjs_by_type(node->topology->topo, HWLOC_OBJ_PACKAGE) > 1) {
                        /* don't have info about pci locality */
                        prte_show_help("help-prte-rmaps-md.txt",
                                       "prte-rmaps-mindist:no-pci-locality-info", true, node->name);
                    }
                    /* else silently switch to byslot mapper since distance info is irrelevant for
                     * this machine configuration */
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                    rc = PRTE_ERR_TAKE_NEXT_OPTION;
                    goto error;
                }
                while (NULL != (numa_item = prte_list_remove_first(&numa_list))) {
                    PRTE_RELEASE(numa_item);
                }
                PRTE_DESTRUCT(&numa_list);
                if (bynode) {
                    nextra--;
                    if (nextra == 0) {
                        num_procs_to_assign--;
                    }
                }
            }
        } while (bynode && nprocs_mapped < app->num_procs && 0 < num_nodes);

        /* track the total number of processes we mapped - must update
         * this value AFTER we compute vpids so that computation
         * is done correctly
         */
        jdata->num_procs += app->num_procs;

        /* cleanup the node list - it can differ from one app_context
         * to another, so we have to get it every time
         */
        PRTE_LIST_DESTRUCT(&node_list);
    }
    if (NULL != device) {
        free(device);
    }

    return PRTE_SUCCESS;

error:
    PRTE_LIST_DESTRUCT(&node_list);
    if (NULL != device) {
        free(device);
    }
    return rc;
}

static int assign_locations(prte_job_t *jdata)
{
    int j, k, m, n, npus;
    prte_node_t *node;
    prte_proc_t *proc;
    hwloc_obj_t obj = NULL;
    prte_mca_base_component_t *c = &prte_rmaps_mindist_component.base_version;
    int rc;
    prte_list_t numa_list;
    prte_rmaps_numa_node_t *numa;
    bool use_hwthread_cpus;
    char *job_cpuset;
    hwloc_cpuset_t available, mycpus;
    prte_hwloc_topo_data_t *rdata;

    if (NULL == jdata->map->last_mapper
        || 0 != strcasecmp(jdata->map->last_mapper, c->mca_component_name)) {
        /* the mapper should have been set to me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:mindist: job %s not using mindist mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:mindist: assign locations for job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* check for type of cpu being used */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING);

    /* start assigning procs to objects, filling each object as we go until
     * all procs are assigned. If one pass doesn't catch all the required procs,
     * then loop thru the list again to handle the oversubscription
     */
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == prte_pointer_array_get_item(jdata->apps, n)) {
            continue;
        }
        for (m = 0; m < jdata->map->nodes->size; m++) {
            if (NULL
                == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            if (NULL == node->topology || NULL == node->topology->topo) {
                prte_show_help("help-prte-rmaps-ppr.txt", "ppr-topo-missing", true, node->name);
                return PRTE_ERR_SILENT;
            }

            obj = hwloc_get_root_obj(node->topology->topo);
            rdata = (prte_hwloc_topo_data_t *) obj->userdata;
            available = hwloc_bitmap_dup(rdata->available);
            if (NULL != job_cpuset) {
                mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                         job_cpuset);
                hwloc_bitmap_and(available, mycpus, available);
                hwloc_bitmap_free(mycpus);
            }

            /* first we need to fill summary object for root with information about nodes
             * so we call prte_hwloc_base_get_nbobjs_by_type */
            prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, HWLOC_OBJ_NODE, 0);
            PRTE_CONSTRUCT(&numa_list, prte_list_t);
            rc = prte_hwloc_get_sorted_numa_list(node->topology->topo, prte_rmaps_base.device,
                                                 &numa_list);
            if (rc > 1) {
                prte_show_help("help-prte-rmaps-md.txt", "prte-rmaps-mindist:several-devices", true,
                               prte_rmaps_base.device, rc, node->name);
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                PRTE_LIST_DESTRUCT(&numa_list);
                return PRTE_ERR_TAKE_NEXT_OPTION;
            } else if (rc < 0) {
                prte_show_help("help-prte-rmaps-md.txt", "prte-rmaps-mindist:device-not-found",
                               true, prte_rmaps_base.device, node->name);
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                PRTE_LIST_DESTRUCT(&numa_list);
                return PRTE_ERR_TAKE_NEXT_OPTION;
            }
            j = 0;
            PRTE_LIST_FOREACH(numa, &numa_list, prte_rmaps_numa_node_t)
            {
                /* get the hwloc object for this numa */
                if (NULL
                    == (obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, HWLOC_OBJ_NODE,
                                                              0, numa->index))) {
                    PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                    PRTE_LIST_DESTRUCT(&numa_list);
                    return PRTE_ERR_NOT_FOUND;
                }
                npus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus, available,
                                                obj);
                /* fill the numa region with procs from this job until we either
                 * have assigned everyone or the region is full */
                for (k = j; k < node->procs->size && 0 < npus; k++) {
                    if (NULL
                        == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, k))) {
                        continue;
                    }
                    if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                        continue;
                    }
                    prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL,
                                       obj, PMIX_POINTER);
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:mindist: assigning proc %d to numa %d", k,
                                        numa->index);
                    ++j;
                    --npus;
                }
            }
            PRTE_LIST_DESTRUCT(&numa_list);
            hwloc_bitmap_free(available);
        }
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    return PRTE_SUCCESS;
}
