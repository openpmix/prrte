/*
 * Copyright (c) 2009-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
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
#include <stdio.h>

#include "src/util/argv.h"
#include "src/class/prrte_pointer_array.h"

#include "src/util/error_strings.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
#include "rmaps_resilient.h"

static int prrte_rmaps_resilient_map(prrte_job_t *jdata);
static int resilient_assign(prrte_job_t *jdata);

prrte_rmaps_base_module_t prrte_rmaps_resilient_module = {
    .map_job = prrte_rmaps_resilient_map,
    .assign_locations = resilient_assign
};


/*
 * Local variable
 */
static char *prrte_getline(FILE *fp);
static bool have_ftgrps=false, made_ftgrps=false;

static int construct_ftgrps(void);
static int get_ftgrp_target(prrte_proc_t *proc,
                            prrte_rmaps_res_ftgrp_t **target,
                            prrte_node_t **nd);
static int get_new_node(prrte_proc_t *proc,
                        prrte_app_context_t *app,
                        prrte_job_map_t *map,
                        prrte_node_t **ndret);
static int map_to_ftgrps(prrte_job_t *jdata);

/*
 * Loadbalance the cluster
 */
static int prrte_rmaps_resilient_map(prrte_job_t *jdata)
{
    prrte_app_context_t *app;
    int i, j;
    int rc = PRRTE_SUCCESS;
    prrte_node_t *nd=NULL, *oldnode, *node, *nptr;
    prrte_rmaps_res_ftgrp_t *target = NULL;
    prrte_proc_t *proc;
    prrte_vpid_t totprocs;
    prrte_list_t node_list;
    prrte_std_cntr_t num_slots;
    prrte_list_item_t *item;
    prrte_mca_base_component_t *c = &prrte_rmaps_resilient_component.super.base_version;
    bool found;

    if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        if (NULL != jdata->map->req_mapper &&
            0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
            /* a mapper has been specified, and it isn't me */
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:resilient: job %s not using resilient mapper",
                                PRRTE_JOBID_PRINT(jdata->jobid));
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
        if (NULL == prrte_rmaps_resilient_component.fault_group_file) {
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:resilient: cannot perform initial map of job %s - no fault groups",
                                PRRTE_JOBID_PRINT(jdata->jobid));
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    } else if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_PROCS_MIGRATING)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:resilient: cannot map job %s - not in restart or migrating",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:resilient: mapping job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* have we already constructed the fault group list? */
    if (!made_ftgrps) {
        construct_ftgrps();
    }

    if (PRRTE_JOB_STATE_INIT == jdata->state) {
        /* this is an initial map - let the fault group mapper
         * handle it
         */
        return map_to_ftgrps(jdata);
    }

    /*
     * NOTE: if a proc is being ADDED to an existing job, then its
     * node field will be NULL.
     */
    PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: remapping job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    /* cycle through all the procs in this job to find the one(s) that failed */
    for (i=0; i < jdata->procs->size; i++) {
        /* get the proc object */
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, i))) {
            continue;
        }
        PRRTE_OUTPUT_VERBOSE((7, prrte_rmaps_base_framework.framework_output,
                             "%s PROC %s STATE %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name),
                             prrte_proc_state_to_str(proc->state)));
        /* is this proc to be restarted? */
        if (proc->state != PRRTE_PROC_STATE_RESTART) {
            continue;
        }
        /* save the current node */
        oldnode = proc->node;
        /* point to the app */
        app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, proc->app_idx);
        if( NULL == app ) {
            PRRTE_ERROR_LOG(PRRTE_ERR_FAILED_TO_MAP);
            rc = PRRTE_ERR_FAILED_TO_MAP;
            goto error;
        }

        if (NULL == oldnode) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: proc %s is to be started",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name)));
        } else {
            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: proc %s from node %s[%s] is to be restarted",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name),
                                 (NULL == oldnode->name) ? "NULL" : oldnode->name,
                                 (NULL == oldnode->daemon) ? "--" : PRRTE_VPID_PRINT(oldnode->daemon->name.vpid)));
        }

        if (NULL == oldnode) {
            /* this proc was not previously running - likely it is being added
             * to the job. So place it on the node with the fewest procs to
             * balance the load
             */
            PRRTE_CONSTRUCT(&node_list, prrte_list_t);
            if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_get_target_nodes(&node_list,
                                                                       &num_slots,
                                                                       app,
                                                                       jdata->map->mapping,
                                                                       false, false))) {
                PRRTE_ERROR_LOG(rc);
                while (NULL != (item = prrte_list_remove_first(&node_list))) {
                    PRRTE_RELEASE(item);
                }
                PRRTE_DESTRUCT(&node_list);
                goto error;
            }
            if (prrte_list_is_empty(&node_list)) {
                /* put the proc on "hold" until resources are available */
                PRRTE_DESTRUCT(&node_list);
                proc->state = PRRTE_PROC_STATE_MIGRATING;
                rc = PRRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            totprocs = 1000000;
            nd = NULL;
            while (NULL != (item = prrte_list_remove_first(&node_list))) {
                node = (prrte_node_t*)item;
                if (node->num_procs < totprocs) {
                    nd = node;
                    totprocs = node->num_procs;
                }
                PRRTE_RELEASE(item); /* maintain accounting */
            }
            PRRTE_DESTRUCT(&node_list);
            /* we already checked to ensure there was at least one node,
             * so we couldn't have come out of the loop with nd=NULL
             */
            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: Placing new process on node %s[%s] (no ftgrp)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 nd->name,
                                 (NULL == nd->daemon) ? "--" : PRRTE_VPID_PRINT(nd->daemon->name.vpid)));
        } else {

            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: proc %s from node %s is to be restarted",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name),
                                 (NULL == proc->node) ? "NULL" : proc->node->name));

            /* if we have fault groups, use them */
            if (have_ftgrps) {
                if (PRRTE_SUCCESS != (rc = get_ftgrp_target(proc, &target, &nd))) {
                    PRRTE_ERROR_LOG(rc);
                    goto error;
                }
                PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: placing proc %s into fault group %d node %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&proc->name), target->ftgrp, nd->name));
            } else {
                if (PRRTE_SUCCESS != (rc = get_new_node(proc, app, jdata->map, &nd))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
        /* add node to map if necessary - nothing we can do here
         * but search for it
         */
        found = false;
        for (j=0; j < jdata->map->nodes->size; j++) {
            if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, j))) {
                continue;
            }
            if (nptr == nd) {
                found = true;
                break;
            }
        }
        if (!found) {
            PRRTE_RETAIN(nd);
            prrte_pointer_array_add(jdata->map->nodes, nd);
            PRRTE_FLAG_SET(nd, PRRTE_NODE_FLAG_MAPPED);
        }
        PRRTE_RETAIN(nd);  /* maintain accounting on object */
        proc->node = nd;
        nd->num_procs++;
        prrte_pointer_array_add(nd->procs, (void*)proc);
        /* retain the proc struct so that we correctly track its release */
        PRRTE_RETAIN(proc);

        /* flag the proc state as non-launched so we'll know to launch it */
        proc->state = PRRTE_PROC_STATE_INIT;

        /* update the node and local ranks so static ports can
         * be properly selected if active
         */
        prrte_rmaps_base_update_local_ranks(jdata, oldnode, nd, proc);
    }

 error:
    return rc;
}

