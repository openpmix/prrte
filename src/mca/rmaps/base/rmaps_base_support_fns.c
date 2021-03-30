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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/threads/tsd.h"
#include "src/util/argv.h"
#include "src/util/if.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "types.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

int prte_rmaps_base_filter_nodes(prte_app_context_t *app, prte_list_t *nodes, bool remove)
{
    int rc = PRTE_ERR_TAKE_NEXT_OPTION;
    char *hosts;

    /* did the app_context contain a hostfile? */
    if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts, PMIX_STRING)) {
        /* yes - filter the node list through the file, removing
         * any nodes not found in the file
         */
        if (PRTE_SUCCESS != (rc = prte_util_filter_hostfile_nodes(nodes, hosts, remove))) {
            PRTE_ERROR_LOG(rc);
            free(hosts);
            return rc;
        }
        /** check that anything is here */
        if (0 == prte_list_get_size(nodes)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-mapped-node", true,
                           app->app, "-hostfile", hosts);
            free(hosts);
            return PRTE_ERR_SILENT;
        }
        free(hosts);
    }
    /* did the app_context contain an add-hostfile? */
    if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE, (void **) &hosts,
                           PMIX_STRING)) {
        /* yes - filter the node list through the file, removing
         * any nodes not found in the file
         */
        if (PRTE_SUCCESS != (rc = prte_util_filter_hostfile_nodes(nodes, hosts, remove))) {
            free(hosts);
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /** check that anything is here */
        if (0 == prte_list_get_size(nodes)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-mapped-node", true,
                           app->app, "-add-hostfile", hosts);
            free(hosts);
            return PRTE_ERR_SILENT;
        }
        free(hosts);
    }
    /* now filter the list through any -host specification */
    if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts, PMIX_STRING)) {
        if (PRTE_SUCCESS != (rc = prte_util_filter_dash_host_nodes(nodes, hosts, remove))) {
            PRTE_ERROR_LOG(rc);
            free(hosts);
            return rc;
        }
        /** check that anything is left! */
        if (0 == prte_list_get_size(nodes)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-mapped-node", true,
                           app->app, "-host", hosts);
            free(hosts);
            return PRTE_ERR_SILENT;
        }
        free(hosts);
    }
    /* now filter the list through any add-host specification */
    if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOST, (void **) &hosts, PMIX_STRING)) {
        if (PRTE_SUCCESS != (rc = prte_util_filter_dash_host_nodes(nodes, hosts, remove))) {
            PRTE_ERROR_LOG(rc);
            free(hosts);
            return rc;
        }
        /** check that anything is left! */
        if (0 == prte_list_get_size(nodes)) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-mapped-node", true,
                           app->app, "-add-host", hosts);
            free(hosts);
            return PRTE_ERR_SILENT;
        }
        free(hosts);
    }

    return rc;
}

/*
 * Query the registry for all nodes allocated to a specified app_context
 */
