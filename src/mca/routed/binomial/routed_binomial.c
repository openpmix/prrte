/*
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
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
#include "src/class/prte_pointer_array.h"
#include "src/util/bit_ops.h"
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

#include "routed_binomial.h"
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

prte_routed_module_t prte_routed_binomial_module = {
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

    /* if we are using static ports, set my lifeline to point at my parent */
    if (prte_static_ports) {
        lifeline = PRTE_PROC_MY_PARENT;
    } else {
        /* set our lifeline to the HNP - we will abort if that connection is lost */
        lifeline = PRTE_PROC_MY_HNP;
    }
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_PARENT->nspace, PRTE_PROC_MY_NAME->nspace);

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
                         "%s routed_binomial_delete_route for %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));

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
                         "%s routed_binomial_update: %s --> %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(target), PRTE_NAME_PRINT(route)));

    /* if I am a daemon and the target is my HNP, then check
     * the route - if it isn't direct, then we just flag that
     * we have a route to the HNP
     */
    if (PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, PRTE_PROC_MY_HNP, target)
        && PRTE_EQUAL != prte_util_compare_name_fields(PRTE_NS_CMP_ALL, PRTE_PROC_MY_HNP, route)) {
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

    /* initialize */
    PMIX_XFER_PROCID(&daemon, PRTE_PROC_MY_NAME);

    if (PMIX_PROCID_INVALID(target)) {
        ret = PRTE_NAME_INVALID;
        goto found;
    }

    /* if it is me, then the route is just direct */
    if (PMIX_CHECK_PROCID(PRTE_PROC_MY_NAME, target)) {
        ret = target;
        goto found;
    }

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

    /* find out what daemon hosts this proc */
    if (PMIX_RANK_INVALID == (daemon.rank = prte_get_proc_daemon_vpid(target))) {
        /*PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);*/
        ret = PRTE_NAME_INVALID;
        goto found;
    }

    /* if the daemon is me, then send direct to the target! */
    if (PRTE_PROC_MY_NAME->rank == daemon.rank) {
        ret = target;
        goto found;
    }

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

    /* if we get here, then the target daemon is not beneath
     * any of our children, so we have to step up through our parent
     */
    daemon.rank = PRTE_PROC_MY_PARENT->rank;

    ret = &daemon;

found:
    PRTE_OUTPUT_VERBOSE((1, prte_routed_base_framework.framework_output,
                         "%s routed_binomial_get(%s) --> %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
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
                             "%s routed:binomial: Connection to lifeline %s lost",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(lifeline)));
        return PRTE_ERR_FATAL;
    }

    /* is it a daemon, and one of my children? if so, then
     * remove it from the child list
     */
    if (PMIX_CHECK_NSPACE(route->nspace, PRTE_PROC_MY_NAME->nspace)) {
        for (item = prte_list_get_first(&my_children); item != prte_list_get_end(&my_children);
             item = prte_list_get_next(item)) {
            child = (prte_routed_tree_t *) item;
            if (child->rank == route->rank) {
                PRTE_OUTPUT_VERBOSE((4, prte_routed_base_framework.framework_output,
                                     "%s routed_binomial: removing route to child daemon %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(route)));
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

static int binomial_tree(int rank, int parent, int me, int num_procs, int *nchildren,
                         prte_list_t *childrn, prte_bitmap_t *relatives, bool mine)
{
    int i, bitmap, peer, hibit, mask, found;
    prte_routed_tree_t *child;
    prte_bitmap_t *relations;

    PRTE_OUTPUT_VERBOSE((3, prte_routed_base_framework.framework_output,
                         "%s routed:binomial rank %d parent %d me %d num_procs %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), rank, parent, me, num_procs));

    /* is this me? */
    if (me == rank) {
        bitmap = prte_cube_dim(num_procs);

        hibit = prte_hibit(rank, bitmap);
        --bitmap;

        for (i = hibit + 1, mask = 1 << i; i <= bitmap; ++i, mask <<= 1) {
            peer = rank | mask;
            if (peer < num_procs) {
                child = PRTE_NEW(prte_routed_tree_t);
                child->rank = peer;
                PRTE_OUTPUT_VERBOSE((3, prte_routed_base_framework.framework_output,
                                     "%s routed:binomial %d found child %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), rank,
                                     PRTE_VPID_PRINT(child->rank)));

                if (mine) {
                    /* this is a direct child - add it to my list */
                    prte_list_append(childrn, &child->super);
                    (*nchildren)++;
                    /* setup the relatives bitmap */
                    prte_bitmap_init(&child->relatives, num_procs);

                    /* point to the relatives */
                    relations = &child->relatives;
                } else {
                    /* we are recording someone's relatives - set the bit */
                    prte_bitmap_set_bit(relatives, peer);
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
    PRTE_OUTPUT_VERBOSE((5, prte_routed_base_framework.framework_output,
                         "%s routed:binomial find children of rank %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), rank));
    bitmap = prte_cube_dim(num_procs);

    hibit = prte_hibit(rank, bitmap);
    --bitmap;

    for (i = hibit + 1, mask = 1 << i; i <= bitmap; ++i, mask <<= 1) {
        peer = rank | mask;
        PRTE_OUTPUT_VERBOSE((5, prte_routed_base_framework.framework_output,
                             "%s routed:binomial find children checking peer %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), peer));
        if (peer < num_procs) {
            PRTE_OUTPUT_VERBOSE((5, prte_routed_base_framework.framework_output,
                                 "%s routed:binomial find children computing tree",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* execute compute on this child */
            if (0 <= (found = binomial_tree(peer, rank, me, num_procs, nchildren, childrn,
                                            relatives, mine))) {
                PRTE_OUTPUT_VERBOSE((5, prte_routed_base_framework.framework_output,
                                     "%s routed:binomial find children returning found value %d",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), found));
                return found;
            }
        }
    }
    return -1;
}

static void update_routing_plan(void)
{
    prte_routed_tree_t *child;
    int j;
    prte_list_item_t *item;

    /* clear the list of children if any are already present */
    while (NULL != (item = prte_list_remove_first(&my_children))) {
        PRTE_RELEASE(item);
    }
    num_children = 0;

    /* compute my direct children and the bitmap that shows which vpids
     * lie underneath their branch
     */
    PRTE_PROC_MY_PARENT->rank = binomial_tree(0, 0, PRTE_PROC_MY_NAME->rank,
                                              prte_process_info.num_daemons, &num_children,
                                              &my_children, NULL, true);

    if (0 < prte_output_get_verbosity(prte_routed_base_framework.framework_output)) {
        prte_output(0, "%s: parent %u num_children %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    PRTE_PROC_MY_PARENT->rank, num_children);
        for (item = prte_list_get_first(&my_children); item != prte_list_get_end(&my_children);
             item = prte_list_get_next(item)) {
            child = (prte_routed_tree_t *) item;
            prte_output(0, "%s: \tchild %u", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), child->rank);
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
    PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output, "%s num routes %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) prte_list_get_size(&my_children)));
    return prte_list_get_size(&my_children);
}