static int resilient_assign(prrte_job_t *jdata)
{
    prrte_mca_base_component_t *c = &prrte_rmaps_resilient_component.super.base_version;

    if (NULL == jdata->map->last_mapper ||
        0 != strcasecmp(jdata->map->last_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:resilient: job %s not using resilient assign: %s",
                            PRRTE_JOBID_PRINT(jdata->jobid),
                            (NULL == jdata->map->last_mapper) ? "NULL" : jdata->map->last_mapper);
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    return PRRTE_ERR_NOT_IMPLEMENTED;
}

static char *prrte_getline(FILE *fp)
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
    prrte_rmaps_res_ftgrp_t *ftgrp;
    prrte_node_t *node;
    FILE *fp;
    char *ftinput;
    int grp;
    char **nodes;
    bool found;
    int i, k;

    /* flag that we did this */
    made_ftgrps = true;

    if (NULL == prrte_rmaps_resilient_component.fault_group_file) {
        /* nothing to build */
        return PRRTE_SUCCESS;
    }

    /* construct it */
    PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: constructing fault groups",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
    fp = fopen(prrte_rmaps_resilient_component.fault_group_file, "r");
    if (NULL == fp) { /* not found */
        prrte_show_help("help-prrte-rmaps-resilient.txt", "prrte-rmaps-resilient:file-not-found",
                       true, prrte_rmaps_resilient_component.fault_group_file);
        return PRRTE_ERR_FAILED_TO_MAP;
    }

    /* build list of fault groups */
    grp = 0;
    while (NULL != (ftinput = prrte_getline(fp))) {
        ftgrp = PRRTE_NEW(prrte_rmaps_res_ftgrp_t);
        ftgrp->ftgrp = grp++;
        nodes = prrte_argv_split(ftinput, ',');
        /* find the referenced nodes */
        for (k=0; k < prrte_argv_count(nodes); k++) {
            found = false;
            for (i=0; i < prrte_node_pool->size && !found; i++) {
                if (NULL == (node = prrte_pointer_array_get_item(prrte_node_pool, i))) {
                    continue;
                }
                if (0 == strcmp(node->name, nodes[k])) {
                    PRRTE_RETAIN(node);
                    PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                         "%s rmaps:resilient: adding node %s to fault group %d",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         node->name, ftgrp->ftgrp));
                    prrte_pointer_array_add(&ftgrp->nodes, node);
                    found = true;
                    break;
                }
            }
        }
        prrte_list_append(&prrte_rmaps_resilient_component.fault_grps, &ftgrp->super);
        prrte_argv_free(nodes);
        free(ftinput);
    }
    fclose(fp);

    /* flag that we have fault grps */
    have_ftgrps = true;
    return PRRTE_SUCCESS;
}

