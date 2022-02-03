/*
 * Copyright (c) 2009-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2017 Intel, Inc. All rights reserved.
 *
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
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
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>
#include <stdio.h>

#include "src/util/argv.h"
#include "src/class/prte_pointer_array.h"

#include "src/util/error_strings.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "rmaps_resilient.h"

static int resilient_map(prte_job_t *jdata);
static int resilient_assign(prte_job_t *jdata);

prte_rmaps_base_module_t prte_rmaps_resilient_module = {
    .map_job = resilient_map,
    .assign_locations = resilient_assign
};


/*
 * Local variable
 */
static char *prte_getline(FILE *fp);
static bool have_ftgrps=false, made_ftgrps=false;

static int construct_ftgrps(void);
static int get_ftgrp_target(prte_proc_t *proc,
                            prte_rmaps_res_ftgrp_t **target,
                            prte_node_t **nd);
static int get_new_node(prte_proc_t *proc,
                        prte_app_context_t *app,
                        prte_job_map_t *map,
                        prte_node_t **ndret);
static int map_to_ftgrps(prte_job_t *jdata);

/*
 * Loadbalance the cluster
 */
static int resilient_map(prte_job_t *jdata)
{
    prte_app_context_t *app;
    int i, j;
    int rc = PRTE_SUCCESS;
    prte_node_t *nd=NULL, *oldnode, *node, *nptr;
    prte_rmaps_res_ftgrp_t *target = NULL;
    prte_proc_t *proc;
    pmix_rank_t totprocs;
    prte_list_t node_list;
    int num_slots;
    prte_list_item_t *item;
    prte_mca_base_component_t *c = &prte_rmaps_resilient_component.super.base_version;
    bool found;

    if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        if (NULL != jdata->map->req_mapper &&
            0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
            /* a mapper has been specified, and it isn't me */
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:resilient: job %s not using resilient mapper",
                                PRTE_JOBID_PRINT(jdata->nspace));
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        if (NULL == prte_rmaps_resilient_component.fault_group_file) {
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:resilient: cannot perform initial map of job %s - no fault groups",
                                PRTE_JOBID_PRINT(jdata->nspace));
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
    } else if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_PROCS_MIGRATING)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:resilient: cannot map job %s - not in restart or migrating",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:resilient: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* have we already constructed the fault group list? */
    if (!made_ftgrps) {
        construct_ftgrps();
    }

    if (PRTE_JOB_STATE_INIT == jdata->state) {
        /* this is an initial map - let the fault group mapper
         * handle it
         */
        return map_to_ftgrps(jdata);
    }

    /*
     * NOTE: if a proc is being ADDED to an existing job, then its
     * node field will be NULL.
     */
    PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: remapping job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* cycle through all the procs in this job to find the one(s) that failed */
    for (i=0; i < jdata->procs->size; i++) {
        /* get the proc object */
        if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, i))) {
            continue;
        }
        PRTE_OUTPUT_VERBOSE((7, prte_rmaps_base_framework.framework_output,
                             "%s PROC %s STATE %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&proc->name),
                             prte_proc_state_to_str(proc->state)));
        /* is this proc to be restarted? */
        if (proc->state != PRTE_PROC_STATE_RESTART) {
            continue;
        }
        /* save the current node */
        oldnode = proc->node;
        /* point to the app */
        app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, proc->app_idx);
        if( NULL == app ) {
            PRTE_ERROR_LOG(PRTE_ERR_FAILED_TO_MAP);
            rc = PRTE_ERR_FAILED_TO_MAP;
            goto error;
        }

        if (NULL == oldnode) {
            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: proc %s is to be started",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&proc->name)));
        } else {
            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: proc %s from node %s[%s] is to be restarted",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&proc->name),
                                 (NULL == oldnode->name) ? "NULL" : oldnode->name,
                                 (NULL == oldnode->daemon) ? "--" : PRTE_VPID_PRINT(oldnode->daemon->name.rank)));
        }

        if (NULL == oldnode) {
            /* this proc was not previously running - likely it is being added
             * to the job. So place it on the node with the fewest procs to
             * balance the load
             */
            PRTE_CONSTRUCT(&node_list, prte_list_t);
            if (PRTE_SUCCESS != (rc = prte_rmaps_base_get_target_nodes(&node_list,
                                                                       &num_slots,
                                                                       app,
                                                                       jdata->map->mapping,
                                                                       false, false))) {
                PRTE_ERROR_LOG(rc);
                while (NULL != (item = prte_list_remove_first(&node_list))) {
                    PRTE_RELEASE(item);
                }
                PRTE_DESTRUCT(&node_list);
                goto error;
            }
            if (prte_list_is_empty(&node_list)) {
                /* put the proc on "hold" until resources are available */
                PRTE_DESTRUCT(&node_list);
                proc->state = PRTE_PROC_STATE_MIGRATING;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            totprocs = 1000000;
            nd = NULL;
            while (NULL != (item = prte_list_remove_first(&node_list))) {
                node = (prte_node_t*)item;
                if (node->num_procs < totprocs) {
                    nd = node;
                    totprocs = node->num_procs;
                }
                PRTE_RELEASE(item); /* maintain accounting */
            }
            PRTE_DESTRUCT(&node_list);
            /* we already checked to ensure there was at least one node,
             * so we couldn't have come out of the loop with nd=NULL
             */
            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: Placing new process on node %s[%s] (no ftgrp)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 nd->name,
                                 (NULL == nd->daemon) ? "--" : PRTE_VPID_PRINT(nd->daemon->name.rank)));
        } else {

            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: proc %s from node %s is to be restarted",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&proc->name),
                                 (NULL == proc->node) ? "NULL" : proc->node->name));

            /* if we have fault groups, use them */
            if (have_ftgrps) {
                if (PRTE_SUCCESS != (rc = get_ftgrp_target(proc, &target, &nd))) {
                    PRTE_ERROR_LOG(rc);
                    goto error;
                }
                PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: placing proc %s into fault group %d node %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proc->name), target->ftgrp, nd->name));
            } else {
                if (PRTE_SUCCESS != (rc = get_new_node(proc, app, jdata->map, &nd))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
        /* add node to map if necessary - nothing we can do here
         * but search for it
         */
        found = false;
        for (j=0; j < jdata->map->nodes->size; j++) {
            if (NULL == (nptr = (prte_node_t*)prte_pointer_array_get_item(jdata->map->nodes, j))) {
                continue;
            }
            if (nptr == nd) {
                found = true;
                break;
            }
        }
        if (!found) {
            PRTE_RETAIN(nd);
            prte_pointer_array_add(jdata->map->nodes, nd);
            PRTE_FLAG_SET(nd, PRTE_NODE_FLAG_MAPPED);
        }
        PRTE_RETAIN(nd);  /* maintain accounting on object */
        proc->node = nd;
        nd->num_procs++;
        prte_pointer_array_add(nd->procs, (void*)proc);
        /* retain the proc struct so that we correctly track its release */
        PRTE_RETAIN(proc);

        /* flag the proc state as non-launched so we'll know to launch it */
        proc->state = PRTE_PROC_STATE_INIT;

        /* update the node and local ranks so static ports can
         * be properly selected if active
         */
        prte_rmaps_base_update_local_ranks(jdata, oldnode, nd, proc);
    }

 error:
    return rc;
}

