/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/argv.h"
#include "src/util/if.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/threads/tsd.h"

#include "types.h"
#include "src/util/show_help.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"

int prrte_rmaps_base_filter_nodes(prrte_app_context_t *app,
                                 prrte_list_t *nodes, bool remove)
{
    int rc=PRRTE_ERR_TAKE_NEXT_OPTION;
    char *hosts;

    /* did the app_context contain a hostfile? */
    if (prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
        /* yes - filter the node list through the file, removing
         * any nodes not found in the file
         */
        if (PRRTE_SUCCESS != (rc = prrte_util_filter_hostfile_nodes(nodes, hosts, remove))) {
            PRRTE_ERROR_LOG(rc);
            free(hosts);
            return rc;
        }
        /** check that anything is here */
        if (0 == prrte_list_get_size(nodes)) {
            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:no-mapped-node",
                           true, app->app, "-hostfile", hosts);
            free(hosts);
            return PRRTE_ERR_SILENT;
        }
        free(hosts);
    }
    /* did the app_context contain an add-hostfile? */
    if (prrte_get_attribute(&app->attributes, PRRTE_APP_ADD_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
        /* yes - filter the node list through the file, removing
         * any nodes not found in the file
         */
        if (PRRTE_SUCCESS != (rc = prrte_util_filter_hostfile_nodes(nodes, hosts, remove))) {
            free(hosts);
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /** check that anything is here */
        if (0 == prrte_list_get_size(nodes)) {
            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:no-mapped-node",
                           true, app->app, "-add-hostfile", hosts);
            free(hosts);
            return PRRTE_ERR_SILENT;
        }
        free(hosts);
    }
    /* now filter the list through any -host specification */
    if (!prrte_soft_locations &&
        prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&hosts, PRRTE_STRING)) {
        if (PRRTE_SUCCESS != (rc = prrte_util_filter_dash_host_nodes(nodes, hosts, remove))) {
            PRRTE_ERROR_LOG(rc);
            free(hosts);
            return rc;
        }
        /** check that anything is left! */
        if (0 == prrte_list_get_size(nodes)) {
            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:no-mapped-node",
                           true, app->app, "-host", hosts);
            free(hosts);
            return PRRTE_ERR_SILENT;
        }
        free(hosts);
    }
    /* now filter the list through any add-host specification */
    if (prrte_get_attribute(&app->attributes, PRRTE_APP_ADD_HOST, (void**)&hosts, PRRTE_STRING)) {
        if (PRRTE_SUCCESS != (rc = prrte_util_filter_dash_host_nodes(nodes, hosts, remove))) {
            PRRTE_ERROR_LOG(rc);
            free(hosts);
            return rc;
        }
        /** check that anything is left! */
        if (0 == prrte_list_get_size(nodes)) {
            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:no-mapped-node",
                           true, app->app, "-add-host", hosts);
            free(hosts);
            return PRRTE_ERR_SILENT;
        }
        free(hosts);
    }

    return rc;
}


/*
 * Query the registry for all nodes allocated to a specified app_context
 */
