/*
 * Copyright (c) 2007-2011 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stddef.h>

#include "src/class/prte_bitmap.h"
#include "src/class/prte_hash_table.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/runtime/runtime.h"
#include "src/util/name_fns.h"

#include "src/mca/rml/base/rml_contact.h"

#include "routed_radix.h"
#include "src/mca/routed/base/base.h"

static int init(void);
static int finalize(void);
static int delete_route(pmix_proc_t *proc);
static int update_route(pmix_proc_t *target, pmix_proc_t *route);
static pmix_proc_t get_route(pmix_proc_t *target);
static int route_lost(const pmix_proc_t *route);
static bool route_is_defined(const pmix_proc_t *target);
static void update_routing_plan(void);
static void get_routing_list(prte_list_t *coll);
static int set_lifeline(pmix_proc_t *proc);
static size_t num_routes(void);

prte_routed_module_t prte_routed_radix_module = {
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
static pmix_proc_t *lifeline = NULL;
static pmix_proc_t local_lifeline;
static int num_children;
static prte_list_t my_children;
static bool hnp_direct = true;

static int init(void)
{
    lifeline = NULL;

    if (PRTE_PROC_IS_DAEMON) {
        /* if we are using static ports, set my lifeline to point at my parent */
        if (prte_static_ports) {
            lifeline = PRTE_PROC_MY_PARENT;
        } else {
            /* set our lifeline to the HNP - we will abort if that connection is lost */
            lifeline = PRTE_PROC_MY_HNP;
        }
        PMIX_LOAD_NSPACE(PRTE_PROC_MY_PARENT->nspace, PRTE_PROC_MY_NAME->nspace);
    }

    /* setup the list of children */
    PRTE_CONSTRUCT(&my_children, prte_list_t);
    num_children = 0;

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    prte_list_item_t *item;

    lifeline = NULL;

    /* deconstruct the list of children */
    while (NULL != (item = prte_list_remove_first(&my_children))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&my_children);
    num_children = 0;

    return PRTE_SUCCESS;
}