static int get_ftgrp_target(prrte_proc_t *proc,
                            prrte_rmaps_res_ftgrp_t **tgt,
                            prrte_node_t **ndret)
{
    prrte_list_item_t *item;
    int k, totnodes;
    prrte_node_t *node, *nd;
    prrte_rmaps_res_ftgrp_t *target, *ftgrp;
    float avgload, minload;
    prrte_vpid_t totprocs, lowprocs;

    /* set defaults */
    *tgt = NULL;
    *ndret = NULL;

    /* flag all the fault groups that
     * include this node so we don't reuse them
     */
    minload = 1000000.0;
    target = NULL;
    for (item = prrte_list_get_first(&prrte_rmaps_resilient_component.fault_grps);
         item != prrte_list_get_end(&prrte_rmaps_resilient_component.fault_grps);
         item = prrte_list_get_next(item)) {
        ftgrp = (prrte_rmaps_res_ftgrp_t*)item;
        /* see if the node is in this fault group */
        ftgrp->included = true;
        ftgrp->used = false;
        for (k=0; k < ftgrp->nodes.size; k++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(&ftgrp->nodes, k))) {
                continue;
            }
            if (NULL != proc->node && 0 == strcmp(node->name, proc->node->name)) {
                /* yes - mark it to not be included */
                PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: node %s is in fault group %d, which will be excluded",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
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
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(&ftgrp->nodes, k))) {
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
            PRRTE_OUTPUT_VERBOSE((2, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: found new min load ftgrp %d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 ftgrp->ftgrp));
        }
    }

    if (NULL == target) {
        /* nothing found */
        return PRRTE_ERR_NOT_FOUND;
    }

    /* if we did find a target, re-map the proc to the lightest loaded
     * node in that group
     */
    lowprocs = 1000000;
    nd = NULL;
    for (k=0; k < target->nodes.size; k++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(&target->nodes, k))) {
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

    return PRRTE_SUCCESS;
}

