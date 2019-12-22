/*
 * Copyright (c) 2007-2011 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
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

#include <stddef.h>

#include "src/dss/dss.h"
#include "src/class/prrte_hash_table.h"
#include "src/class/prrte_bitmap.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/runtime.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/rml/base/rml_contact.h"

#include "src/mca/routed/base/base.h"
#include "routed_radix.h"


static int init(void);
static int finalize(void);
static int delete_route(prrte_process_name_t *proc);
static int update_route(prrte_process_name_t *target,
                        prrte_process_name_t *route);
static prrte_process_name_t get_route(prrte_process_name_t *target);
static int route_lost(const prrte_process_name_t *route);
static bool route_is_defined(const prrte_process_name_t *target);
static void update_routing_plan(void);
static void get_routing_list(prrte_list_t *coll);
static int set_lifeline(prrte_process_name_t *proc);
static size_t num_routes(void);

prrte_routed_module_t prrte_routed_radix_module = {
    .initialize = init,
    .finalize = finalize,
    .delete_route = delete_route,
    .update_route = update_route,
    .get_route = get_route,
    .route_lost = route_lost,
    .route_is_defined = route_is_defined,
    .set_lifeline = set_lifeline,
    .update_routing_plan = update_routing_plan,
    .get_routing_list = get_routing_list,
    .num_routes = num_routes,
};

/* local globals */
static prrte_process_name_t      *lifeline=NULL;
static prrte_process_name_t      local_lifeline;
static int                      num_children;
static prrte_list_t              my_children;
static bool                     hnp_direct=true;

static int init(void)
{
    lifeline = NULL;

    if (PRRTE_PROC_IS_DAEMON) {
        /* if we are using static ports, set my lifeline to point at my parent */
        if (prrte_static_ports) {
            lifeline = PRRTE_PROC_MY_PARENT;
        } else {
            /* set our lifeline to the HNP - we will abort if that connection is lost */
            lifeline = PRRTE_PROC_MY_HNP;
        }
        PRRTE_PROC_MY_PARENT->jobid = PRRTE_PROC_MY_NAME->jobid;
    }

    /* setup the list of children */
    PRRTE_CONSTRUCT(&my_children, prrte_list_t);
    num_children = 0;

    return PRRTE_SUCCESS;
}

static int finalize(void)
{
    prrte_list_item_t *item;

    lifeline = NULL;

    /* deconstruct the list of children */
    while (NULL != (item = prrte_list_remove_first(&my_children))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&my_children);
    num_children = 0;

    return PRRTE_SUCCESS;
}

static int delete_route(prrte_process_name_t *proc)
{
    if (proc->jobid == PRRTE_JOBID_INVALID ||
        proc->vpid == PRRTE_VPID_INVALID) {
        return PRRTE_ERR_BAD_PARAM;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_radix_delete_route for %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc)));


    /* THIS CAME FROM OUR OWN JOB FAMILY...there is nothing
     * to do here. The routes will be redefined when we update
     * the routing tree
     */

    return PRRTE_SUCCESS;
}

static int update_route(prrte_process_name_t *target,
                        prrte_process_name_t *route)
{
    if (target->jobid == PRRTE_JOBID_INVALID ||
        target->vpid == PRRTE_VPID_INVALID) {
        return PRRTE_ERR_BAD_PARAM;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_radix_update: %s --> %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(target),
                         PRRTE_NAME_PRINT(route)));


    /* if I am a daemon and the target is my HNP, then check
     * the route - if it isn't direct, then we just flag that
     * we have a route to the HNP
     */
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_HNP, target) &&
        PRRTE_EQUAL != prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_HNP, route)) {
        hnp_direct = false;
        return PRRTE_SUCCESS;
    }

    return PRRTE_SUCCESS;
}