static int delete_route(pmix_proc_t *proc)
{
    if (PMIX_PROCID_INVALID(proc)) {
        return PRTE_ERR_BAD_PARAM;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_routed_base_framework.framework_output,
                         "%s routed_radix_delete_route for %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(proc)));

    /* THIS CAME FROM OUR OWN JOB FAMILY...there is nothing
     * to do here. The routes will be redefined when we update
     * the routing tree
     */

    return PRTE_SUCCESS;
}

static int update_route(pmix_proc_t *target, pmix_proc_t *route)
{
    if (PMIX_PROCID_INVALID(target)) {
        return PRTE_ERR_BAD_PARAM;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_routed_base_framework.framework_output,
                         "%s routed_radix_update: %s --> %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(target), PRTE_NAME_PRINT(route)));

    /* if I am a daemon and the target is my HNP, then check
     * the route - if it isn't direct, then we just flag that
     * we have a route to the HNP
     */
    if (PMIX_CHECK_PROCID(PRTE_PROC_MY_HNP, target) && PMIX_CHECK_PROCID(PRTE_PROC_MY_HNP, route)) {
        hnp_direct = false;
        return PRTE_SUCCESS;
    }

    return PRTE_SUCCESS;
}

static pmix_proc_t get_route(pmix_proc_t *target)
{
    pmix_proc_t *ret, daemon;
    prte_list_item_t *item;
    prte_routed_tree_t *child;

    if (!prte_routing_is_enabled) {
        ret = target;
        goto found;
    }

    if (PMIX_PROCID_INVALID(target)) {
        ret = PRTE_NAME_INVALID;
        goto found;
    }

    /* if it is me, then the route is just direct */
    if (PMIX_CHECK_PROCID(PRTE_PROC_MY_NAME, target)) {
        ret = target;
        goto found;
    }

    /* if this is going to the HNP, then send it direct if we don't know
     * how to get there - otherwise, send it via the tree
     */
    if (PMIX_CHECK_PROCID(PRTE_PROC_MY_HNP, target)) {
        if (!hnp_direct || prte_static_ports) {
            PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                                 "%s routing to the HNP through my parent %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));
            ret = PRTE_PROC_MY_PARENT;
            goto found;
        } else {
            PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                                 "%s routing direct to the HNP",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            ret = PRTE_PROC_MY_HNP;
            goto found;
        }
    }

    /* if the target is our parent, then send it direct */
    if (PMIX_CHECK_PROCID(PRTE_PROC_MY_PARENT, target)) {
        ret = PRTE_PROC_MY_PARENT;
        goto found;
    }

    PMIX_LOAD_NSPACE(daemon.nspace, PRTE_PROC_MY_NAME->nspace);
    /* find out what daemon hosts this proc */
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, target->nspace)) {
        /* it's a daemon - no need to look it up */
        daemon.rank = target->rank;
    } else {
        if (PMIX_RANK_INVALID == (daemon.rank = prte_get_proc_daemon_vpid(target))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            ret = PRTE_NAME_INVALID;
            goto found;
        }
    }

    /* if the daemon is me, then send direct to the target! */
    if (PRTE_PROC_MY_NAME->rank == daemon.rank) {
        ret = target;
        goto found;
    } else {
        /* search routing tree for next step to that daemon */
        for (item = prte_list_get_first(&my_children); item != prte_list_get_end(&my_children);
             item = prte_list_get_next(item)) {
            child = (prte_routed_tree_t *) item;
            if (child->rank == daemon.rank) {
                /* the child is hosting the proc - just send it there */
                ret = &daemon;
                goto found;
            }
            /* otherwise, see if the daemon we need is below the child */
            if (prte_bitmap_is_set_bit(&child->relatives, daemon.rank)) {
                /* yep - we need to step through this child */
                daemon.rank = child->rank;
                ret = &daemon;
                goto found;
            }
        }
    }

    /* if we get here, then the target daemon is not beneath
     * any of our children, so we have to step up through our parent
     */
    daemon.rank = PRTE_PROC_MY_PARENT->rank;

    ret = &daemon;

found:
    PRTE_OUTPUT_VERBOSE((1, prte_routed_base_framework.framework_output,
                         "%s routed_radix_get(%s) --> %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(target), PRTE_NAME_PRINT(ret)));

    return *ret;
}

static int route_lost(const pmix_proc_t *route)
{
    prte_list_item_t *item;
    prte_routed_tree_t *child;

    PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output, "%s route to %s lost",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(route)));

    /* if we lose the connection to the lifeline and we are NOT already,
     * in finalize, tell the OOB to abort.
     * NOTE: we cannot call abort from here as the OOB needs to first
     * release a thread-lock - otherwise, we will hang!!
     */
    if (!prte_finalizing && NULL != lifeline
        && PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, route, lifeline)) {
        PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                             "%s routed:radix: Connection to lifeline %s lost",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(lifeline)));
        return PRTE_ERR_FATAL;
    }

    /* if the route is a daemon,
     * see if it is one of our children - if so, remove it
     */
    if (PMIX_CHECK_NSPACE(route->nspace, PRTE_PROC_MY_NAME->nspace)) {
        for (item = prte_list_get_first(&my_children); item != prte_list_get_end(&my_children);
             item = prte_list_get_next(item)) {
            child = (prte_routed_tree_t *) item;
            if (child->rank == route->rank) {
                prte_list_remove_item(&my_children, item);
                PRTE_RELEASE(item);
                return PRTE_SUCCESS;
            }
        }
    }

    /* we don't care about this one, so return success */
    return PRTE_SUCCESS;
}

static bool route_is_defined(const pmix_proc_t *target)
{
    /* find out what daemon hosts this proc */
    if (PMIX_RANK_INVALID == prte_get_proc_daemon_vpid((pmix_proc_t *) target)) {
        return false;
    }

    return true;
}

static int set_lifeline(pmix_proc_t *proc)
{
    /* we have to copy the proc data because there is no
     * guarantee that it will be preserved
     */
    PMIX_XFER_PROCID(&local_lifeline, proc);
    lifeline = &local_lifeline;

    return PRTE_SUCCESS;
}

