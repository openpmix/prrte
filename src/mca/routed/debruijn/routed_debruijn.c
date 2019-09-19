/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
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
#include "routed_debruijn.h"


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

prrte_routed_module_t prrte_routed_debruijn_module = {
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
static prrte_list_t              my_children;
static bool                     hnp_direct=true;
static int                      log_nranks;
static int                      log_npeers;
static unsigned int             rank_mask;

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

    return PRRTE_SUCCESS;
}

static int delete_route(prrte_process_name_t *proc)
{
    if (proc->jobid == PRRTE_JOBID_INVALID ||
        proc->vpid == PRRTE_VPID_INVALID) {
        return PRRTE_ERR_BAD_PARAM;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_debruijn_delete_route for %s",
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
                         "%s routed_debruijn_update: %s --> %s",
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

static inline unsigned int debruijn_next_hop (int target)
{
    const int my_id = PRRTE_PROC_MY_NAME->vpid;
    uint64_t route, mask = rank_mask;
    unsigned int i, next_hop;

    if (target == my_id) {
        return my_id;
    }

    i = -log_npeers;
    do {
        i += log_npeers;
        mask = (mask >> i) << i;
        route = (my_id << i) | target;
    } while ((route & mask) != (((my_id << i) & target) & mask));

    next_hop = (int)((route >> (i - log_npeers)) & rank_mask);

    /* if the next hop does not exist route to the lowest proc with the same lower routing bits */
    return (next_hop < prrte_process_info.num_procs) ? next_hop : (next_hop & (rank_mask >> log_npeers));
}

static prrte_process_name_t get_route(prrte_process_name_t *target)
{
    prrte_process_name_t ret;

    /* initialize */

    do {
        ret = *PRRTE_NAME_INVALID;

        if (PRRTE_JOBID_INVALID == target->jobid ||
            PRRTE_VPID_INVALID == target->vpid) {
            break;
        }

        /* if it is me, then the route is just direct */
        if (PRRTE_EQUAL == prrte_dss.compare(PRRTE_PROC_MY_NAME, target, PRRTE_NAME)) {
            ret = *target;
            break;
        }

        if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_HNP, target)) {
            if (!hnp_direct || prrte_static_ports) {
                PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                                     "%s routing to the HNP through my parent %s",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_PARENT)));
                ret = *PRRTE_PROC_MY_PARENT;
            } else {
                PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                                     "%s routing direct to the HNP",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                ret = *PRRTE_PROC_MY_HNP;
            }

            break;
        }

        ret.jobid = PRRTE_PROC_MY_NAME->jobid;
        /* find out what daemon hosts this proc */
        if (PRRTE_VPID_INVALID == (ret.vpid = prrte_get_proc_daemon_vpid(target))) {
            /* we don't yet know about this daemon. just route this to the "parent" */
            ret = *PRRTE_PROC_MY_PARENT;
            break;
        }

        /* if the daemon is me, then send direct to the target! */
        if (PRRTE_PROC_MY_NAME->vpid == ret.vpid) {
            ret = *target;
            break;
        }

        /* find next hop */
        ret.vpid = debruijn_next_hop (ret.vpid);
    } while (0);

    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_debruijn_get(%s) --> %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(target),
                         PRRTE_NAME_PRINT(&ret)));

    return ret;
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
                             "%s routed:debruijn: Connection to lifeline %s lost",
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

static unsigned int ilog2 (unsigned int v)
{
    const unsigned int b[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
    const unsigned int S[] = {1, 2, 4, 8, 16};
    int i;

    register unsigned int r = 0;
    for (i = 4; i >= 0; i--) {
        if (v & b[i]) {
            v >>= S[i];
            r |= S[i];
        }
    }

    return r;
}

static void update_routing_plan(void)
{
    prrte_routed_tree_t *child;
    prrte_list_item_t *item;
    int my_vpid = PRRTE_PROC_MY_NAME->vpid;
    int i;

    /* clear the list of children if any are already present */
    while (NULL != (item = prrte_list_remove_first(&my_children))) {
        PRRTE_RELEASE(item);
    }

    log_nranks = (int) ilog2 ((unsigned int)prrte_process_info.num_procs) ;
    assert(log_nranks < 31);

    if (log_nranks < 3) {
      log_npeers = 1;
    } else if (log_nranks < 7) {
      log_npeers = 2;
    } else {
      log_npeers = 4;
    }

    /* round log_nranks to a multiple of log_npeers */
    log_nranks = ((log_nranks + log_npeers) & ~(log_npeers - 1)) - 1;

    rank_mask = (1 << (log_nranks + 1)) - 1;

    /* compute my parent */
    PRRTE_PROC_MY_PARENT->vpid = my_vpid ? my_vpid >> log_npeers : -1;

    /* only add peers to the routing tree if this rank is the smallest rank that will send to
       the any peer */
    if ((my_vpid >> (log_nranks + 1 - log_npeers)) == 0) {
        for (i = (1 << log_npeers) - 1 ; i >= 0 ; --i) {
            int next = ((my_vpid << log_npeers) | i) & rank_mask;

            /* add a peer to the routing tree only if its vpid is smaller than this rank */
            if (next > my_vpid && next < (int)prrte_process_info.num_procs) {
                child = PRRTE_NEW(prrte_routed_tree_t);
                child->vpid = next;
                prrte_list_append (&my_children, &child->super);
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
