/*
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
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
#include "src/class/prrte_pointer_array.h"
#include "src/class/prrte_bitmap.h"
#include "src/util/bit_ops.h"
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
#include "routed_binomial.h"

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

prrte_routed_module_t prrte_routed_binomial_module = {
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

    /* if we are using static ports, set my lifeline to point at my parent */
    if (prrte_static_ports) {
        lifeline = PRRTE_PROC_MY_PARENT;
    } else {
        /* set our lifeline to the HNP - we will abort if that connection is lost */
        lifeline = PRRTE_PROC_MY_HNP;
    }
    PRRTE_PROC_MY_PARENT->jobid = PRRTE_PROC_MY_NAME->jobid;

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
                         "%s routed_binomial_delete_route for %s",
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
                         "%s routed_binomial_update: %s --> %s",
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


    daemon.jobid = PRRTE_PROC_MY_NAME->jobid;
    /* find out what daemon hosts this proc */
    if (PRRTE_VPID_INVALID == (daemon.vpid = prrte_get_proc_daemon_vpid(target))) {
        /*PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);*/
        ret = PRRTE_NAME_INVALID;
        goto found;
    }

    /* if the daemon is me, then send direct to the target! */
    if (PRRTE_PROC_MY_NAME->vpid == daemon.vpid) {
        ret = target;
        goto found;
    }

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

    /* if we get here, then the target daemon is not beneath
     * any of our children, so we have to step up through our parent
     */
    daemon.vpid = PRRTE_PROC_MY_PARENT->vpid;

    ret = &daemon;

 found:
    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_binomial_get(%s) --> %s",
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
                             "%s routed:binomial: Connection to lifeline %s lost",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(lifeline)));
        return PRRTE_ERR_FATAL;
    }

    /* is it a daemon, and one of my children? if so, then
     * remove it from the child list
     */
    if (route->jobid == PRRTE_PROC_MY_NAME->jobid) {
        for (item = prrte_list_get_first(&my_children);
             item != prrte_list_get_end(&my_children);
             item = prrte_list_get_next(item)) {
            child = (prrte_routed_tree_t*)item;
            if (child->vpid == route->vpid) {
                PRRTE_OUTPUT_VERBOSE((4, prrte_routed_base_framework.framework_output,
                                     "%s routed_binomial: removing route to child daemon %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(route)));
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

static int binomial_tree(int rank, int parent, int me, int num_procs,
                         int *nchildren, prrte_list_t *childrn,
                         prrte_bitmap_t *relatives, bool mine)
{
    int i, bitmap, peer, hibit, mask, found;
    prrte_routed_tree_t *child;
    prrte_bitmap_t *relations;

    PRRTE_OUTPUT_VERBOSE((3, prrte_routed_base_framework.framework_output,
                         "%s routed:binomial rank %d parent %d me %d num_procs %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         rank, parent, me, num_procs));

    /* is this me? */
    if (me == rank) {
        bitmap = prrte_cube_dim(num_procs);

        hibit = prrte_hibit(rank, bitmap);
        --bitmap;

        for (i = hibit + 1, mask = 1 << i; i <= bitmap; ++i, mask <<= 1) {
            peer = rank | mask;
            if (peer < num_procs) {
                child = PRRTE_NEW(prrte_routed_tree_t);
                child->vpid = peer;
                PRRTE_OUTPUT_VERBOSE((3, prrte_routed_base_framework.framework_output,
                                     "%s routed:binomial %d found child %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     rank,
                                     PRRTE_VPID_PRINT(child->vpid)));

                if (mine) {
                    /* this is a direct child - add it to my list */
                    prrte_list_append(childrn, &child->super);
                    (*nchildren)++;
                    /* setup the relatives bitmap */
                    prrte_bitmap_init(&child->relatives, num_procs);

                    /* point to the relatives */
                    relations = &child->relatives;
                } else {
                    /* we are recording someone's relatives - set the bit */
                    prrte_bitmap_set_bit(relatives, peer);
                    /* point to this relations */
                    relations = relatives;
                }
                /* search for this child's relatives */
                binomial_tree(0, 0, peer, num_procs, nchildren, childrn, relations, false);
            }
        }
        return parent;
    }

    /* find the children of this rank */
    PRRTE_OUTPUT_VERBOSE((5, prrte_routed_base_framework.framework_output,
                         "%s routed:binomial find children of rank %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), rank));
    bitmap = prrte_cube_dim(num_procs);

    hibit = prrte_hibit(rank, bitmap);
    --bitmap;

    for (i = hibit + 1, mask = 1 << i; i <= bitmap; ++i, mask <<= 1) {
        peer = rank | mask;
        PRRTE_OUTPUT_VERBOSE((5, prrte_routed_base_framework.framework_output,
                             "%s routed:binomial find children checking peer %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), peer));
        if (peer < num_procs) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_routed_base_framework.framework_output,
                                 "%s routed:binomial find children computing tree",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            /* execute compute on this child */
            if (0 <= (found = binomial_tree(peer, rank, me, num_procs, nchildren, childrn, relatives, mine))) {
                PRRTE_OUTPUT_VERBOSE((5, prrte_routed_base_framework.framework_output,
                                     "%s routed:binomial find children returning found value %d",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), found));
                return found;
            }
        }
    }
    return -1;
}

static void update_routing_plan(void)
{
    prrte_routed_tree_t *child;
    int j;
    prrte_list_item_t *item;

    /* clear the list of children if any are already present */
    while (NULL != (item = prrte_list_remove_first(&my_children))) {
        PRRTE_RELEASE(item);
    }
    num_children = 0;

    /* compute my direct children and the bitmap that shows which vpids
     * lie underneath their branch
     */
    PRRTE_PROC_MY_PARENT->vpid = binomial_tree(0, 0, PRRTE_PROC_MY_NAME->vpid,
                                   prrte_process_info.max_procs,
                                   &num_children, &my_children, NULL, true);

    if (0 < prrte_output_get_verbosity(prrte_routed_base_framework.framework_output)) {
        prrte_output(0, "%s: parent %d num_children %d", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_PROC_MY_PARENT->vpid, num_children);
        for (item = prrte_list_get_first(&my_children);
             item != prrte_list_get_end(&my_children);
             item = prrte_list_get_next(item)) {
            child = (prrte_routed_tree_t*)item;
            prrte_output(0, "%s: \tchild %d", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), child->vpid);
            for (j=0; j < (int)prrte_process_info.max_procs; j++) {
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
    PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                         "%s num routes %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)prrte_list_get_size(&my_children)));
    return prrte_list_get_size(&my_children);
}