int prte_rmaps_base_get_target_nodes(prte_list_t *allocated_nodes, int32_t *total_num_slots,
                                     prte_app_context_t *app, prte_mapping_policy_t policy,
                                     bool initial_map, bool silent)
{
    prte_list_item_t *item;
    prte_node_t *node, *nd, *nptr, *next;
    int32_t num_slots;
    int32_t i;
    int rc;
    prte_job_t *daemons;
    bool novm;
    prte_list_t nodes;
    char *hosts = NULL;

    /** set default answer */
    *total_num_slots = 0;

    /* get the daemon job object */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    /* see if we have a vm or not */
    novm = prte_get_attribute(&daemons->attributes, PRTE_JOB_NO_VM, NULL, PMIX_BOOL);

    /* if this is NOT a managed allocation, then we use the nodes
     * that were specified for this app - there is no need to collect
     * all available nodes and "filter" them.
     *
     * However, if it is a managed allocation AND the hostfile or the hostlist was
     * provided, those take precedence, so process them and filter as we normally do.
     */
    if (!prte_managed_allocation
        || (prte_managed_allocation
            && (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts,
                                   PMIX_STRING)
                || prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts,
                                      PMIX_STRING)))) {
        PRTE_CONSTRUCT(&nodes, prte_list_t);
        /* if the app provided a dash-host, then use those nodes */
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts,
                               PMIX_STRING)) {
            PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                 "%s using dash_host %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 hosts));
            if (PRTE_SUCCESS != (rc = prte_util_add_dash_host_nodes(&nodes, hosts, false))) {
                PRTE_ERROR_LOG(rc);
                free(hosts);
                return rc;
            }
            free(hosts);
        } else if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts,
                                      PMIX_STRING)) {
            /* otherwise, if the app provided a hostfile, then use that */
            PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                 "%s using hostfile %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 hosts));
            if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&nodes, hosts))) {
                free(hosts);
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            free(hosts);
        } else {
            /* if nothing else was specified by the app, then use all known nodes, which
             * will include ourselves
             */
            PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                 "%s using known nodes", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            goto addknown;
        }
        /** if we still don't have anything */
        if (0 == prte_list_get_size(&nodes)) {
            if (!silent) {
                prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-available-resources",
                               true);
            }
            PRTE_DESTRUCT(&nodes);
            return PRTE_ERR_SILENT;
        }
        /* find the nodes in our node array and assemble them
         * in list order as that is what the user specified. Note
         * that the prte_node_t objects on the nodes list are not
         * fully filled in - they only contain the user-provided
         * name of the node as a temp object. Thus, we cannot just
         * check to see if the node pointer matches that of a node
         * in the node_pool.
         */
        PRTE_LIST_FOREACH_SAFE(nptr, next, &nodes, prte_node_t)
        {
            for (i = 0; i < prte_node_pool->size; i++) {
                if (NULL
                    == (node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, i))) {
                    continue;
                }
                /* ignore nodes that are non-usable */
                if (PRTE_FLAG_TEST(node, PRTE_NODE_NON_USABLE)) {
                    continue;
                }
                /* ignore nodes that are marked as do-not-use for this mapping */
                if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                    PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_USE", node->name));
                    /* reset the state so it can be used another time */
                    node->state = PRTE_NODE_STATE_UP;
                    continue;
                }
                if (PRTE_NODE_STATE_DOWN == node->state) {
                    PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                         "NODE %s IS DOWN", node->name));
                    continue;
                }
                if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                    PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                         "NODE %s IS MARKED NO_INCLUDE", node->name));
                    /* not to be used */
                    continue;
                }
                /* if this node wasn't included in the vm (e.g., by -host), ignore it,
                 * unless we are mapping prior to launching the vm
                 */
                if (NULL == node->daemon && !novm) {
                    PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                         "NODE %s HAS NO DAEMON", node->name));
                    continue;
                }
                if (!prte_node_match(node, nptr->name)) {
                    PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                         "NODE %s DOESNT MATCH NODE %s", node->name, nptr->name));
                    continue;
                }
                /* retain a copy for our use in case the item gets
                 * destructed along the way
                 */
                PRTE_RETAIN(node);
                if (initial_map) {
                    /* if this is the first app_context we
                     * are getting for an initial map of a job,
                     * then mark all nodes as unmapped
                     */
                    PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
                }
                /* the list is ordered as per user direction using -host
                 * or the listing in -hostfile - preserve that ordering */
                prte_list_append(allocated_nodes, &node->super);
                break;
            }
            /* remove the item from the list as we have allocated it */
            prte_list_remove_item(&nodes, (prte_list_item_t *) nptr);
            PRTE_RELEASE(nptr);
        }
        PRTE_DESTRUCT(&nodes);
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
    if (0 == prte_list_get_size(allocated_nodes)) {
        /* the list is empty - if the HNP is allocated, then add it */
        if (prte_hnp_is_allocated) {
            nd = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, 0);
            if (!PRTE_FLAG_TEST(nd, PRTE_NODE_NON_USABLE)) {
                PRTE_RETAIN(nd);
                prte_list_append(allocated_nodes, &nd->super);
            } else {
                nd = NULL;
            }
        } else {
            nd = NULL;
        }
    } else {
        nd = (prte_node_t *) prte_list_get_last(allocated_nodes);
    }
    for (i = 1; i < prte_node_pool->size; i++) {
        if (NULL != (node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, i))) {
            /* ignore nodes that are non-usable */
            if (PRTE_FLAG_TEST(node, PRTE_NODE_NON_USABLE)) {
                continue;
            }
            /* ignore nodes that are marked as do-not-use for this mapping */
            if (PRTE_NODE_STATE_DO_NOT_USE == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_USE", node->name));
                /* reset the state so it can be used another time */
                node->state = PRTE_NODE_STATE_UP;
                continue;
            }
            if (PRTE_NODE_STATE_DOWN == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                     "NODE %s IS MARKED DOWN", node->name));
                continue;
            }
            if (PRTE_NODE_STATE_NOT_INCLUDED == node->state) {
                PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                     "NODE %s IS MARKED NO_INCLUDE", node->name));
                /* not to be used */
                continue;
            }
            /* if this node wasn't included in the vm (e.g., by -host), ignore it,
             * unless we are mapping prior to launching the vm
             */
            if (NULL == node->daemon && !novm) {
                PRTE_OUTPUT_VERBOSE((10, prte_rmaps_base_framework.framework_output,
                                     "NODE %s HAS NO DAEMON", node->name));
                continue;
            }
            /* retain a copy for our use in case the item gets
             * destructed along the way
             */
            PRTE_RETAIN(node);
            if (initial_map) {
                /* if this is the first app_context we
                 * are getting for an initial map of a job,
                 * then mark all nodes as unmapped
                 */
                PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
            }
            if (NULL == nd || NULL == nd->daemon || NULL == node->daemon
                || nd->daemon->name.rank < node->daemon->name.rank) {
                /* just append to end */
                prte_list_append(allocated_nodes, &node->super);
                nd = node;
            } else {
                /* starting from end, put this node in daemon-vpid order */
                while (node->daemon->name.rank < nd->daemon->name.rank) {
                    if (prte_list_get_begin(allocated_nodes) == prte_list_get_prev(&nd->super)) {
                        /* insert at beginning */
                        prte_list_prepend(allocated_nodes, &node->super);
                        goto moveon;
                    }
                    nd = (prte_node_t *) prte_list_get_prev(&nd->super);
                }
                item = prte_list_get_next(&nd->super);
                if (item == prte_list_get_end(allocated_nodes)) {
                    /* we are at the end - just append */
                    prte_list_append(allocated_nodes, &node->super);
                } else {
                    nd = (prte_node_t *) item;
                    prte_list_insert_pos(allocated_nodes, item, &node->super);
                }
            moveon:
                /* reset us back to the end for the next node */
                nd = (prte_node_t *) prte_list_get_last(allocated_nodes);
            }
        }
    }

    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                         "%s Starting with %d nodes in list", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) prte_list_get_size(allocated_nodes)));

    /** check that anything is here */
    if (0 == prte_list_get_size(allocated_nodes)) {
        if (!silent) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-available-resources",
                           true);
        }
        return PRTE_ERR_SILENT;
    }

    /* filter the nodes thru any hostfile and dash-host options */
    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output, "%s Filtering thru apps",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (PRTE_SUCCESS != (rc = prte_rmaps_base_filter_nodes(app, allocated_nodes, true))
        && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                         "%s Retained %d nodes in list", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) prte_list_get_size(allocated_nodes)));