int prrte_rmaps_base_get_target_nodes(prrte_list_t *allocated_nodes, prrte_std_cntr_t *total_num_slots,
                                     prrte_app_context_t *app, prrte_mapping_policy_t policy,
                                     bool initial_map, bool silent)
{
    prrte_list_item_t *item;
    prrte_node_t *node, *nd, *nptr, *next;
    prrte_std_cntr_t num_slots;
    prrte_std_cntr_t i;
    int rc;
    prrte_job_t *daemons;
    bool novm;
    prrte_list_t nodes;
    char *hosts = NULL;

    /** set default answer */
    *total_num_slots = 0;

    /* get the daemon job object */
    daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    /* see if we have a vm or not */
    novm = prrte_get_attribute(&daemons->attributes, PRRTE_JOB_NO_VM, NULL, PRRTE_BOOL);

    /* if this is NOT a managed allocation, then we use the nodes
     * that were specified for this app - there is no need to collect
     * all available nodes and "filter" them
     */
    if (!prrte_managed_allocation) {
        PRRTE_CONSTRUCT(&nodes, prrte_list_t);
        /* if the app provided a dash-host, and we are not treating
         * them as requested or "soft" locations, then use those nodes
         */
        hosts = NULL;
        if (!prrte_soft_locations &&
            prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&hosts, PRRTE_STRING)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                 "%s using dash_host %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hosts));
            if (PRRTE_SUCCESS != (rc = prrte_util_add_dash_host_nodes(&nodes, hosts, false))) {
                PRRTE_ERROR_LOG(rc);
                free(hosts);
                return rc;
            }
            free(hosts);
        } else if (prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
            /* otherwise, if the app provided a hostfile, then use that */
            PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                 "%s using hostfile %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hosts));
            if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&nodes, hosts))) {
                free(hosts);
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            free(hosts);
        } else {
            /* if nothing else was specified by the app, then use all known nodes, which
             * will include ourselves
             */
            PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                 "%s using known nodes",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            goto addknown;
        }
        /** if we still don't have anything */
        if (0 == prrte_list_get_size(&nodes)) {
            if (!silent) {
                prrte_show_help("help-prrte-rmaps-base.txt",
                               "prrte-rmaps-base:no-available-resources",
                               true);
            }
            PRRTE_DESTRUCT(&nodes);
            return PRRTE_ERR_SILENT;
        }
        /* find the nodes in our node array and assemble them
         * in list order as that is what the user specified
         */
        PRRTE_LIST_FOREACH_SAFE(nptr, next, &nodes, prrte_node_t) {
            for (i=0; i < prrte_node_pool->size; i++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                    continue;
                }
                /* ignore nodes that are non-usable */
                if (PRRTE_FLAG_TEST(node, PRRTE_NODE_NON_USABLE)) {
                    continue;
                }
                if (0 != strcmp(node->name, nptr->name)) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                         "NODE %s DOESNT MATCH NODE %s",
                                         node->name, nptr->name));
                    continue;
                }
                /* ignore nodes that are marked as do-not-use for this mapping */
                if (PRRTE_NODE_STATE_DO_NOT_USE == node->state) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_USE", node->name));
                    /* reset the state so it can be used another time */
                    node->state = PRRTE_NODE_STATE_UP;
                    continue;
                }
                if (PRRTE_NODE_STATE_DOWN == node->state) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                         "NODE %s IS DOWN", node->name));
                    continue;
                }
                if (PRRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_INCLUDE", node->name));
                    /* not to be used */
                    continue;
                }
                /* if this node wasn't included in the vm (e.g., by -host), ignore it,
                 * unless we are mapping prior to launching the vm
                 */
                if (NULL == node->daemon && !novm) {
                    PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                         "NODE %s HAS NO DAEMON", node->name));
                    continue;
                }
                /* retain a copy for our use in case the item gets
                 * destructed along the way
                 */
                PRRTE_RETAIN(node);
                if (initial_map) {
                    /* if this is the first app_context we
                     * are getting for an initial map of a job,
                     * then mark all nodes as unmapped
                     */
                    PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
                }
                /* the list is ordered as per user direction using -host
                 * or the listing in -hostfile - preserve that ordering */
                prrte_list_append(allocated_nodes, &node->super);
                break;
            }
            /* remove the item from the list as we have allocated it */
            prrte_list_remove_item(&nodes, (prrte_list_item_t*)nptr);
            PRRTE_RELEASE(nptr);
        }
        PRRTE_DESTRUCT(&nodes);
        /* now prune for usage and compute total slots */
        goto complete;
    }

  addknown:
    /* add everything in the node pool that can be used - add them
     * in daemon order, which may be different than the order in the
     * node pool. Since an empty list is passed into us, the list at
     * this point either has the HNP node or nothing, and the HNP
     * node obviously has a daemon on it (us!)
     */
    if (0 == prrte_list_get_size(allocated_nodes)) {
        /* the list is empty - if the HNP is allocated, then add it */
        if (prrte_hnp_is_allocated) {
            nd = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
            PRRTE_RETAIN(nd);
            prrte_list_append(allocated_nodes, &nd->super);
        } else {
            nd = NULL;
        }
    } else {
        nd = (prrte_node_t*)prrte_list_get_last(allocated_nodes);
    }
    for (i=1; i < prrte_node_pool->size; i++) {
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
            /* ignore nodes that are non-usable */
            if (PRRTE_FLAG_TEST(node, PRRTE_NODE_NON_USABLE)) {
                continue;
            }
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRRTE_NODE_STATE_UP;
                continue;
            }
            if (PRRTE_NODE_STATE_DOWN == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            /* if this node wasn't included in the vm (e.g., by -host), ignore it,
             * unless we are mapping prior to launching the vm
             */
            if (NULL == node->daemon && !novm) {
                PRRTE_OUTPUT_VERBOSE((10, prrte_rmaps_base_framework.framework_output,
                                     "NODE %s HAS NO DAEMON", node->name));
                continue;
            }
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PRRTE_RETAIN(node);
            if (initial_map) {
                /* if this is the first app_context we
                 * are getting for an initial map of a job,
                 * then mark all nodes as unmapped
                 */
                PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
            }
            if (NULL == nd || NULL == nd->daemon ||
                NULL == node->daemon ||
                nd->daemon->name.vpid < node->daemon->name.vpid) {
                /* just append to end */
                prrte_list_append(allocated_nodes, &node->super);
                nd = node;
            } else {
                /* starting from end, put this node in daemon-vpid order */
                while (node->daemon->name.vpid < nd->daemon->name.vpid) {
                    if (prrte_list_get_begin(allocated_nodes) == prrte_list_get_prev(&nd->super)) {
                        /* insert at beginning */
                        prrte_list_prepend(allocated_nodes, &node->super);
                        goto moveon;
                    }
                    nd = (prrte_node_t*)prrte_list_get_prev(&nd->super);
                }
                item = prrte_list_get_next(&nd->super);
                if (item == prrte_list_get_end(allocated_nodes)) {
                    /* we are at the end - just append */
                    prrte_list_append(allocated_nodes, &node->super);
                } else {
                    nd = (prrte_node_t*)item;
                    prrte_list_insert_pos(allocated_nodes, item, &node->super);
                }
            moveon:
                /* reset us back to the end for the next node */
                nd = (prrte_node_t*)prrte_list_get_last(allocated_nodes);
            }
        }
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s Starting with %d nodes in list",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)prrte_list_get_size(allocated_nodes)));

    /** check that anything is here */
    if (0 == prrte_list_get_size(allocated_nodes)) {
        if (!silent) {
            prrte_show_help("help-prrte-rmaps-base.txt",
                           "prrte-rmaps-base:no-available-resources",
                           true);
        }
        return PRRTE_ERR_SILENT;
    }

    /* filter the nodes thru any hostfile and dash-host options */
    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s Filtering thru apps",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_filter_nodes(app, allocated_nodes, true))
        && PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s Retained %d nodes in list",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)prrte_list_get_size(allocated_nodes)));

  complete:
    num_slots = 0;
    /* remove all nodes that are already at max usage, and
     * compute the total number of allocated slots while
     * we do so - can ignore this if we are mapping debugger
     * daemons as they do not count against the allocation */
    if (PRRTE_MAPPING_DEBUGGER & PRRTE_GET_MAPPING_DIRECTIVE(policy)) {
        num_slots = prrte_list_get_size(allocated_nodes);    // tell the mapper there is one slot/node for debuggers
    } else {
        item  = prrte_list_get_first(allocated_nodes);
        PRRTE_LIST_FOREACH_SAFE(node, next, allocated_nodes, prrte_node_t) {
            /* if the hnp was not allocated, or flagged not to be used,
             * then remove it here */
            if (!prrte_hnp_is_allocated || (PRRTE_GET_MAPPING_DIRECTIVE(policy) & PRRTE_MAPPING_NO_USE_LOCAL)) {
                if (0 == node->index) {
                    prrte_list_remove_item(allocated_nodes, &node->super);
                    PRRTE_RELEASE(node);  /* "un-retain" it */
                    continue;
                }
            }
            /** check to see if this node is fully used - remove if so */
            if (0 != node->slots_max && node->slots_inuse > node->slots_max) {
                PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                     "%s Removing node %s: max %d inuse %d",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     node->name, node->slots_max, node->slots_inuse));
                prrte_list_remove_item(allocated_nodes, &node->super);
                PRRTE_RELEASE(node);  /* "un-retain" it */
                continue;
            }
            if (node->slots <= node->slots_inuse &&
                       (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(policy))) {
                /* remove the node as fully used */
                PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                     "%s Removing node %s slots %d inuse %d",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     node->name, node->slots, node->slots_inuse));
                prrte_list_remove_item(allocated_nodes, &node->super);
                PRRTE_RELEASE(node);  /* "un-retain" it */
                continue;
            }
            if (node->slots > node->slots_inuse) {
                prrte_std_cntr_t s;
                /* check for any -host allocations */
                if (prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&hosts, PRRTE_STRING)) {
                    s = prrte_util_dash_host_compute_slots(node, hosts);
                } else {
                    s = node->slots - node->slots_inuse;
                }
                /* add the available slots */
                PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                     "%s node %s has %d slots available",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     node->name, s));
                num_slots += s;
                continue;
            }
            if (!(PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(policy))) {
                /* nothing needed to do here - we don't add slots to the
                 * count as we don't have any available. Just let the mapper
                 * do what it needs to do to meet the request
                 */
                PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                                     "%s node %s is fully used, but available for oversubscription",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     node->name));
            } else {
                /* if we cannot use it, remove it from list */
                prrte_list_remove_item(allocated_nodes, &node->super);
                PRRTE_RELEASE(node);  /* "un-retain" it */
            }
        }
    }

    /* Sanity check to make sure we have resources available */
    if (0 == prrte_list_get_size(allocated_nodes)) {
        if (silent) {
            /* let the caller know that the resources exist,
             * but are currently busy
             */
            return PRRTE_ERR_RESOURCE_BUSY;
        } else {
            prrte_show_help("help-prrte-rmaps-base.txt",
                           "prrte-rmaps-base:all-available-resources-used", true);
            return PRRTE_ERR_SILENT;
        }
    }

    /* pass back the total number of available slots */
    *total_num_slots = num_slots;

    if (4 < prrte_output_get_verbosity(prrte_rmaps_base_framework.framework_output)) {
        prrte_output(0, "AVAILABLE NODES FOR MAPPING:");
        for (item = prrte_list_get_first(allocated_nodes);
             item != prrte_list_get_end(allocated_nodes);
             item = prrte_list_get_next(item)) {
            node = (prrte_node_t*)item;
            prrte_output(0, "    node: %s daemon: %s", node->name,
                        (NULL == node->daemon) ? "NULL" : PRRTE_VPID_PRINT(node->daemon->name.vpid));
        }
    }

    return PRRTE_SUCCESS;
}