static prrte_process_name_t get_route(prrte_process_name_t *target)
{
    prrte_process_name_t *ret, daemon;
    prrte_list_item_t *item;
    prrte_routed_tree_t *child;

    if (!prrte_routing_is_enabled) {
        ret = target;
        goto found;
    }

    /* initialize */
    daemon.jobid = PRRTE_PROC_MY_DAEMON->jobid;
    daemon.vpid = PRRTE_PROC_MY_DAEMON->vpid;

    if (target->jobid == PRRTE_JOBID_INVALID ||
        target->vpid == PRRTE_VPID_INVALID) {
        ret = PRRTE_NAME_INVALID;
        goto found;
    }

    /* if it is me, then the route is just direct */
    if (PRRTE_EQUAL == prrte_dss.compare(PRRTE_PROC_MY_NAME, target, PRRTE_NAME)) {
        ret = target;
        goto found;
    }

    /* if this is going to the HNP, then send it direct if we don't know
     * how to get there - otherwise, send it via the tree
     */
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_HNP, target)) {
        if (!hnp_direct || prrte_static_ports) {
            PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                                 "%s routing to the HNP through my parent %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_PARENT)));
            ret = PRRTE_PROC_MY_PARENT;
            goto found;
        } else {
            PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                                 "%s routing direct to the HNP",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            ret = PRRTE_PROC_MY_HNP;
            goto found;
        }
    }

    /* if the target is our parent, then send it direct */
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_PARENT, target)) {
        ret = PRRTE_PROC_MY_PARENT;
        goto found;
    }

    daemon.jobid = PRRTE_PROC_MY_NAME->jobid;
    /* find out what daemon hosts this proc */
    if (PRRTE_PROC_MY_NAME->jobid == target->jobid) {
        /* it's a daemon - no need to look it up */
        daemon.vpid = target->vpid;
    } else {
        if (PRRTE_VPID_INVALID == (daemon.vpid = prrte_get_proc_daemon_vpid(target))) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            ret = PRRTE_NAME_INVALID;
            goto found;
        }
    }

    /* if the daemon is me, then send direct to the target! */
    if (PRRTE_PROC_MY_NAME->vpid == daemon.vpid) {
        ret = target;
        goto found;
    } else {
        /* search routing tree for next step to that daemon */
        for (item = prrte_list_get_first(&my_children);
             item != prrte_list_get_end(&my_children);
             item = prrte_list_get_next(item)) {
            child = (prrte_routed_tree_t*)item;
            if (child->vpid == daemon.vpid) {
                /* the child is hosting the proc - just send it there */
                ret = &daemon;
                goto found;
            }
            /* otherwise, see if the daemon we need is below the child */
            if (prrte_bitmap_is_set_bit(&child->relatives, daemon.vpid)) {
                /* yep - we need to step through this child */
                daemon.vpid = child->vpid;
                ret = &daemon;
                goto found;
            }
        }
    }

    /* if we get here, then the target daemon is not beneath
     * any of our children, so we have to step up through our parent
     */
    daemon.vpid = PRRTE_PROC_MY_PARENT->vpid;

    ret = &daemon;

found:
    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_radix_get(%s) --> %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(target),
                         PRRTE_NAME_PRINT(ret)));

    return *ret;
}

static int route_lost(const prrte_process_name_t *route)
{
    prrte_list_item_t *item;
    prrte_routed_tree_t *child;

    PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                         "%s route to %s lost",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(route)));

    /* if we lose the connection to the lifeline and we are NOT already,
     * in finalize, tell the OOB to abort.
     * NOTE: we cannot call abort from here as the OOB needs to first
     * release a thread-lock - otherwise, we will hang!!
     */
    if (!prrte_finalizing &&
        NULL != lifeline &&
        PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, route, lifeline)) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                             "%s routed:radix: Connection to lifeline %s lost",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(lifeline)));
        return PRRTE_ERR_FATAL;
    }

    /* if the route is a daemon,
     * see if it is one of our children - if so, remove it
     */
    if (route->jobid == PRRTE_PROC_MY_NAME->jobid) {
        for (item = prrte_list_get_first(&my_children);
             item != prrte_list_get_end(&my_children);
             item = prrte_list_get_next(item)) {
            child = (prrte_routed_tree_t*)item;
            if (child->vpid == route->vpid) {
                prrte_list_remove_item(&my_children, item);
                PRRTE_RELEASE(item);
                return PRRTE_SUCCESS;
            }
        }
    }

    /* we don't care about this one, so return success */
    return PRRTE_SUCCESS;
}

static bool route_is_defined(const prrte_process_name_t *target)
{
    /* find out what daemon hosts this proc */
    if (PRRTE_VPID_INVALID == prrte_get_proc_daemon_vpid((prrte_process_name_t*)target)) {
        return false;
    }

    return true;
}