static int resilient_assign(prte_job_t *jdata)
{
    prte_mca_base_component_t *c = &prte_rmaps_resilient_component.super.base_version;

    if (NULL == jdata->map->last_mapper ||
        0 != strcasecmp(jdata->map->last_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:resilient: job %s not using resilient assign: %s",
                            PRTE_JOBID_PRINT(jdata->nspace),
                            (NULL == jdata->map->last_mapper) ? "NULL" : jdata->map->last_mapper);
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    return PRTE_ERR_NOT_IMPLEMENTED;
}

static char *prte_getline(FILE *fp)
{
    char *ret, *buff;
    char input[1024];

    ret = fgets(input, 1024, fp);
    if (NULL != ret) {
        input[strlen(input)-1] = '\0';  /* remove newline */
        buff = strdup(input);
        return buff;
    }

    return NULL;
}


static int construct_ftgrps(void)
{
    prte_rmaps_res_ftgrp_t *ftgrp;
    prte_node_t *node;
    FILE *fp;
    char *ftinput;
    int grp;
    char **nodes;
    bool found;
    int i, k;

    /* flag that we did this */
    made_ftgrps = true;

    if (NULL == prte_rmaps_resilient_component.fault_group_file) {
        /* nothing to build */
        return PRTE_SUCCESS;
    }

    /* construct it */
    PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: constructing fault groups",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    fp = fopen(prte_rmaps_resilient_component.fault_group_file, "r");
    if (NULL == fp) { /* not found */
        prte_show_help("help-rmaps-resilient.txt", "file-not-found",
                       true, prte_rmaps_resilient_component.fault_group_file);
        return PRTE_ERR_FAILED_TO_MAP;
    }

    /* build list of fault groups */
    grp = 0;
    while (NULL != (ftinput = prte_getline(fp))) {
        ftgrp = PRTE_NEW(prte_rmaps_res_ftgrp_t);
        ftgrp->ftgrp = grp++;
        nodes = prte_argv_split(ftinput, ',');
        /* find the referenced nodes */
        for (k=0; k < prte_argv_count(nodes); k++) {
            found = false;
            for (i=0; i < prte_node_pool->size && !found; i++) {
                if (NULL == (node = prte_pointer_array_get_item(prte_node_pool, i))) {
                    continue;
                }
                if (0 == strcmp(node->name, nodes[k])) {
                    PRTE_RETAIN(node);
                    PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                         "%s rmaps:resilient: adding node %s to fault group %d",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         node->name, ftgrp->ftgrp));
                    prte_pointer_array_add(&ftgrp->nodes, node);
                    found = true;
                    break;
                }
            }
        }
        prte_list_append(&prte_rmaps_resilient_component.fault_grps, &ftgrp->super);
        prte_argv_free(nodes);
        free(ftinput);
    }
    fclose(fp);

    /* flag that we have fault grps */
    have_ftgrps = true;
    return PRTE_SUCCESS;
}