complete:
    num_slots = 0;
    /* remove all nodes that are already at max usage, and
     * compute the total number of allocated slots while
     * we do so - can ignore this if we are mapping debugger
     * daemons as they do not count against the allocation */
    if (PRTE_MAPPING_DEBUGGER & PRTE_GET_MAPPING_DIRECTIVE(policy)) {
        num_slots = prte_list_get_size(
            allocated_nodes); // tell the mapper there is one slot/node for debuggers
    } else {
        PRTE_LIST_FOREACH_SAFE(node, next, allocated_nodes, prte_node_t)
        {
            /* if the hnp was not allocated, or flagged not to be used,
             * then remove it here */
            if (!prte_hnp_is_allocated
                || (PRTE_GET_MAPPING_DIRECTIVE(policy) & PRTE_MAPPING_NO_USE_LOCAL)) {
                if (0 == node->index) {
                    prte_list_remove_item(allocated_nodes, &node->super);
                    PRTE_RELEASE(node); /* "un-retain" it */
                    continue;
                }
            }
            /** check to see if this node is fully used - remove if so */
            if (0 != node->slots_max && node->slots_inuse > node->slots_max) {
                PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                     "%s Removing node %s: max %d inuse %d",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name,
                                     node->slots_max, node->slots_inuse));
                prte_list_remove_item(allocated_nodes, &node->super);
                PRTE_RELEASE(node); /* "un-retain" it */
                continue;
            }
            if (node->slots <= node->slots_inuse
                && (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(policy))) {
                /* remove the node as fully used */
                PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                     "%s Removing node %s slots %d inuse %d",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, node->slots,
                                     node->slots_inuse));
                prte_list_remove_item(allocated_nodes, &node->super);
                PRTE_RELEASE(node); /* "un-retain" it */
                continue;
            }
            if (node->slots > node->slots_inuse) {
                int32_t s;
                /* check for any -host allocations */
                if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts,
                                       PMIX_STRING)) {
                    s = prte_util_dash_host_compute_slots(node, hosts);
                } else {
                    s = node->slots - node->slots_inuse;
                }
                node->slots_available = s;
                /* add the available slots */
                PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                     "%s node %s has %d slots available",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, s));
                num_slots += s;
                continue;
            }
            if (!(PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(policy))) {
                /* nothing needed to do here - we don't add slots to the
                 * count as we don't have any available. Just let the mapper
                 * do what it needs to do to meet the request
                 */
                PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                                     "%s node %s is fully used, but available for oversubscription",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
            } else {
                /* if we cannot use it, remove it from list */
                prte_list_remove_item(allocated_nodes, &node->super);
                PRTE_RELEASE(node); /* "un-retain" it */
            }
        }
    }

    /* Sanity check to make sure we have resources available */
    if (0 == prte_list_get_size(allocated_nodes)) {
        if (silent) {
            /* let the caller know that the resources exist,
             * but are currently busy
             */
            return PRTE_ERR_RESOURCE_BUSY;
        } else {
            prte_show_help("help-prte-rmaps-base.txt",
                           "prte-rmaps-base:all-available-resources-used", true);
            return PRTE_ERR_SILENT;
        }
    }

    /* pass back the total number of available slots */
    *total_num_slots = num_slots;

    if (4 < prte_output_get_verbosity(prte_rmaps_base_framework.framework_output)) {
        prte_output(0, "AVAILABLE NODES FOR MAPPING:");
        for (item = prte_list_get_first(allocated_nodes);
             item != prte_list_get_end(allocated_nodes); item = prte_list_get_next(item)) {
            node = (prte_node_t *) item;
            prte_output(0, "    node: %s daemon: %s slots_available: %d", node->name,
                        (NULL == node->daemon) ? "NULL" : PRTE_VPID_PRINT(node->daemon->name.rank),
                        node->slots_available);
        }
    }

    return PRTE_SUCCESS;
}