prrte_proc_t* prrte_rmaps_base_setup_proc(prrte_job_t *jdata,
                                        prrte_node_t *node,
                                        prrte_app_idx_t idx)
{
    prrte_proc_t *proc;
    int rc;

    proc = PRRTE_NEW(prrte_proc_t);
    /* set the jobid */
    proc->name.jobid = jdata->jobid;
    /* flag the proc as ready for launch */
    proc->state = PRRTE_PROC_STATE_INIT;
    proc->app_idx = idx;
    /* mark the proc as UPDATED so it will be included in the launch */
    PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_UPDATED);
    if (NULL == node->daemon) {
        proc->parent = PRRTE_VPID_INVALID;
    } else {
        proc->parent = node->daemon->name.vpid;
    }

    PRRTE_RETAIN(node);  /* maintain accounting on object */
    proc->node = node;
    /* if this is a debugger job, then it doesn't count against
     * available slots - otherwise, it does */
    if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        node->num_procs++;
        ++node->slots_inuse;
    }
    if (0 > (rc = prrte_pointer_array_add(node->procs, (void*)proc))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(proc);
        return NULL;
    }
    /* retain the proc struct so that we correctly track its release */
    PRRTE_RETAIN(proc);

    return proc;
}

/*
 * determine the proper starting point for the next mapping operation
 */