static int get_new_node(prrte_proc_t *proc,
                        prrte_app_context_t *app,
                        prrte_job_map_t *map,
                        prrte_node_t **ndret)
{
    prrte_node_t *nd, *oldnode, *node;
    prrte_proc_t *pptr;
    int rc, j;
    prrte_list_t node_list, candidates;
    prrte_list_item_t *item, *next;
    prrte_std_cntr_t num_slots;
    bool found;

    /* set defaults */
    *ndret = NULL;
    nd = NULL;
    oldnode = NULL;
    prrte_get_attribute(&proc->attributes, PRRTE_PROC_PRIOR_NODE, (void**)&oldnode, PRRTE_PTR);

    /*
     * Get a list of all nodes
     */
    PRRTE_CONSTRUCT(&node_list, prrte_list_t);
    if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_get_target_nodes(&node_list,
                                                               &num_slots,
                                                               app,
                                                               map->mapping,
                                                               false, false))) {
        PRRTE_ERROR_LOG(rc);
        goto release;
    }
    if (prrte_list_is_empty(&node_list)) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
        goto release;
    }

    if (1 == prrte_list_get_size(&node_list)) {
        /* if we have only one node, all we can do is put the proc on that
         * node, even if it is the same one - better than not restarting at
         * all
         */
        nd = (prrte_node_t*)prrte_list_get_first(&node_list);
        prrte_set_attribute(&proc->attributes, PRRTE_PROC_PRIOR_NODE, PRRTE_ATTR_LOCAL, oldnode, PRRTE_PTR);
        PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                             "%s rmaps:resilient: Placing process %s on node %s[%s] (only one avail node)",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name),
                             nd->name,
                             (NULL == nd->daemon) ? "--" : PRRTE_VPID_PRINT(nd->daemon->name.vpid)));
        goto release;
    }

    /*
     * Cycle thru the list, transferring
     * all available nodes to the candidate list
     * so we can get them in the right order
     *
     */
    PRRTE_CONSTRUCT(&candidates, prrte_list_t);
    while (NULL != (item = prrte_list_remove_first(&node_list))) {
        node = (prrte_node_t*)item;
        /* don't put it back on current node */
        if (node == oldnode) {
            PRRTE_RELEASE(item);
            continue;
        }
        if (0 == node->num_procs) {
            PRRTE_OUTPUT_VERBOSE((7, prrte_rmaps_base_framework.framework_output,
                                 "%s PREPENDING EMPTY NODE %s[%s] TO CANDIDATES",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 (NULL == node->daemon) ? "--" : PRRTE_VPID_PRINT(node->daemon->name.vpid)));
            prrte_list_prepend(&candidates, item);
        } else {
            PRRTE_OUTPUT_VERBOSE((7, prrte_rmaps_base_framework.framework_output,
                                 "%s APPENDING NON-EMPTY NODE %s[%s] TO CANDIDATES",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 (NULL == node->daemon) ? "--" : PRRTE_VPID_PRINT(node->daemon->name.vpid)));
            prrte_list_append(&candidates, item);
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
    item = prrte_list_get_first(&candidates);
    while (item != prrte_list_get_end(&candidates)) {
        node = (prrte_node_t*)item;
        next = prrte_list_get_next(item);
        /* don't return to our prior location to avoid
         * "ricochet" effect
         */
        if (NULL != oldnode && node == oldnode) {
            PRRTE_OUTPUT_VERBOSE((7, prrte_rmaps_base_framework.framework_output,
                                 "%s REMOVING PRIOR NODE %s[%s] FROM CANDIDATES",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 (NULL == node->daemon) ? "--" : PRRTE_VPID_PRINT(node->daemon->name.vpid)));
            prrte_list_remove_item(&candidates, item);
            PRRTE_RELEASE(item);  /* maintain acctg */
            item = next;
            continue;
        }
        /* if this node is empty, then it is the winner */
        if (0 == node->num_procs) {
            nd = node;
            prrte_set_attribute(&proc->attributes, PRRTE_PROC_PRIOR_NODE, PRRTE_ATTR_LOCAL, oldnode, PRRTE_PTR);
            break;
        }
        /* if this node has someone from my job, then skip it
         * to avoid (a)
         */
        found = false;
        for (j=0; j < node->procs->size; j++) {
            if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                continue;
            }
            if (pptr->name.jobid == proc->name.jobid) {
                PRRTE_OUTPUT_VERBOSE((7, prrte_rmaps_base_framework.framework_output,
                                     "%s FOUND PEER %s ON NODE %s[%s]",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&pptr->name),
                                     (NULL == node->name) ? "NULL" : node->name,
                                     (NULL == node->daemon) ? "--" : PRRTE_VPID_PRINT(node->daemon->name.vpid)));
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
        prrte_set_attribute(&proc->attributes, PRRTE_PROC_PRIOR_NODE, PRRTE_ATTR_LOCAL, oldnode, PRRTE_PTR);
        break;
    }
    if (NULL == nd) {
        /* didn't find anything */
        if (NULL != oldnode) {
            nd = oldnode;
            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: Placing process %s on prior node %s[%s] (no ftgrp)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name),
                                 (NULL == nd->name) ? "NULL" : nd->name,
                                 (NULL == nd->daemon) ? "--" : PRRTE_VPID_PRINT(nd->daemon->name.vpid)));
        } else {
            nd = proc->node;
            prrte_set_attribute(&proc->attributes, PRRTE_PROC_PRIOR_NODE, PRRTE_ATTR_LOCAL, nd, PRRTE_PTR);
            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: Placing process %s back on same node %s[%s] (no ftgrp)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name),
                                 (NULL == nd->name) ? "NULL" : nd->name,
                                 (NULL == nd->daemon) ? "--" : PRRTE_VPID_PRINT(nd->daemon->name.vpid)));
        }

    }
    /* cleanup candidate list */
    while (NULL != (item = prrte_list_remove_first(&candidates))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&candidates);

 release:
    PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: Placing process on node %s[%s] (no ftgrp)",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (NULL == nd->name) ? "NULL" : nd->name,
                         (NULL == nd->daemon) ? "--" : PRRTE_VPID_PRINT(nd->daemon->name.vpid)));

    while (NULL != (item = prrte_list_remove_first(&node_list))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&node_list);

    *ndret = nd;
    return rc;
}