prte_proc_t *prte_rmaps_base_setup_proc(prte_job_t *jdata, prte_node_t *node, prte_app_idx_t idx)
{
    prte_proc_t *proc;
    int rc;

    proc = PRTE_NEW(prte_proc_t);
    /* set the jobid */
    PMIX_LOAD_NSPACE(proc->name.nspace, jdata->nspace);
    proc->job = jdata;
    /* flag the proc as ready for launch */
    proc->state = PRTE_PROC_STATE_INIT;
    proc->app_idx = idx;
    /* mark the proc as UPDATED so it will be included in the launch */
    PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_UPDATED);
    if (NULL == node->daemon) {
        proc->parent = PMIX_RANK_INVALID;
    } else {
        proc->parent = node->daemon->name.rank;
    }

    PRTE_RETAIN(node); /* maintain accounting on object */
    proc->node = node;
    /* if this is a debugger job, then it doesn't count against
     * available slots - otherwise, it does */
    if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON)
        && !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
        node->num_procs++;
        ++node->slots_inuse;
    }
    if (0 > (rc = prte_pointer_array_add(node->procs, (void *) proc))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(proc);
        return NULL;
    }
    /* retain the proc struct so that we correctly track its release */
    PRTE_RETAIN(proc);

    return proc;
}

/*
 * determine the proper starting point for the next mapping operation
 */
prte_node_t *prte_rmaps_base_get_starting_point(prte_list_t *node_list, prte_job_t *jdata)
{
    prte_list_item_t *item, *cur_node_item;
    prte_node_t *node, *nd1, *ndmin;
    int overload;

    /* if a bookmark exists from some prior mapping, set us to start there */
    if (NULL != jdata->bookmark) {
        cur_node_item = NULL;
        /* find this node on the list */
        for (item = prte_list_get_first(node_list); item != prte_list_get_end(node_list);
             item = prte_list_get_next(item)) {
            node = (prte_node_t *) item;

            if (node->index == jdata->bookmark->index) {
                cur_node_item = item;
                break;
            }
        }
        /* see if we found it - if not, just start at the beginning */
        if (NULL == cur_node_item) {
            cur_node_item = prte_list_get_first(node_list);
        }
    } else {
        /* if no bookmark, then just start at the beginning of the list */
        cur_node_item = prte_list_get_first(node_list);
    }

    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output,
                         "%s Starting bookmark at node %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         ((prte_node_t *) cur_node_item)->name));

    /* is this node fully subscribed? If so, then the first
     * proc we assign will oversubscribe it, so let's look
     * for another candidate
     */
    node = (prte_node_t *) cur_node_item;
    ndmin = node;
    overload = ndmin->slots_inuse - ndmin->slots;
    if (node->slots_inuse >= node->slots) {
        /* work down the list - is there another node that
         * would not be oversubscribed?
         */
        if (cur_node_item != prte_list_get_last(node_list)) {
            item = prte_list_get_next(cur_node_item);
        } else {
            item = prte_list_get_first(node_list);
        }
        nd1 = NULL;
        while (item != cur_node_item) {
            nd1 = (prte_node_t *) item;
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
            if (item == prte_list_get_last(node_list)) {
                item = prte_list_get_first(node_list);
            } else {
                item = prte_list_get_next(item);
            }
        }
        /* if we get here, then we cycled all the way around the
         * list without finding a better answer - just use the node
         * that is minimally overloaded if it is better than
         * what we already have
         */
        if (NULL != nd1 && (nd1->slots_inuse - nd1->slots) < (node->slots_inuse - node->slots)) {
            cur_node_item = (prte_list_item_t *) ndmin;
        }
    }

process:
    PRTE_OUTPUT_VERBOSE((5, prte_rmaps_base_framework.framework_output, "%s Starting at node %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         ((prte_node_t *) cur_node_item)->name));

    return (prte_node_t *) cur_node_item;
}