static int set_lifeline(prrte_process_name_t *proc)
{
    /* we have to copy the proc data because there is no
     * guarantee that it will be preserved
     */
    local_lifeline.jobid = proc->jobid;
    local_lifeline.vpid = proc->vpid;
    lifeline = &local_lifeline;

    return PRRTE_SUCCESS;
}

static void radix_tree(int rank, int *num_children,
                       prrte_list_t *children, prrte_bitmap_t *relatives)
{
    int i, peer, Sum, NInLevel;
    prrte_routed_tree_t *child;
    prrte_bitmap_t *relations;

    /* compute how many procs are at my level */
    Sum=1;
    NInLevel=1;

    while ( Sum < (rank+1) ) {
        NInLevel *= prrte_routed_radix_component.radix;
        Sum += NInLevel;
    }

    /* our children start at our rank + num_in_level */
    peer = rank + NInLevel;
    for (i = 0; i < prrte_routed_radix_component.radix; i++) {
        if (peer < (int)prrte_process_info.num_procs) {
            child = PRRTE_NEW(prrte_routed_tree_t);
            child->vpid = peer;
            if (NULL != children) {
                /* this is a direct child - add it to my list */
                prrte_list_append(children, &child->super);
                (*num_children)++;
                /* setup the relatives bitmap */
                prrte_bitmap_init(&child->relatives, prrte_process_info.num_procs);
                /* point to the relatives */
                relations = &child->relatives;
            } else {
                /* we are recording someone's relatives - set the bit */
                if (PRRTE_SUCCESS != prrte_bitmap_set_bit(relatives, peer)) {
                    prrte_output(0, "%s Error: could not set relations bit!", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                }
                /* point to this relations */
                relations = relatives;
                PRRTE_RELEASE(child);
            }
            /* search for this child's relatives */
            radix_tree(peer, NULL, NULL, relations);
        }
        peer += NInLevel;
    }
}

static void update_routing_plan(void)
{
    prrte_routed_tree_t *child;
    int j;
    prrte_list_item_t *item;
    int Level,Sum,NInLevel,Ii;
    int NInPrevLevel;

    /* clear the list of children if any are already present */
    while (NULL != (item = prrte_list_remove_first(&my_children))) {
        PRRTE_RELEASE(item);
    }
    num_children = 0;

    /* compute my parent */
    Ii =  PRRTE_PROC_MY_NAME->vpid;
    Level=0;
    Sum=1;
    NInLevel=1;

    while ( Sum < (Ii+1) ) {
        Level++;
        NInLevel *= prrte_routed_radix_component.radix;
        Sum += NInLevel;
    }
    Sum -= NInLevel;

    NInPrevLevel = NInLevel/prrte_routed_radix_component.radix;

    if( 0 == Ii ) {
        PRRTE_PROC_MY_PARENT->vpid = -1;
    }  else {
        PRRTE_PROC_MY_PARENT->vpid = (Ii-Sum) % NInPrevLevel;
        PRRTE_PROC_MY_PARENT->vpid += (Sum - NInPrevLevel);
    }

    /* compute my direct children and the bitmap that shows which vpids
     * lie underneath their branch
     */
    radix_tree(Ii, &num_children, &my_children, NULL);

    if (0 < prrte_output_get_verbosity(prrte_routed_base_framework.framework_output)) {
        prrte_output(0, "%s: parent %d num_children %d", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_PROC_MY_PARENT->vpid, num_children);
        for (item = prrte_list_get_first(&my_children);
             item != prrte_list_get_end(&my_children);
             item = prrte_list_get_next(item)) {
            child = (prrte_routed_tree_t*)item;
            prrte_output(0, "%s: \tchild %d", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), child->vpid);
            for (j=0; j < (int)prrte_process_info.num_procs; j++) {
                if (prrte_bitmap_is_set_bit(&child->relatives, j)) {
                    prrte_output(0, "%s: \t\trelation %d", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j);
                }
            }
        }
    }
}

static void get_routing_list(prrte_list_t *coll)
{
    prrte_routed_base_xcast_routing(coll, &my_children);
}

static size_t num_routes(void)
{
    return prrte_list_get_size(&my_children);
}