static int get_ftgrp_target(prte_proc_t *proc,
                            prte_rmaps_res_ftgrp_t **tgt,
                            prte_node_t **ndret)
{
    prte_list_item_t *item;
    int k, totnodes;
    prte_node_t *node, *nd;
    prte_rmaps_res_ftgrp_t *target, *ftgrp;
    float avgload, minload;
    pmix_rank_t totprocs, lowprocs;

    /* set defaults */
    *tgt = NULL;
    *ndret = NULL;

    /* flag all the fault groups that
     * include this node so we don't reuse them
     */
    minload = 1000000.0;
    target = NULL;
    for (item = prte_list_get_first(&prte_rmaps_resilient_component.fault_grps);
         item != prte_list_get_end(&prte_rmaps_resilient_component.fault_grps);
         item = prte_list_get_next(item)) {
        ftgrp = (prte_rmaps_res_ftgrp_t*)item;
        /* see if the node is in this fault group */
        ftgrp->included = true;
        ftgrp->used = false;
        for (k=0; k < ftgrp->nodes.size; k++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(&ftgrp->nodes, k))) {
                continue;
            }
            if (NULL != proc->node && 0 == strcmp(node->name, proc->node->name)) {
                /* yes - mark it to not be included */
                PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: node %s is in fault group %d, which will be excluded",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     proc->node->name, ftgrp->ftgrp));
                ftgrp->included = false;
                break;
            }
        }
        /* if this ftgrp is not included, then skip it */
        if (!ftgrp->included) {
            continue;
        }
        /* compute the load average on this fault group */
        totprocs = 0;
        totnodes = 0;
        for (k=0; k < ftgrp->nodes.size; k++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(&ftgrp->nodes, k))) {
                continue;
            }
            totnodes++;
            totprocs += node->num_procs;
        }
        avgload = (float)totprocs / (float)totnodes;
        /* now find the lightest loaded of the included fault groups */
        if (avgload < minload) {
            minload = avgload;
            target = ftgrp;
            PRTE_OUTPUT_VERBOSE((2, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: found new min load ftgrp %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 ftgrp->ftgrp));
        }
    }

    if (NULL == target) {
        /* nothing found */
        return PRTE_ERR_NOT_FOUND;
    }

    /* if we did find a target, re-map the proc to the lightest loaded
     * node in that group
     */
    lowprocs = 1000000;
    nd = NULL;
    for (k=0; k < target->nodes.size; k++) {
        if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(&target->nodes, k))) {
            continue;
        }
        if (node->num_procs < lowprocs) {
            lowprocs = node->num_procs;
            nd = node;
        }
    }

    /* return the results */
    *tgt = target;
    *ndret = nd;

    return PRTE_SUCCESS;
}

