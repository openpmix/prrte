/*
 * Copyright (c) 2007-2011 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
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

#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"

#include "src/mca/rml/base/rml_contact.h"

#include "routed_direct.h"
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

prte_routed_module_t prte_routed_direct_module = {
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

static pmix_proc_t mylifeline;
static pmix_proc_t *lifeline = NULL;
static prte_list_t my_children;

static int init(void)
{
    lifeline = NULL;

    if (PRTE_PROC_IS_DAEMON) {
        PMIX_LOAD_NSPACE(PRTE_PROC_MY_PARENT->nspace, PRTE_PROC_MY_NAME->nspace);
        /* if we are using static ports, set my lifeline to point at my parent */
        if (prte_static_ports) {
            /* we will have been given our parent's vpid by MCA param */
            lifeline = PRTE_PROC_MY_PARENT;
        } else {
            /* set our lifeline to the HNP - we will abort if that connection is lost */
            lifeline = PRTE_PROC_MY_HNP;
            PRTE_PROC_MY_PARENT->rank = 0;
        }
    }
    /* setup the list of children */
    PRTE_CONSTRUCT(&my_children, prte_list_t);

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    PRTE_LIST_DESTRUCT(&my_children);
    return PRTE_SUCCESS;
}

static int delete_route(pmix_proc_t *proc)
{
    PRTE_OUTPUT_VERBOSE((1, prte_routed_base_framework.framework_output,
                         "%s routed_direct_delete_route for %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(proc)));

    /*There is nothing to do here */

    return PRTE_SUCCESS;
}

static int update_route(pmix_proc_t *target, pmix_proc_t *route)
{
    PRTE_OUTPUT_VERBOSE((1, prte_routed_base_framework.framework_output,
                         "%s routed_direct_update: %s --> %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(target), PRTE_NAME_PRINT(route)));

    /*There is nothing to do here */

    return PRTE_SUCCESS;
}

static pmix_proc_t get_route(pmix_proc_t *target)
{
    pmix_proc_t *ret, daemon;

    if (PMIX_PROCID_INVALID(target)) {
        ret = PRTE_NAME_INVALID;
        goto found;
    }

    if (PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, PRTE_PROC_MY_HNP, target)) {
        PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                             "%s routing direct to the HNP", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        ret = PRTE_PROC_MY_HNP;
        goto found;
    }

    PMIX_LOAD_NSPACE(daemon.nspace, PRTE_PROC_MY_NAME->nspace);
    /* find out what daemon hosts this proc */
    if (PMIX_RANK_INVALID == (daemon.rank = prte_get_proc_daemon_vpid(target))) {
        ret = PRTE_NAME_INVALID;
        goto found;
    }

    /* if the daemon is me, then send direct to the target! */
    if (PRTE_PROC_MY_NAME->rank == daemon.rank) {
        ret = target;
        goto found;
    }

    /* else route to this daemon directly */
    ret = &daemon;

found:
    PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                         "%s routed_direct_get(%s) --> %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
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
                             "%s routed:direct: Connection to lifeline %s lost",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(lifeline)));
        return PRTE_ERR_FATAL;
    }

    /* if we are the HNP, and the route is a daemon,
     * see if it is one of our children - if so, remove it
     */
    if (PRTE_PROC_IS_MASTER && PMIX_CHECK_NSPACE(route->nspace, PRTE_PROC_MY_NAME->nspace)) {
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
    /* all routes are defined */
    return true;
}

static int set_lifeline(pmix_proc_t *proc)
{
    PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                         "%s routed:direct: set lifeline to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(proc)));
    mylifeline = *proc;
    lifeline = &mylifeline;
    return PRTE_SUCCESS;
}

static void update_routing_plan(void)
{
    prte_routed_tree_t *child;
    int32_t i;
    prte_job_t *jdata;
    prte_proc_t *proc;

    PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                         "%s routed:direct: update routing plan",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (!PRTE_PROC_IS_MASTER) {
        /* nothing to do */
        return;
    }

    /* clear the current list */
    PRTE_LIST_DESTRUCT(&my_children);
    PRTE_CONSTRUCT(&my_children, prte_list_t);

    /* HNP is directly connected to each daemon */
    if (NULL == (jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    for (i = 1; i < jdata->procs->size; i++) {
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, i))) {
            continue;
        }
        child = PRTE_NEW(prte_routed_tree_t);
        child->rank = proc->name.rank;
        prte_list_append(&my_children, &child->super);
    }

    return;
}

static void get_routing_list(prte_list_t *coll)
{

    PRTE_OUTPUT_VERBOSE((2, prte_routed_base_framework.framework_output,
                         "%s routed:direct: get routing list", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    prte_routed_base_xcast_routing(coll, &my_children);
}

static size_t num_routes(void)
{
    if (!PRTE_PROC_IS_MASTER) {
        return 0;
    }
    return prte_list_get_size(&my_children);
}
