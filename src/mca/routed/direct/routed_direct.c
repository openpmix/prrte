/*
 * Copyright (c) 2007-2011 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/dss/dss.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"
#include "src/runtime/prrte_wait.h"

#include "src/mca/rml/base/rml_contact.h"

#include "src/mca/routed/base/base.h"
#include "routed_direct.h"

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

prrte_routed_module_t prrte_routed_direct_module = {
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

static prrte_process_name_t mylifeline;
static prrte_process_name_t *lifeline = NULL;
static prrte_list_t my_children;

static int init(void)
{
    lifeline = NULL;

    if (PRRTE_PROC_IS_DAEMON) {
        PRRTE_PROC_MY_PARENT->jobid = PRRTE_PROC_MY_NAME->jobid;
        /* if we are using static ports, set my lifeline to point at my parent */
        if (prrte_static_ports) {
            /* we will have been given our parent's vpid by MCA param */
            lifeline = PRRTE_PROC_MY_PARENT;
        } else {
            /* set our lifeline to the HNP - we will abort if that connection is lost */
            lifeline = PRRTE_PROC_MY_HNP;
            PRRTE_PROC_MY_PARENT->vpid = 0;
        }
    }
    /* setup the list of children */
    PRRTE_CONSTRUCT(&my_children, prrte_list_t);

    return PRRTE_SUCCESS;
}

static int finalize(void)
{
    PRRTE_LIST_DESTRUCT(&my_children);
    return PRRTE_SUCCESS;
}

static int delete_route(prrte_process_name_t *proc)
{
    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_direct_delete_route for %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc)));

    /*There is nothing to do here */

    return PRRTE_SUCCESS;
}

static int update_route(prrte_process_name_t *target,
                        prrte_process_name_t *route)
{
    PRRTE_OUTPUT_VERBOSE((1, prrte_routed_base_framework.framework_output,
                         "%s routed_direct_update: %s --> %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(target),
                         PRRTE_NAME_PRINT(route)));

    /*There is nothing to do here */

    return PRRTE_SUCCESS;
}


static prrte_process_name_t get_route(prrte_process_name_t *target)
{
    prrte_process_name_t *ret, daemon;

    if (target->jobid == PRRTE_JOBID_INVALID ||
        target->vpid == PRRTE_VPID_INVALID) {
        ret = PRRTE_NAME_INVALID;
        goto found;
    }

    /* initialize */
    daemon.jobid = PRRTE_PROC_MY_DAEMON->jobid;
    daemon.vpid = PRRTE_PROC_MY_DAEMON->vpid;
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_HNP, target)) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                    "%s routing direct to the HNP",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        ret = PRRTE_PROC_MY_HNP;
        goto found;
    }

    daemon.jobid = PRRTE_PROC_MY_NAME->jobid;
    /* find out what daemon hosts this proc */
    if (PRRTE_VPID_INVALID == (daemon.vpid = prrte_get_proc_daemon_vpid(target))) {
        ret = PRRTE_NAME_INVALID;
        goto found;
    }

    /* if the daemon is me, then send direct to the target! */
    if (PRRTE_PROC_MY_NAME->vpid == daemon.vpid) {
        ret = target;
        goto found;
    }

    /* else route to this daemon directly */
    ret = &daemon;

 found:
    PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                         "%s routed_direct_get(%s) --> %s",
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
                             "%s routed:direct: Connection to lifeline %s lost",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(lifeline)));
        return PRRTE_ERR_FATAL;
    }

    /* if we are the HNP, and the route is a daemon,
     * see if it is one of our children - if so, remove it
     */
    if (PRRTE_PROC_IS_MASTER &&
        route->jobid == PRRTE_PROC_MY_NAME->jobid) {
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
    /* all routes are defined */
    return true;
}

static int set_lifeline(prrte_process_name_t *proc)
{
    PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                         "%s routed:direct: set lifeline to %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc)));
    mylifeline = *proc;
    lifeline = &mylifeline;
    return PRRTE_SUCCESS;
}

static void update_routing_plan(void)
{
    prrte_routed_tree_t *child;
    int32_t i;
    prrte_job_t *jdata;
    prrte_proc_t *proc;

    PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                         "%s routed:direct: update routing plan",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    if (!PRRTE_PROC_IS_MASTER) {
        /* nothing to do */
        return;
    }

    /* clear the current list */
    PRRTE_LIST_DESTRUCT(&my_children);
    PRRTE_CONSTRUCT(&my_children, prrte_list_t);

    /* HNP is directly connected to each daemon */
    if (NULL == (jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return;
    }
    for (i=1; i < jdata->procs->size; i++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, i))) {
            continue;
        }
        child = PRRTE_NEW(prrte_routed_tree_t);
        child->vpid = proc->name.vpid;
        prrte_list_append(&my_children, &child->super);
    }

    return;
}

static void get_routing_list(prrte_list_t *coll)
{

    PRRTE_OUTPUT_VERBOSE((2, prrte_routed_base_framework.framework_output,
                         "%s routed:direct: get routing list",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    prrte_routed_base_xcast_routing(coll, &my_children);
}

static size_t num_routes(void)
{
    if (!PRRTE_PROC_IS_MASTER) {
        return 0;
    }
    return prrte_list_get_size(&my_children);
}