static int get_new_node(prte_proc_t *proc,
                        prte_app_context_t *app,
                        prte_job_map_t *map,
                        prte_node_t **ndret)
{
    prte_node_t *nd, *oldnode, *node;
    prte_proc_t *pptr;
    int rc, j;
    prte_list_t node_list, candidates;
    prte_list_item_t *item, *next;
    int num_slots;
    bool found;

    /* set defaults */
    *ndret = NULL;
    nd = NULL;
    oldnode = NULL;
    prte_get_attribute(&proc->attributes, PRTE_PROC_PRIOR_NODE, (void**)&oldnode, PMIX_POINTER);

    /*
     * Get a list of all nodes
     */
    PRTE_CONSTRUCT(&node_list, prte_list_t);
    if (PRTE_SUCCESS != (rc = prte_rmaps_base_get_target_nodes(&node_list,
                                                               &num_slots,
                                                               app,
                                                               map->mapping,
                                                               false, false))) {
        PRTE_ERROR_LOG(rc);
        goto release;
    }
    if (prte_list_is_empty(&node_list)) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto release;
    }

    if (1 == prte_list_get_size(&node_list)) {
        /* if we have only one node, all we can do is put the proc on that
         * node, even if it is the same one - better than not restarting at
         * all
         */
        nd = (prte_node_t*)prte_list_get_first(&node_list);
        prte_set_attribute(&proc->attributes, PRTE_PROC_PRIOR_NODE, PRTE_ATTR_LOCAL, oldnode, PMIX_POINTER);
        PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                             "%s rmaps:resilient: Placing process %s on node %s[%s] (only one avail node)",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&proc->name),
                             nd->name,
                             (NULL == nd->daemon) ? "--" : PRTE_VPID_PRINT(nd->daemon->name.rank)));
        goto release;
    }

    /*
     * Cycle thru the list, transferring
     * all available nodes to the candidate list
     * so we can get them in the right order
     *
     */
    PRTE_CONSTRUCT(&candidates, prte_list_t);
    while (NULL != (item = prte_list_remove_first(&node_list))) {
        node = (prte_node_t*)item;
        /* don't put it back on current node */
        if (node == oldnode) {
            PRTE_RELEASE(item);
            continue;
        }
        if (0 == node->num_procs) {
            PRTE_OUTPUT_VERBOSE((7, prte_rmaps_base_framework.framework_output,
                                 "%s PREPENDING EMPTY NODE %s[%s] TO CANDIDATES",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 (NULL == node->daemon) ? "--" : PRTE_VPID_PRINT(node->daemon->name.rank)));
            prte_list_prepend(&candidates, item);
        } else {
            PRTE_OUTPUT_VERBOSE((7, prte_rmaps_base_framework.framework_output,
                                 "%s APPENDING NON-EMPTY NODE %s[%s] TO CANDIDATES",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 (NULL == node->daemon) ? "--" : PRTE_VPID_PRINT(node->daemon->name.rank)));
            prte_list_append(&candidates, item);
        }
    }
    /* search the candidates
     * try to use a semi-intelligent selection logic here that:
     *
     * (a) avoids putting the proc on a node where a peer is already
     *     located as this degrades our fault tolerance
     *
     * (b) avoids "ricochet effect" where a process would ping-pong
     *     between two nodes as it fails
     */
    nd = NULL;
    item = prte_list_get_first(&candidates);
    while (item != prte_list_get_end(&candidates)) {
        node = (prte_node_t*)item;
        next = prte_list_get_next(item);
        /* don't return to our prior location to avoid
         * "ricochet" effect
         */
        if (NULL != oldnode && node == oldnode) {
            PRTE_OUTPUT_VERBOSE((7, prte_rmaps_base_framework.framework_output,
                                 "%s REMOVING PRIOR NODE %s[%s] FROM CANDIDATES",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 (NULL == node->daemon) ? "--" : PRTE_VPID_PRINT(node->daemon->name.rank)));
            prte_list_remove_item(&candidates, item);
            PRTE_RELEASE(item);  /* maintain acctg */
            item = next;
            continue;
        }
        /* if this node is empty, then it is the winner */
        if (0 == node->num_procs) {
            nd = node;
            prte_set_attribute(&proc->attributes, PRTE_PROC_PRIOR_NODE, PRTE_ATTR_LOCAL, oldnode, PMIX_POINTER);
            break;
        }
        /* if this node has someone from my job, then skip it
         * to avoid (a)
         */
        found = false;
        for (j=0; j < node->procs->size; j++) {
            if (NULL == (pptr = (prte_proc_t*)prte_pointer_array_get_item(node->procs, j))) {
                continue;
            }
            if (PMIX_CHECK_NSPACE(pptr->name.nspace, proc->name.nspace)) {
                PRTE_OUTPUT_VERBOSE((7, prte_rmaps_base_framework.framework_output,
                                     "%s FOUND PEER %s ON NODE %s[%s]",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&pptr->name),
                                     (NULL == node->name) ? "NULL" : node->name,
                                     (NULL == node->daemon) ? "--" : PRTE_VPID_PRINT(node->daemon->name.rank)));
                found = true;
                break;
            }
        }
        if (found) {
            item = next;
            continue;
        }
        /* get here if all tests pass - take this node */
        nd = node;
        prte_set_attribute(&proc->attributes, PRTE_PROC_PRIOR_NODE, PRTE_ATTR_LOCAL, oldnode, PMIX_POINTER);
        break;
    }
    if (NULL == nd) {
        /* didn't find anything */
        if (NULL != oldnode) {
            nd = oldnode;
            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: Placing process %s on prior node %s[%s] (no ftgrp)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&proc->name),
                                 (NULL == nd->name) ? "NULL" : nd->name,
                                 (NULL == nd->daemon) ? "--" : PRTE_VPID_PRINT(nd->daemon->name.rank)));
        } else {
            nd = proc->node;
            prte_set_attribute(&proc->attributes, PRTE_PROC_PRIOR_NODE, PRTE_ATTR_LOCAL, nd, PMIX_POINTER);
            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: Placing process %s back on same node %s[%s] (no ftgrp)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&proc->name),
                                 (NULL == nd->name) ? "NULL" : nd->name,
                                 (NULL == nd->daemon) ? "--" : PRTE_VPID_PRINT(nd->daemon->name.rank)));
        }

    }
    /* cleanup candidate list */
    while (NULL != (item = prte_list_remove_first(&candidates))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&candidates);

 release:
    PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: Placing process on node %s[%s] (no ftgrp)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == nd->name) ? "NULL" : nd->name,
                         (NULL == nd->daemon) ? "--" : PRTE_VPID_PRINT(nd->daemon->name.rank)));

    while (NULL != (item = prte_list_remove_first(&node_list))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&node_list);

    *ndret = nd;
    return rc;
}