static void flag_nodes(prrte_list_t *node_list)
{
    prrte_list_item_t *item, *nitem;
    prrte_node_t *node, *nd;
    prrte_rmaps_res_ftgrp_t *ftgrp;
    int k;

    for (item = prrte_list_get_first(&prrte_rmaps_resilient_component.fault_grps);
         item != prrte_list_get_end(&prrte_rmaps_resilient_component.fault_grps);
         item = prrte_list_get_next(item)) {
        ftgrp = (prrte_rmaps_res_ftgrp_t*)item;
        /* reset the flags */
        ftgrp->used = false;
        ftgrp->included = false;
        /* if at least one node in our list is included in this
         * ftgrp, then flag it as included
         */
        for (nitem = prrte_list_get_first(node_list);
             !ftgrp->included && nitem != prrte_list_get_end(node_list);
             nitem = prrte_list_get_next(nitem)) {
            node = (prrte_node_t*)nitem;
            for (k=0; k < ftgrp->nodes.size; k++) {
                if (NULL == (nd = (prrte_node_t*)prrte_pointer_array_get_item(&ftgrp->nodes, k))) {
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

static int map_to_ftgrps(prrte_job_t *jdata)
{
    prrte_job_map_t *map;
    prrte_app_context_t *app;
    int i, j, k, totnodes;
    prrte_list_t node_list;
    prrte_list_item_t *item, *next, *curitem;
    prrte_std_cntr_t num_slots;
    int rc = PRRTE_SUCCESS;
    float avgload, minload;
    prrte_node_t *node, *nd=NULL;
    prrte_rmaps_res_ftgrp_t *ftgrp, *target = NULL;
    prrte_vpid_t totprocs, num_assigned;
    bool initial_map=true;

    PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:resilient: creating initial map for job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    /* start at the beginning... */
    jdata->num_procs = 0;
    map = jdata->map;

    for (i=0; i < jdata->apps->size; i++) {
        /* get the app_context */
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        /* you cannot use this mapper unless you specify the number of procs to
         * launch for each app
         */
        if (0 == app->num_procs) {
            prrte_show_help("help-prrte-rmaps-resilient.txt",
                           "prrte-rmaps-resilient:num-procs",
                           true);
            return PRRTE_ERR_SILENT;
        }
        num_assigned = 0;
        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        PRRTE_CONSTRUCT(&node_list, prrte_list_t);
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                                   map->mapping, initial_map, false))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* remove all nodes that are not "up" or do not have a running daemon on them */
        item = prrte_list_get_first(&node_list);
        while (item != prrte_list_get_end(&node_list)) {
            next = prrte_list_get_next(item);
            node = (prrte_node_t*)item;
            if (PRRTE_NODE_STATE_UP != node->state ||
                NULL == node->daemon ||
                PRRTE_PROC_STATE_RUNNING != node->daemon->state) {
                prrte_list_remove_item(&node_list, item);
                PRRTE_RELEASE(item);
            }
            item = next;
        }
        curitem = prrte_list_get_first(&node_list);

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
            for (item = prrte_list_get_first(&prrte_rmaps_resilient_component.fault_grps);
                 item != prrte_list_get_end(&prrte_rmaps_resilient_component.fault_grps);
                 item = prrte_list_get_next(item)) {
                ftgrp = (prrte_rmaps_res_ftgrp_t*)item;
                PRRTE_OUTPUT_VERBOSE((2, prrte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: fault group %d used: %s included %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
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
                    if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(&ftgrp->nodes, k))) {
                        continue;
                    }
                    totnodes++;
                    totprocs += node->num_procs;
                }
                avgload = (float)totprocs / (float)totnodes;
                if (avgload < minload) {
                    minload = avgload;
                    target = ftgrp;
                    PRRTE_OUTPUT_VERBOSE((2, prrte_rmaps_base_framework.framework_output,
                                         "%s rmaps:resilient: found new min load ftgrp %d",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         ftgrp->ftgrp));
                }
            }
            /* if we have more procs than fault groups, then we simply
             * map the remaining procs on available nodes in a round-robin
             * fashion - it doesn't matter where they go as they will not
             * be contributing to fault tolerance by definition
             */
            if (NULL == target) {
                PRRTE_OUTPUT_VERBOSE((2, prrte_rmaps_base_framework.framework_output,
                                     "%s rmaps:resilient: more procs than fault groups - mapping excess rr",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                nd = (prrte_node_t*)curitem;
                curitem = prrte_list_get_next(curitem);
                if (curitem == prrte_list_get_end(&node_list)) {
                    curitem = prrte_list_get_first(&node_list);
                }
            } else {
                /* pick node with lowest load from within that group */
                totprocs = 1000000;
                for (k=0; k < target->nodes.size; k++) {
                    if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(&target->nodes, k))) {
                        continue;
                    }
                    if (node->num_procs < totprocs) {
                        totprocs = node->num_procs;
                        nd = node;
                    }
                }
            }
            PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                                 "%s rmaps:resilient: placing proc into fault group %d node %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == target) ? -1 : target->ftgrp, nd->name));
            /* if the node isn't in the map, add it */
            if (!PRRTE_FLAG_TEST(nd, PRRTE_NODE_FLAG_MAPPED)) {
                PRRTE_RETAIN(nd);
                prrte_pointer_array_add(map->nodes, nd);
                PRRTE_FLAG_SET(nd, PRRTE_NODE_FLAG_MAPPED);
            }
            if (NULL == prrte_rmaps_base_setup_proc(jdata, nd, app->idx)) {
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                return PRRTE_ERROR;
            }
            if ((nd->slots < (int)nd->num_procs) ||
                (0 < nd->slots_max && nd->slots_max < (int)nd->num_procs)) {
                if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                   true, nd->num_procs, app->app);
                    PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                    return PRRTE_ERR_SILENT;
                }
                /* flag the node as oversubscribed so that sched-yield gets
                 * properly set
                 */
                PRRTE_FLAG_SET(nd, PRRTE_NODE_FLAG_OVERSUBSCRIBED);
                PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_OVERSUBSCRIBED);
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
        while (NULL != (item = prrte_list_remove_first(&node_list))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&node_list);
    }

    return PRRTE_SUCCESS;
}