prrte_node_t* prrte_rmaps_base_get_starting_point(prrte_list_t *node_list,
                                                prrte_job_t *jdata)
{
    prrte_list_item_t *item, *cur_node_item;
    prrte_node_t *node, *nd1, *ndmin;
    int overload;

    /* if a bookmark exists from some prior mapping, set us to start there */
    if (NULL != jdata->bookmark) {
        cur_node_item = NULL;
        /* find this node on the list */
        for (item = prrte_list_get_first(node_list);
             item != prrte_list_get_end(node_list);
             item = prrte_list_get_next(item)) {
            node = (prrte_node_t*)item;

            if (node->index == jdata->bookmark->index) {
                cur_node_item = item;
                break;
            }
        }
        /* see if we found it - if not, just start at the beginning */
        if (NULL == cur_node_item) {
            cur_node_item = prrte_list_get_first(node_list);
        }
    } else {
        /* if no bookmark, then just start at the beginning of the list */
        cur_node_item = prrte_list_get_first(node_list);
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s Starting bookmark at node %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         ((prrte_node_t*)cur_node_item)->name));

    /* is this node fully subscribed? If so, then the first
     * proc we assign will oversubscribe it, so let's look
     * for another candidate
     */
    node = (prrte_node_t*)cur_node_item;
    ndmin = node;
    overload = ndmin->slots_inuse - ndmin->slots;
    if (node->slots_inuse >= node->slots) {
        /* work down the list - is there another node that
         * would not be oversubscribed?
         */
        if (cur_node_item != prrte_list_get_last(node_list)) {
            item = prrte_list_get_next(cur_node_item);
        } else {
            item = prrte_list_get_first(node_list);
        }
        nd1 = NULL;
        while (item != cur_node_item) {
            nd1 = (prrte_node_t*)item;
            if (nd1->slots_inuse < nd1->slots) {
                /* this node is not oversubscribed! use it! */
                cur_node_item = item;
                goto process;
            }
            /* this one was also oversubscribed, keep track of the
             * node that has the least usage - if we can't
             * find anyone who isn't fully utilized, we will
             * start with the least used node
             */
            if (overload >= (nd1->slots_inuse - nd1->slots)) {
                ndmin = nd1;
                overload = ndmin->slots_inuse - ndmin->slots;
            }
            if (item == prrte_list_get_last(node_list)) {
                item = prrte_list_get_first(node_list);
            } else {
                item= prrte_list_get_next(item);
            }
        }
        /* if we get here, then we cycled all the way around the
         * list without finding a better answer - just use the node
         * that is minimally overloaded if it is better than
         * what we already have
         */
        if (NULL != nd1 &&
            (nd1->slots_inuse - nd1->slots) < (node->slots_inuse - node->slots)) {
            cur_node_item = (prrte_list_item_t*)ndmin;
        }
    }

  process:
    PRRTE_OUTPUT_VERBOSE((5, prrte_rmaps_base_framework.framework_output,
                         "%s Starting at node %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         ((prrte_node_t*)cur_node_item)->name));

    return (prrte_node_t*)cur_node_item;
}