static void flag_nodes(prte_list_t *node_list)
{
    prte_list_item_t *item, *nitem;
    prte_node_t *node, *nd;
    prte_rmaps_res_ftgrp_t *ftgrp;
    int k;

    for (item = prte_list_get_first(&prte_rmaps_resilient_component.fault_grps);
         item != prte_list_get_end(&prte_rmaps_resilient_component.fault_grps);
         item = prte_list_get_next(item)) {
        ftgrp = (prte_rmaps_res_ftgrp_t*)item;
        /* reset the flags */
        ftgrp->used = false;
        ftgrp->included = false;
        /* if at least one node in our list is included in this
         * ftgrp, then flag it as included
         */
        for (nitem = prte_list_get_first(node_list);
             !ftgrp->included && nitem != prte_list_get_end(node_list);
             nitem = prte_list_get_next(nitem)) {
            node = (prte_node_t*)nitem;
            for (k=0; k < ftgrp->nodes.size; k++) {
                if (NULL == (nd = (prte_node_t*)prte_pointer_array_get_item(&ftgrp->nodes, k))) {
                    continue;
                }
                if (0 == strcmp(nd->name, node->name)) {
                    ftgrp->included = true;
                    break;
                }
            }
        }
    }
}

static int map_to_ftgrps(prte_job_t *jdata)
{
    prte_job_map_t *map;
    prte_app_context_t *app;
    int i, j, k, totnodes;
    prte_list_t node_list;
    prte_list_item_t *item, *next, *curitem;
    int num_slots;
    int rc = PRTE_SUCCESS;
    float avgload, minload;
    prte_node_t *node, *nd=NULL;
    prte_rmaps_res_ftgrp_t *ftgrp, *target = NULL;
    pmix_rank_t totprocs, num_assigned;
    bool initial_map=true;

    PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: creating initial map for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* start at the beginning... */
    jdata->num_procs = 0;
    map = jdata->map;

    for (i=0; i < jdata->apps->size; i++) {
        /* get the app_context */
        if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        /* you cannot use this mapper unless you specify the number of procs to
         * launch for each app
         */
        if (0 == app->num_procs) {
            prte_show_help("help-rmaps-resilient.txt",
                           "num-procs",
                           true);
            return PRTE_ERR_SILENT;
        }
        num_assigned = 0;
        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        PRTE_CONSTRUCT(&node_list, prte_list_t);
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                                   map->mapping, initial_map, false))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* remove all nodes that are not "up" or do not have a running daemon on them */
        item = prte_list_get_first(&node_list);
        while (item != prte_list_get_end(&node_list)) {
            next = prte_list_get_next(item);
            node = (prte_node_t*)item;
            if (PRTE_NODE_STATE_UP != node->state ||
                NULL == node->daemon ||
                PRTE_PROC_STATE_RUNNING != node->daemon->state) {
                prte_list_remove_item(&node_list, item);
                PRTE_RELEASE(item);
            }
            item = next;
        }
        curitem = prte_list_get_first(&node_list);

        /* flag the fault groups included by these nodes */
        flag_nodes(&node_list);
        /* map each copy to a different fault group - if more copies are
         * specified than fault groups, then overlap in a round-robin fashion
         */
        for (j=0; j < app->num_procs; j++) {
            /* find unused included fault group with lowest average load - if none
             * found, then break
             */
            target = NULL;
            minload = 1000000000.0;
            for (item = prte_list_get_first(&prte_rmaps_resilient_component.fault_grps);
                 item != prte_list_get_end(&prte_rmaps_resilient_component.fault_grps);
                 item = prte_list_get_next(item)) {
                ftgrp = (prte_rmaps_res_ftgrp_t*)item;
                PRTE_OUTPUT_VERBOSE((2, prte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: fault group %d used: %s included %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     ftgrp->ftgrp,
                                     ftgrp->used ? "YES" : "NO",
                                     ftgrp->included ? "YES" : "NO" ));
                /* if this ftgrp has already been used or is not included, then
                 * skip it
                 */
                if (ftgrp->used || !ftgrp->included) {
                    continue;
                }
                /* compute the load average on this fault group */
                totprocs = 0;
                totnodes = 0;
                for (k=0; k < ftgrp->nodes.size; k++) {
                    if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(&ftgrp->nodes, k))) {
                        continue;
                    }
                    totnodes++;
                    totprocs += node->num_procs;
                }
                avgload = (float)totprocs / (float)totnodes;
                if (avgload < minload) {
                    minload = avgload;
                    target = ftgrp;
                    PRTE_OUTPUT_VERBOSE((2, prte_rmaps_base_framework.framework_output,
                                         "%s rmaps:resilient: found new min load ftgrp %d",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         ftgrp->ftgrp));
                }
            }
            /* if we have more procs than fault groups, then we simply
             * map the remaining procs on available nodes in a round-robin
             * fashion - it doesn't matter where they go as they will not
             * be contributing to fault tolerance by definition
             */
            if (NULL == target) {
                PRTE_OUTPUT_VERBOSE((2, prte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: more procs than fault groups - mapping excess rr",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                nd = (prte_node_t*)curitem;
                curitem = prte_list_get_next(curitem);
                if (curitem == prte_list_get_end(&node_list)) {
                    curitem = prte_list_get_first(&node_list);
                }
            } else {
                /* pick node with lowest load from within that group */
                totprocs = 1000000;
                for (k=0; k < target->nodes.size; k++) {
                    if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(&target->nodes, k))) {
                        continue;
                    }
                    if (node->num_procs < totprocs) {
                        totprocs = node->num_procs;
                        nd = node;
                    }
                }
            }
            PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: placing proc into fault group %d node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (NULL == target) ? -1 : target->ftgrp, nd->name));
            /* if the node isn't in the map, add it */
            if (!PRTE_FLAG_TEST(nd, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_RETAIN(nd);
                prte_pointer_array_add(map->nodes, nd);
                PRTE_FLAG_SET(nd, PRTE_NODE_FLAG_MAPPED);
            }
            if (NULL == prte_rmaps_base_setup_proc(jdata, nd, app->idx)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
                return PRTE_ERROR;
            }
            if ((nd->slots < (int)nd->num_procs) ||
                (0 < nd->slots_max && nd->slots_max < (int)nd->num_procs)) {
                if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                    prte_show_help("help-orte-rmaps-base.txt", "alloc-error",
                                   true, nd->num_procs, app->app);
                    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                    return PRTE_ERR_SILENT;
                }
                /* flag the node as oversubscribed so that sched-yield gets
                 * properly set
                 */
                PRTE_FLAG_SET(nd, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
            }

            /* track number of procs mapped */
            num_assigned++;

            /* flag this fault group as used */
            if (NULL != target) {
                target->used = true;
            }
        }

        /* track number of procs */
        jdata->num_procs += app->num_procs;

        /* cleanup the node list - it can differ from one app_context
         * to another, so we have to get it every time
         */
        while (NULL != (item = prte_list_remove_first(&node_list))) {
            PRTE_RELEASE(item);
        }
        PRTE_DESTRUCT(&node_list);
    }

    return PRTE_SUCCESS;
}