static void radix_tree(int rank, int *num_children_out, prte_list_t *children,
                       prte_bitmap_t *relatives)
{
    int i, peer, Sum, NInLevel;
    prte_routed_tree_t *child;
    prte_bitmap_t *relations;

    /* compute how many procs are at my level */
    Sum = 1;
    NInLevel = 1;

    while (Sum < (rank + 1)) {
        NInLevel *= prte_routed_radix_component.radix;
        Sum += NInLevel;
    }

    /* our children start at our rank + num_in_level */
    peer = rank + NInLevel;
    for (i = 0; i < prte_routed_radix_component.radix; i++) {
        if (peer < (int) prte_process_info.num_daemons) {
            child = PRTE_NEW(prte_routed_tree_t);
            child->rank = peer;
            if (NULL != children) {
                /* this is a direct child - add it to my list */
                prte_list_append(children, &child->super);
                (*num_children_out)++;
                /* setup the relatives bitmap */
                prte_bitmap_init(&child->relatives, prte_process_info.num_daemons);
                /* point to the relatives */
                relations = &child->relatives;
            } else {
                /* we are recording someone's relatives - set the bit */
                if (PRTE_SUCCESS != prte_bitmap_set_bit(relatives, peer)) {
                    prte_output(0, "%s Error: could not set relations bit!",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                }
                /* point to this relations */
                relations = relatives;
                PRTE_RELEASE(child);
            }
            /* search for this child's relatives */
            radix_tree(peer, NULL, NULL, relations);
        }
        peer += NInLevel;
    }
}

static void update_routing_plan(void)
{
    prte_routed_tree_t *child;
    int j;
    prte_list_item_t *item;
    int Level, Sum, NInLevel, Ii;
    int NInPrevLevel;
    prte_job_t *dmns;
    prte_proc_t *d;

    /* clear the list of children if any are already present */
    while (NULL != (item = prte_list_remove_first(&my_children))) {
        PRTE_RELEASE(item);
    }
    num_children = 0;

    /* compute my parent */
    Ii = PRTE_PROC_MY_NAME->rank;
    Level = 0;
    Sum = 1;
    NInLevel = 1;

    while (Sum < (Ii + 1)) {
        Level++;
        NInLevel *= prte_routed_radix_component.radix;
        Sum += NInLevel;
    }
    Sum -= NInLevel;

    NInPrevLevel = NInLevel / prte_routed_radix_component.radix;

    if (0 == Ii) {
        PRTE_PROC_MY_PARENT->rank = -1;
    } else {
        PRTE_PROC_MY_PARENT->rank = (Ii - Sum) % NInPrevLevel;
        PRTE_PROC_MY_PARENT->rank += (Sum - NInPrevLevel);
    }

    /* compute my direct children and the bitmap that shows which vpids
     * lie underneath their branch
     */
    radix_tree(Ii, &num_children, &my_children, NULL);

    if (0 < prte_output_get_verbosity(prte_routed_base_framework.framework_output)) {
        prte_output(0, "%s: parent %d num_children %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    PRTE_PROC_MY_PARENT->rank, num_children);
        dmns = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        for (item = prte_list_get_first(&my_children); item != prte_list_get_end(&my_children);
             item = prte_list_get_next(item)) {
            child = (prte_routed_tree_t *) item;
            d = (prte_proc_t *) prte_pointer_array_get_item(dmns->procs, child->rank);
            prte_output(0, "%s: \tchild %d node %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        child->rank, d->node->name);
            for (j = 0; j < (int) prte_process_info.num_daemons; j++) {
                if (prte_bitmap_is_set_bit(&child->relatives, j)) {
                    prte_output(0, "%s: \t\trelation %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), j);
                }
            }
        }
    }
}

static void get_routing_list(prte_list_t *coll)
{
    prte_routed_base_xcast_routing(coll, &my_children);
}

static size_t num_routes(void)
{
    return prte_list_get_size(&my_children);
}
