/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <limits.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif

#include "constants.h"
#include "types.h"

#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/mca/preg/preg.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/state/state.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/error_strings.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "src/util/pmix_string_copy.h"
#include "src/util/proc_info.h"
#include "src/util/prte_cmd_line.h"

#include "src/mca/ras/base/base.h"

char *prte_ras_base_flag_string(prte_node_t *node)
{
    char *tmp, *t3, **t2 = NULL;

    if (0 == node->flags) {
        tmp = strdup("Flags: NONE");
        return tmp;
    }

    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
        PMIx_Argv_append_nosize(&t2, "DAEMON_LAUNCHED");
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_LOC_VERIFIED)) {
        PMIx_Argv_append_nosize(&t2, "LOCATION_VERIFIED");
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_OVERSUBSCRIBED)) {
        PMIx_Argv_append_nosize(&t2, "OVERSUBSCRIBED");
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
        PMIx_Argv_append_nosize(&t2, "MAPPED");
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
        PMIx_Argv_append_nosize(&t2, "SLOTS_GIVEN");
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_NON_USABLE)) {
        PMIx_Argv_append_nosize(&t2, "NONUSABLE");
    }
    if (NULL != t2) {
        t3 = PMIx_Argv_join(t2, ':');
        pmix_asprintf(&tmp, "Flags: %s", t3);
        free(t3);
        PMIx_Argv_free(t2);
    } else {
        tmp = strdup("Flags: NONE");
    }
    return tmp;
}

/* function to display allocation */
void prte_ras_base_display_alloc(prte_job_t *jdata)
{
    char *tmp = NULL, *tmp2, *tmp3;
    int i, istart;
    prte_node_t *alloc;
    char *flgs, *aliases;
    bool parsable;
    pmix_proc_t source;

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_ALLOC_DISPLAYED, NULL, PMIX_BOOL)) {
        return;
    }

    parsable = prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT, NULL, PMIX_BOOL);
    PMIX_LOAD_PROCID(&source, jdata->nspace, PMIX_RANK_WILDCARD);

    if (parsable) {
        pmix_asprintf(&tmp, "<allocation>\n");
    } else {
        pmix_asprintf(&tmp,
                      "\n======================   ALLOCATED NODES FOR JOB %s  ======================\n", jdata->nspace);
    }
    if (prte_hnp_is_allocated) {
        istart = 0;
    } else {
        istart = 1;
    }
    for (i = istart; i < prte_node_pool->size; i++) {
        if (NULL == (alloc = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
            continue;
        }
        if (parsable) {
            /* need to create the output in XML format */
            pmix_asprintf(&tmp2,
                          "\t<host name=\"%s\" slots=\"%d\" max_slots=\"%d\" slots_inuse=\"%d\">\n",
                          (NULL == alloc->name) ? "UNKNOWN" : alloc->name, (int) alloc->slots,
                          (int) alloc->slots_max, (int) alloc->slots_inuse);
        } else {
            /* build the flags string */
            flgs = prte_ras_base_flag_string(alloc);
            /* build the aliases string */
            if (NULL != alloc->aliases) {
                aliases = PMIx_Argv_join(alloc->aliases, ',');
            } else {
                aliases = NULL;
            }
            pmix_asprintf(&tmp2, "    %s: slots=%d max_slots=%d slots_inuse=%d state=%s\n\t%s\n\taliases: %s\n",
                          (NULL == alloc->name) ? "UNKNOWN" : alloc->name, (int) alloc->slots,
                          (int) alloc->slots_max, (int) alloc->slots_inuse,
                          prte_node_state_to_str(alloc->state), flgs,
                          (NULL == aliases) ? "NONE" : aliases);
            free(flgs);
            if (NULL != aliases) {
                free(aliases);
            }
        }
        if (NULL == tmp) {
            tmp = tmp2;
        } else {
            pmix_asprintf(&tmp3, "%s%s", tmp, tmp2);
            free(tmp);
            free(tmp2);
            tmp = tmp3;
        }
    }
    if (parsable) {
        pmix_asprintf(&tmp2, "%s</allocation>\n", tmp);
    } else {
        pmix_asprintf(&tmp2,
                    "%s=================================================================\n", tmp);
    }
    free(tmp);
    if (prte_persistent) {
        fprintf(stdout, "%s", tmp2);
        /* prte_iof_base_output would have taken ownership of the string;
         * this branch does not hand it anywhere, so it has to free it */
        free(tmp2);
    } else {
        prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, tmp2);
    }
    prte_set_attribute(&jdata->attributes, PRTE_JOB_ALLOC_DISPLAYED, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
}

static void display_cpus(prte_topology_t *t,
                         prte_job_t *jdata,
                         char *node)
{
    char *tmp;
    unsigned pkg, npkgs;
    bool use_hwthread_cpus, physical;
    hwloc_obj_t obj;
    hwloc_cpuset_t avail = NULL;
    hwloc_cpuset_t allowed;
    bool parsable;

    parsable = prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT, NULL, PMIX_BOOL);

    use_hwthread_cpus = prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL);
    physical = prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL,
                                  PMIX_BOOL);
    avail = hwloc_bitmap_alloc();

    if (parsable) {
        /* "parseable" has to actually parse. This element carried an
         * unquoted attribute value and wrapped a "<pkg=0 cpus=0-7>" that is
         * not an element at all, while the very same information inside
         * "--display map:parseable" is written as
         * <package id="0" cpus="0-7"/> - the two spellings of one fact, one
         * of them unusable by any XML reader. Follow the map document's
         * shape; see prte_node_print() in runtime/data_type_support. */
        pmix_output(prte_clean_output, "<processors node=\"%s\">", node);
    } else {
        pmix_output(prte_clean_output,
                    "\n======================   AVAILABLE PROCESSORS [node: %s]   ======================\n\n", node);
    }
    npkgs = prte_hwloc_base_get_nbobjs_by_type(t->topo, HWLOC_OBJ_PACKAGE);
    allowed = (hwloc_cpuset_t)hwloc_topology_get_allowed_cpuset(t->topo);
    for (pkg = 0; pkg < npkgs; pkg++) {
        obj = prte_hwloc_base_get_obj_by_type(t->topo, HWLOC_OBJ_PACKAGE, pkg);
        hwloc_bitmap_and(avail, obj->cpuset, allowed);
        if (hwloc_bitmap_iszero(avail)) {
            if (parsable) {
                pmix_output(prte_clean_output, "    <package id=\"%d\" cpus=\"%s\"/>", pkg, "NONE");
            } else {
                pmix_output(prte_clean_output, "PKG[%d]: NONE", pkg);
            }
            continue;
        }
        /* the bits are PU OS indices; what the user needs to read back out
         * (and hand to --cpu-set) is the list of cores, or of hwthreads if
         * that is what this job is using as cpus */
        tmp = prte_hwloc_base_cpuset2ranges(t->topo, avail, use_hwthread_cpus, physical);
        if (parsable) {
            pmix_output(prte_clean_output, "    <package id=\"%d\" cpus=\"%s\"/>", pkg,
                        (NULL == tmp) ? "NONE" : tmp);
        } else {
            pmix_output(prte_clean_output, "PKG[%d]: %s", pkg, (NULL == tmp) ? "NONE" : tmp);
        }
        free(tmp);
    }
    hwloc_bitmap_free(avail);
    if (parsable) {
        pmix_output(prte_clean_output, "</processors>\n");
    } else {
        pmix_output(prte_clean_output,
                    "\n======================================================================\n");
    }
    return;
}

void prte_ras_base_display_cpus(prte_job_t *jdata, char *nodelist)
{
    char **nodes = NULL;
    int i, j, m;
    prte_topology_t *t;
    prte_node_t *nptr;
    bool moveon;

    if (NULL == nodelist) {
        /* output the available cpus for all topologies */
        for (i=0; i < prte_node_topologies->size; i++) {
            t = (prte_topology_t*)pmix_pointer_array_get_item(prte_node_topologies, i);
            if (NULL != t) {
                display_cpus(t, jdata, "N/A");
            }
        }
        return;
    }

    nodes = PMIx_Argv_split(nodelist, ';');
    if (NULL == nodes) {
        /* the nodelist held nothing we can resolve */
        return;
    }
    for (j=0; NULL != nodes[j]; j++) {
        moveon = false;
        for (i=0; i < prte_node_pool->size && !moveon; i++) {
            nptr = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, i);
            if (NULL == nptr) {
                continue;
            }
            if (0 == strcmp(nptr->name, nodes[j])) {
                /* a node that has not yet been launched upon carries no
                 * topology - there is nothing to display for it */
                if (NULL != nptr->topology) {
                    display_cpus(nptr->topology, jdata, nodes[j]);
                }
                break;
            }
            if (NULL == nptr->aliases) {
                continue;
            }
            /* no choice but an exhaustive search - fortunately, these lists are short! */
            for (m = 0; NULL != nptr->aliases[m]; m++) {
                if (0 == strcmp(nodes[j], nptr->aliases[m])) {
                    /* this is the node! */
                    if (NULL != nptr->topology) {
                        display_cpus(nptr->topology, jdata, nodes[j]);
                    }
                    moveon = true;
                    break;
                }
            }
        }
    }
    PMIx_Argv_free(nodes);
}


/*
 * Function for selecting one component from all those that are
 * available.
 */
void prte_ras_base_allocate(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    int rc;
    prte_job_t *jdata;
    pmix_list_t nodes;
    prte_node_t *node;
    int32_t j;
    pmix_status_t ret;
    prte_ras_base_selected_module_t *mod;
    char *hosts;
    char **hostlist;
    char *ptr;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);

    PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:base:allocate",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* convenience */
    jdata = caddy->jdata;
    /* construct a list to hold the results */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);

    /* The nodes discovered for the DVM's initial (daemon-job) allocation
     * constitute the fixed base allocation for the entire session, whoever
     * provided them - a scheduler, a hostfile, or -host. Once that base has
     * been established, a subsequent job (for example, the child of a
     * PMIx_Spawn) must not re-run discovery. Re-reading a hostfile overwrites
     * the established per-node slot counts while clearing
     * PRTE_NODE_FLAG_SLOTS_GIVEN, which lets the node be re-sized to its core
     * count - hiding genuine oversubscription from the mapper and binding
     * spawned processes on a node that is actually oversubscribed. Re-reading
     * a resource manager is no better: an RM component that has already
     * recorded this allocation has to spend a return code saying so
     * (ras/slurm answers PRTE_EXISTS to avoid double-inserting the whole
     * node set), and one that does not would insert it twice.
     *
     * The only sanctioned way to change an established allocation is an
     * explicit add-host/add-hostfile or allocation request, which is handled
     * separately (prte_ras_base_add_hosts / prte_ras_base_modify) before we
     * ever reach this point. So if the base allocation already exists and
     * this is not the DVM's own daemon job, simply reuse it.
     *
     * The "established" test is deliberately independent of whether the HNP
     * node is part of the allocation (prte_ras_base.allocation_established is
     * set when the first allocation completes), so the protection holds even
     * for allocations that exclude the head node. */
    if (prte_ras_base.allocation_established &&
        0 != strcmp(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate reusing established base allocation for job %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_JOBID_PRINT(jdata->nspace)));
        PMIX_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
        // give each module an opportunity to try to make the allocation
        if (NULL == mod->module->allocate) {
            continue;
        }
        rc = mod->module->allocate(jdata, &nodes);
        if (PRTE_SUCCESS == rc) {
            // got an allocation, so we are done
            break;
        }
        if (PRTE_ERR_ALLOCATION_PENDING == rc) {
            /* an allocation request is underway, so just do nothing */
            PMIX_DESTRUCT(&nodes);
            PMIX_RELEASE(caddy);
            return;
        } else if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
            // this module didn't contribute anything
            continue;
        } else if (PRTE_EXISTS == rc) {
            /* fixed allocation has already been discovered */
            PMIX_DESTRUCT(&nodes);
            goto DISPLAY;
        } else {
            PRTE_ERROR_LOG(rc);
            PMIX_DESTRUCT(&nodes);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PMIX_RELEASE(caddy);
            return;
        }
    }

    /* if we didn't find anything, and an allocation is required,
     * then that's an error
     */
    if (pmix_list_is_empty(&nodes)) {
        if (prte_allocation_required) {
            /* an allocation is required, so this is fatal */
            PMIX_DESTRUCT(&nodes);
            prte_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PMIX_RELEASE(caddy);
            return;
        }

        /* if nothing was found by any of the above methods, then we have no
         * earthly idea what to do - so just add the local host
         */
        node = PMIX_NEW(prte_node_t);
        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            PMIX_DESTRUCT(&nodes);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PMIX_RELEASE(caddy);
            return;
        }
        /* use the same name we got in prte_process_info so we avoid confusion in
         * the session directories
         */
        node->name = strdup(prte_process_info.nodename);
        node->state = PRTE_NODE_STATE_UP;
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = 1;
        pmix_list_append(&nodes, &node->super);
        /* mark the HNP as "allocated" since we have nothing else to use */
        prte_hnp_is_allocated = true;
    }

    /* store the results in the global resource pool - this removes the
     * list items
     */
    if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DESTRUCT(&nodes);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        PMIX_RELEASE(caddy);
        return;
    }
    PMIX_DESTRUCT(&nodes);

DISPLAY:
    /* the DVM's base allocation is now established; any later job that does
     * not bring its own scheduler/add-host directives will reuse it */
    prte_ras_base.allocation_established = true;

    /* shall we display the results? */
    if (4 < pmix_output_get_verbosity(prte_ras_base_framework.framework_output) &&
        0 == strcmp(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        prte_ras_base_display_alloc(jdata);
    }

    /* are we to report this event? */
    if (prte_report_events) {
        pmix_info_t info;
        PMIX_INFO_LOAD(&info, "prte.notify.donotloop", NULL, PMIX_BOOL);

        ret = PMIx_Notify_event(PMIX_NOTIFY_ALLOC_COMPLETE, NULL, PMIX_GLOBAL,
                                &info, 1, NULL, NULL);
        PMIX_INFO_DESTRUCT(&info);
        if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PMIX_RELEASE(caddy);
            return;
        }
    }

    /* set total slots alloc */
    jdata->total_slots_alloc = prte_ras_base.total_slots_alloc;

    hosts = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_TOPO, (void**)&hosts, PMIX_STRING)) {
        if (NULL != hosts) {
            hostlist = PMIx_Argv_split(hosts, ';');
            free(hosts);
            for (j=0; NULL != hostlist[j]; j++) {
                node = prte_node_match(NULL, hostlist[j]);
                /* a node only acquires a topology once its daemon has
                 * reported in - a node that is allocated but not yet
                 * launched upon (e.g. one just added by a grow) has none */
                if (NULL == node || NULL == node->topology) {
                    continue;
                }
                pmix_output(prte_clean_output,
                            "=================================================================\n");
                pmix_output(prte_clean_output, "TOPOLOGY FOR NODE %s", node->name);
                if (PRTE_SUCCESS == prte_hwloc_print(&ptr, NULL, node->topology->topo)) {
                    pmix_output(prte_clean_output, "%s", ptr);
                    free(ptr);
                } else {
                    pmix_output(prte_clean_output, "TOPOLOGY NOT AVAILABLE");
                }
                pmix_output(prte_clean_output,
                            "=================================================================\n");
            }
            PMIx_Argv_free(hostlist);
        } else {
            for (j=0; j < prte_node_pool->size; j++) {
                node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, j);
                if (NULL == node || NULL == node->topology) {
                    continue;
                }
                pmix_output(prte_clean_output,
                            "=================================================================\n");
                pmix_output(prte_clean_output, "TOPOLOGY FOR NODE %s", node->name);
                if (PRTE_SUCCESS == prte_hwloc_print(&ptr, NULL, node->topology->topo)) {
                    pmix_output(prte_clean_output, "%s", ptr);
                    free(ptr);
                } else {
                    pmix_output(prte_clean_output, "TOPOLOGY NOT AVAILABLE");
                }
                pmix_output(prte_clean_output,
                            "=================================================================\n");
            }
        }
    }

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOCATION_COMPLETE);

    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_ras_base_release_allocation(prte_session_t *session)
{
    prte_ras_base_selected_module_t *mod;
    int rc;

    /* Only the HNP opens the ras framework - ess/hnp does it, because no
     * other role is permitted to allocate - so on a prted this list has
     * never been PMIX_CONSTRUCT'ed.  It is not empty-but-walkable in that
     * state: PMIX_LIST_STATIC_INIT leaves the sentinel's "next" NULL rather
     * than pointing at itself, so PMIX_LIST_FOREACH starts at NULL, fails
     * its "!= &sentinel" test, and dereferences it.
     *
     * That matters because session_des() calls this unconditionally, and
     * EVERY prted releases prte_default_session in prte_finalize().  So
     * every daemon on every node but the head node was segfaulting on the
     * way out, of every job, successful or not.  It went unnoticed because
     * a daemon's stderr is not forwarded - the backtrace only appears under
     * --leave-session-attached, long after the job's own output.
     *
     * Guard on the framework's own open count rather than on
     * PRTE_PROC_IS_MASTER: that is the condition that actually decides
     * whether the list was constructed, so it stays correct if some other
     * role is ever allowed to open ras. */
    if (0 == prte_ras_base_framework.framework_refcnt) {
        return;
    }

    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
        if (NULL == mod->module->release_allocation) {
            continue;
        }
        if (NULL != session->alloc_module &&
            0 != strcmp(session->alloc_module, mod->component->pmix_mca_component_name)) {
            continue;
        }
        rc = mod->module->release_allocation(session);
        if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            return;
        }
    }
}


typedef struct {
    pmix_list_item_t super;
    prte_pmix_server_req_t *req;
} deferred_release_t;
static PMIX_CLASS_INSTANCE(deferred_release_t, pmix_list_item_t, NULL, NULL);

/* Is any daemon still joining the DVM?
 *
 * A shrink cannot be started while one is.  The shrink is broadcast to the
 * whole DVM and its campaign drains only when the broadcast has reached every
 * daemon, and a daemon that has not reported home cannot be reached - so the
 * campaign would neither complete nor abort, prte_shrink_campaigns would stay
 * non-empty forever, and every later job would park at the LAUNCH_APPS hold
 * (#2617).  The precondition is DVM-wide rather than per-target: what stalls
 * the campaign is the reachability of daemons the caller never selected.
 *
 * Three terms, because the growing DVM passes through three states:
 *
 *  - PRTE_JOB_EXTEND_DVM on the daemon job covers the window between
 *    prte_ras_base_activate_dvm_grow() - which only activates LAUNCH_DAEMONS -
 *    and setup_virtual_machine actually running.  In that window there is no
 *    campaign yet, and the new daemons have no proc objects to inspect.  The
 *    attribute is consumed and removed inside setup_virtual_machine.
 *  - a recorded grow campaign covers launch-in-progress, from
 *    setup_virtual_machine to prte_plm_base_grow_drain() at VM_READY.  Only in
 *    elastic mode, which is the only mode that records campaigns at all.
 *  - a daemon we have recorded but never heard from is the exact statement of
 *    the precondition, and catches any path that reaches neither of the above.
 *    rml_uri is the right test and the only right one: it is written solely by
 *    prte_plm_base_daemon_callback, i.e. only by the daemon itself reporting
 *    in, whereas PRTE_PROC_FLAG_ALIVE and a RUNNING state are both set when the
 *    launch is merely *recorded* (see the matching note in errmgr_dvm.c).  The
 *    ALIVE test is still needed to exclude a daemon that failed to start, which
 *    never reports a URI and must not park releases forever.
 */
bool prte_ras_base_dvm_is_growing(void)
{
    prte_job_t *daemons;
    prte_proc_t *dproc;
    int i;

    if (!pmix_list_is_empty(&prte_grow_campaigns)) {
        return true;
    }

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons) {
        return false;
    }
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_EXTEND_DVM,
                           NULL, PMIX_BOOL)) {
        return true;
    }
    for (i = 0; i < daemons->procs->size; i++) {
        dproc = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, i);
        if (NULL == dproc) {
            continue;
        }
        if (PRTE_PROC_MY_NAME->rank == dproc->name.rank) {
            continue;
        }
        if (PRTE_FLAG_TEST(dproc, PRTE_PROC_FLAG_ALIVE) &&
            NULL == dproc->rml_uri) {
            return true;
        }
    }
    return false;
}

void prte_ras_base_replay_deferred_releases(void)
{
    deferred_release_t *dr;

    /* Replay from scratch rather than resuming mid-flight: everything a
     * deferred request had done before it was parked is in-memory target
     * selection, and no scheduler command runs until a campaign drains.
     * Re-running it also re-selects against the post-grow state, which is what
     * the requester meant in the first place.
     *
     * This is called from both of grow_drain's outcomes.  On the failure path
     * the rolled-back nodes have had node->daemon cleared by
     * prte_plm_base_reset_dvm_node, so a replayed request simply re-evaluates
     * and fails through the ordinary error paths - no separate handling.
     *
     * Guard on the framework's open count, as prte_ras_base_release_allocation
     * does: this is called from plm, whose grow_drain has no way to know
     * whether ras is still open, and the list is only constructed when it
     * is. */
    if (0 == prte_ras_base_framework.framework_refcnt) {
        return;
    }

    while (NULL != (dr = (deferred_release_t *)
                             pmix_list_remove_first(&prte_ras_base.deferred_releases))) {
        prte_pmix_server_req_t *req = dr->req;
        PMIX_RELEASE(dr);
        prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE,
                       prte_ras_base_modify, req);
        PMIX_POST_OBJECT(req);
        prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
    }
}

void prte_ras_base_flush_deferred_releases(pmix_status_t status)
{
    deferred_release_t *dr;

    /* The DVM is going away with requests still parked.  Answer each one - a
     * requester released nothing and must not be left waiting on an event that
     * can no longer be raised. */
    while (NULL != (dr = (deferred_release_t *)
                             pmix_list_remove_first(&prte_ras_base.deferred_releases))) {
        prte_pmix_server_req_t *req = dr->req;
        PMIX_RELEASE(dr);
        req->pstatus = status;
        if (NULL != req->infocbfunc) {
            req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata,
                            prte_pmix_server_req_release, req);
            continue;
        }
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                    req->local_index, NULL);
        PMIX_RELEASE(req);
    }
}

void prte_ras_base_modify(int fd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    prte_ras_base_selected_module_t *mod;
    deferred_release_t *dr;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    /* Hold a release while the DVM is still growing, and replay it once the
     * grow resolves.  Deferring beats rejecting: a caller has no way to know a
     * grow is in flight - ras/slurm's extend acknowledges phase one as soon as
     * the scheduler answers, long before its daemons join - and a target that
     * is still joining becomes removable seconds later, so parking is what
     * keeps the semantics the caller expects.
     *
     * The guard sits here, at the driver's entry, rather than beside campaign
     * creation, so it covers both routes into the shrink machinery at once:
     * the component path (ras/slurm's modify) and the base's own
     * ras_base_complete_release_request, which runs downstream of this. */
    if (PMIX_ALLOC_RELEASE == req->allocdir && prte_ras_base_dvm_is_growing()) {
        PMIX_OUTPUT_VERBOSE((2, prte_ras_base_framework.framework_output,
                             "%s ras:base:modify deferring release - the DVM is"
                             " still growing",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        dr = PMIX_NEW(deferred_release_t);
        if (NULL == dr) {
            req->pstatus = PMIX_ERR_NOMEM;
            goto respond;
        }
        dr->req = req;
        pmix_list_append(&prte_ras_base.deferred_releases, &dr->super);
        return;
    }

    // set the default response
    req->pstatus = PMIX_ERR_NOT_SUPPORTED;

    // cycle across the modules and give each a chance to execute request
    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
        if (NULL != req->key &&
            0 != strcasecmp(req->key, mod->component->pmix_mca_component_name)) {
            continue;
        }
        if (NULL != mod->module->modify) {
            rc = mod->module->modify(req);
            if (PMIX_SUCCESS == rc ||
                PMIX_OPERATION_IN_PROGRESS == req->pstatus) {
                // the module is handling it and will call
                // the callback function when complete
                return;
            } else if (PMIX_OPERATION_SUCCEEDED == rc) {
                // the change was atomically accomplished
                req->pstatus = PMIX_SUCCESS;
                break;
            } else if (PMIX_ERR_TAKE_NEXT_OPTION == rc ||
                       PMIX_ERR_NOT_SUPPORTED == rc) {
                // module couldn't do it
                continue;
            } else {
                // true error
                req->pstatus = rc;
                break;
            }
        }
    }

    // get here if the module isn't handling the results itself

    // if we met the request, then process the results
    if (PMIX_SUCCESS == req->pstatus) {
        prte_ras_base_complete_request(req);
    }

respond:
    // execute the callback
    if (NULL != req->infocbfunc) {
        req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata, prte_pmix_server_req_release, req);
        return;
    }

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);

    PMIX_RELEASE(req);
}

void prte_ras_base_shrink_complete(prte_shrink_campaign_t *campaign)
{
    prte_ras_base_selected_module_t *mod;

    /* cycle across the active modules and give each that supports the
     * entry point a chance to release the freed resources back to the
     * scheduler. Unlike modify, a shrink completion is not keyed to a
     * single component, so every module is offered the campaign. */
    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
        if (NULL != mod->module->shrink_complete) {
            mod->module->shrink_complete(campaign);
        }
    }
}

/* Report the application procs killed by the release of this node.  Nothing
 * else does: the target daemon's proc-state updates race the routing teardown
 * its departure starts, and its later comm failure is ignored (errmgr_dvm.c).
 * An unaccounted proc holds its job below PRTE_JOB_STATE_TERMINATED forever.
 *
 * KILLED_BY_RELEASE, not TERMINATED: the release is planned, the kill is not.
 * Reported as a normal termination it left prun exiting 0 for a job that lost
 * ranks mid-fence.  errmgr/dvm decides what it costs.  Accounting is
 * unchanged - proc_errors force-marks WAITPID_FIRED and IOF_COMPLETE for a
 * remote proc, driving the same TERMINATED activation raised here before.
 *
 * A node lists procs that exited earlier until their whole job terminates, so
 * without the RECORDED test the release of an idle node would abort a healthy
 * job.  Runs before prte_plm_base_reset_dvm_node(), which can drop the map's
 * last reference to the node. */
static void release_node_procs(prte_node_t *node)
{
    prte_proc_t *proc;
    int i;

    if (NULL == node || NULL == node->procs) {
        return;
    }

    for (i = 0; i < node->procs->size; i++) {
        proc = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, i);
        if (NULL == proc) {
            continue;
        }
        /* defensive: only application procs are recorded on a node, but the
         * daemon job is not ours to account for -- the per-target block owns
         * the target daemon's teardown */
        if (PMIX_CHECK_NSPACE(proc->name.nspace, PRTE_PROC_MY_NAME->nspace)) {
            continue;
        }
        /* already accounted for: it reported its own termination before the
         * release reached it, so the release did not kill it */
        if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_RECORDED)) {
            continue;
        }
        PMIX_OUTPUT_VERBOSE((2, prte_ras_base_framework.framework_output,
                             "%s ras:base:shrink killing live proc %s with node %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&proc->name), node->name));
        PRTE_ACTIVATE_PROC_STATE(&proc->name, PRTE_PROC_STATE_KILLED_BY_RELEASE);
    }
}

/* Thread-shift target: collectively complete a DVM shrink campaign.  Runs once
 * per campaign on the DVM master, on a fresh event (posted from the grpcomm
 * xcast-completion callback below) so it never executes nested inside the xcast
 * call stack.  By this point the reliable shrink broadcast has been received by
 * every daemon, so the master tears the whole set of targets out of the DVM in
 * a single batch — one routing-tree repair for all of them — rather than once
 * per daemon as each departs.  Because every target is marked failed and its
 * node detached here, the later real departures fall through the errmgr as
 * harmless no-ops (see errmgr_dvm.c). */
static void shrink_campaign_complete(int sd, short args, void *cbdata)
{
    prte_shrink_campaign_t *camp = (prte_shrink_campaign_t *) cbdata;
    prte_job_t *daemons;
    prte_proc_t *dproc;
    prte_node_t *node;
    pmix_data_array_t failed = PMIX_DATA_ARRAY_STATIC_INIT;
    pmix_rank_t *fr;
    int t;

    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* one batch routing-tree repair for the whole campaign: report every target
     * as failed in a single promotion/descendant pass, replacing the up-to-m
     * per-daemon repairs the individual departures would otherwise drive. */
    failed.type = PMIX_PROC_RANK;
    failed.size = camp->ntargets;
    failed.array = malloc(camp->ntargets * sizeof(pmix_rank_t));
    if (NULL != failed.array) {
        fr = (pmix_rank_t *) failed.array;
        for (t = 0; t < camp->ntargets; t++) {
            fr[t] = camp->targets[t];
        }
        prte_rml_repair_routing_tree(&failed, false, /* epoch = */ 0);
        free(failed.array);
        failed.array = NULL;
    }

    /* per-target HNP teardown: mark the daemon gone and detach its node from the
     * DVM's usable set so a released job cannot remap onto it.  This mirrors the
     * comm-failure bookkeeping (unset ALIVE, set state, decrement num_daemons)
     * plus the node detach the grow-rollback path uses. */
    if (NULL != daemons) {
        for (t = 0; t < camp->ntargets; t++) {
            dproc = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs,
                                                                camp->targets[t]);
            if (NULL == dproc) {
                continue;
            }
            node = dproc->node;
            release_node_procs(node);
            if (PRTE_FLAG_TEST(dproc, PRTE_PROC_FLAG_ALIVE)) {
                PRTE_FLAG_UNSET(dproc, PRTE_PROC_FLAG_ALIVE);
                dproc->state = PRTE_PROC_STATE_TERMINATED;
                if (0 < prte_process_info.num_daemons) {
                    --prte_process_info.num_daemons;
                }
            }
            if (NULL != node) {
                if (NULL != node->session && node->session != prte_default_session) {
                    node->session = NULL;
                }
                if (node->daemon == dproc) {
                    node->daemon = NULL;
                    PMIX_RELEASE(dproc);
                }
                /* return the node to a pristine, never-launched state so a later
                 * grow can relaunch a daemon on it (clears the launch flags and
                 * drops it from the daemon-job map) */
                prte_plm_base_reset_dvm_node(node);
            }
        }
    }

    /* the campaign has drained: drop the fence it raised, give the RAS modules
     * their release hook, tell the requester the DVM now reflects the new size,
     * and release any jobs held behind the fence once it reaches zero. */
    prte_dvm_launch_fence -= camp->pending;
    prte_ras_base_shrink_complete(camp);
    if (camp->have_requester) {
        prte_plm_base_dvm_mod_notify(&camp->requester, camp->alloc_id,
                                     camp->req_id, true, PMIX_SUCCESS);
    }
    pmix_list_remove_item(&prte_shrink_campaigns, &camp->super);
    if (0 == prte_dvm_launch_fence) {
        prte_plm_base_fence_release();
    }
    PMIX_RELEASE(camp);
}

/* grpcomm xcast-completion callback: fires on the master, inside the xcast call
 * stack, once the whole DVM has received the shrink broadcast.  Keep it trivial
 * — thread-shift the real teardown onto a fresh event, because that teardown
 * drives routing-tree fault handlers that must not run nested inside grpcomm. */
static void shrink_xcast_complete(void *cbdata)
{
    prte_shrink_campaign_t *camp = (prte_shrink_campaign_t *) cbdata;
    prte_event_set(prte_event_base, &camp->ev, -1, PRTE_EV_WRITE,
                   shrink_campaign_complete, camp);
    PMIX_POST_OBJECT(camp);
    prte_event_active(&camp->ev, PRTE_EV_WRITE, 1);
}

/* monotonic counter used to mint unique session ids for reservations that
 * the host (rather than a scheduler) must identify on its own. */
static uint32_t prte_ras_reservation_counter = 0;

/* Create a new reservation owned by the given namespace. Returns NULL on
 * failure. alloc_id (scheduler-assigned PMIX_ALLOC_ID) and req_id
 * (requester PMIX_ALLOC_REQ_ID) are optional. */
static prte_session_t *create_reservation(const char *nspace, uint8_t inherit,
                                          pmix_proc_t *requestor,
                                          const char *alloc_id, const char *req_id)
{
    prte_session_t *s;
    prte_job_t *ownerjob;
    int rc;

    s = PMIX_NEW(prte_session_t);
    if (NULL == s) {
        return NULL;
    }
    /* assign a fresh, unique session id (UINT32_MAX is reserved for the
     * default session) */
    do {
        s->session_id = ++prte_ras_reservation_counter;
    } while (UINT32_MAX == s->session_id ||
             NULL != prte_get_session_object(s->session_id));
    /* prefer a scheduler-assigned allocation id; otherwise mint one */
    if (NULL != alloc_id) {
        s->alloc_refid = strdup(alloc_id);
    } else {
        pmix_asprintf(&s->alloc_refid, "%s.%u", nspace, s->session_id);
    }
    if (NULL != req_id) {
        s->user_refid = strdup(req_id);
    }
    s->flags |= PRTE_SESSION_FLAG_RESERVED | PRTE_SESSION_FLAG_DYNAMIC;
    PMIX_LOAD_NSPACE(s->owner, nspace);
    s->inheritance = inherit;
    if (NULL != requestor) {
        PMIX_XFER_PROCID(&s->requestor, requestor);
    }
    /* retain the owning namespace's job object so its children subtree stays
     * walkable for CHILD-flavored drain; NULL for a tool with no job object */
    ownerjob = prte_get_job_data_object(nspace);
    if (NULL != ownerjob) {
        PMIX_RETAIN(ownerjob);
        s->owner_job = ownerjob;
        /* Record the user as well as the namespace. A tool's namespace lives
         * only as long as the command that made it, so a reservation
         * identified by namespace alone becomes unusable - by anyone - the
         * moment the requester exits. See prte_session_is_owned_by. */
        s->owner_uid = ownerjob->uid;
    }
    /* seed the owner set with the owning namespace */
    prte_session_add_owner(s, nspace);

    rc = prte_set_session_object(s);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return NULL;
    }
    return s;
}

/* Register the named nodes with the destination reservation: set each node's
 * session backpointer and store a retained reference in the reservation. The
 * node objects themselves live in the global pool, which is why this takes the
 * node *names* and resolves them with prte_node_match: the caller's working
 * list has already been drained into the pool by prte_ras_base_node_insert, so
 * the names must be snapshotted before that insert and handed in here. */
static void add_nodes_to_session(char **names, prte_session_t *dest)
{
    prte_node_t *gnode;
    int k, i;
    bool present;

    if (NULL == dest || dest == prte_default_session || NULL == names) {
        return;
    }
    for (i = 0; NULL != names[i]; i++) {
        gnode = prte_node_match(NULL, names[i]);
        if (NULL == gnode) {
            continue;
        }
        gnode->session = dest;
        /* avoid a double reference on EXTEND */
        present = false;
        for (k = 0; k < dest->nodes->size; k++) {
            if (gnode == (prte_node_t *) pmix_pointer_array_get_item(dest->nodes, k)) {
                present = true;
                break;
            }
        }
        if (!present) {
            PMIX_RETAIN(gnode);
            pmix_pointer_array_add(dest->nodes, gnode);
        }
    }
}

void prte_ras_base_teardown_reservation(prte_session_t *session,
                                        bool return_to_scheduler)
{
    prte_node_t *nd;
    int k;
    pmix_rank_t *ranks = NULL;
    int32_t m = 0;
    pmix_data_buffer_t msg;
    prte_daemon_cmd_flag_t cmd = PRTE_DAEMON_SHRINK_CMD;
    pmix_status_t rc;

    if (NULL == session || session == prte_default_session) {
        return;
    }

    /* if the nodes are being returned to the scheduler, collect the daemon
     * ranks of the member nodes BEFORE detaching so the DVM can shrink them
     * out. The shrink machinery terminates any jobs/daemons on those nodes. */
    if (return_to_scheduler) {
        ranks = (pmix_rank_t *) malloc(session->nodes->size * sizeof(pmix_rank_t));
        if (NULL != ranks) {
            for (k = 0; k < session->nodes->size; k++) {
                nd = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, k);
                if (NULL == nd || NULL == nd->daemon) {
                    continue;
                }
                /* Never shrink ourselves out of our own DVM. A reservation may
                 * legitimately include the head node - a single-node DVM whose
                 * whole allocation is carved into one session is the extreme
                 * case - and ordering the master's own daemon to depart takes
                 * the DVM down with it, losing every other job still running
                 * on it. The head node simply reverts to the general pool. */
                if (nd->daemon->name.rank == PRTE_PROC_MY_NAME->rank) {
                    continue;
                }
                ranks[m++] = nd->daemon->name.rank;
            }
        }
    }

    /* clear the reservation's hold on its nodes BEFORE any shrink so the
     * reservation bookkeeping does not race the shrink-campaign accounting.
     * Each member node reverts to the default pool. */
    for (k = 0; k < session->nodes->size; k++) {
        nd = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, k);
        if (NULL == nd) {
            continue;
        }
        if (nd->session == session) {
            nd->session = NULL;
        }
        pmix_pointer_array_set_item(session->nodes, k, NULL);
        PMIX_RELEASE(nd);
    }

    /* the reservation no longer withholds nodes */
    session->flags &= ~PRTE_SESSION_FLAG_RESERVED;
    if (NULL != session->owners) {
        PMIx_Argv_free(session->owners);
        session->owners = NULL;
    }
    /* drop the retained owning-job reference taken in create_reservation */
    if (NULL != session->owner_job) {
        PMIX_RELEASE(session->owner_job);
        session->owner_job = NULL;
    }

    /* deregister so the reservation can no longer be looked up / targeted.
     * The session object itself is left for any still-running jobs that
     * reference it (via session->jobs) and is reclaimed at DVM teardown. */
    if (0 <= session->index) {
        pmix_pointer_array_set_item(prte_sessions, session->index, NULL);
        session->index = -1;
    }

    if (return_to_scheduler && NULL != ranks && 0 < m) {
        PMIX_DATA_BUFFER_CONSTRUCT(&msg);
        rc = PMIx_Data_pack(NULL, &msg, &cmd, 1, PMIX_UINT8);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_pack(NULL, &msg, &m, 1, PMIX_INT32);
        }
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_pack(NULL, &msg, ranks, m, PMIX_PROC_RANK);
        }
        if (PMIX_SUCCESS == rc) {
            if (PRTE_SUCCESS != (rc = prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, &msg))) {
                PRTE_ERROR_LOG(rc);
            }
        } else {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
    }
    if (NULL != ranks) {
        free(ranks);
    }
}

#if defined(PMIX_ALLOC_INHERIT_CHILD)
/* True if nspace is the root job's namespace or any transitive spawn
 * descendant of it (a recursive walk of prte_job_t::children). */
static bool job_subtree_contains(prte_job_t *root, const pmix_nspace_t nspace)
{
    prte_job_t *child;

    if (NULL == root) {
        return false;
    }
    if (PMIX_CHECK_NSPACE(root->nspace, nspace)) {
        return true;
    }
    PMIX_LIST_FOREACH(child, &root->children, prte_job_t) {
        if (job_subtree_contains(child, nspace)) {
            return true;
        }
    }
    return false;
}

/* True if any job in the subtree rooted at root is still running (has not yet
 * reached PRTE_JOB_STATE_TERMINATED). */
static bool job_subtree_running(prte_job_t *root)
{
    prte_job_t *child;

    if (NULL == root) {
        return false;
    }
    if (root->state < PRTE_JOB_STATE_TERMINATED) {
        return true;
    }
    PMIX_LIST_FOREACH(child, &root->children, prte_job_t) {
        if (job_subtree_running(child)) {
            return true;
        }
    }
    return false;
}

/* True if nspace is the reservation's owning namespace or one of its derived
 * children (the transitive spawn subtree). Roots at the retained owner_job
 * when present, else at the jobs spawned directly into the reservation (the
 * tool-owner case). */
static bool reservation_term_in_genealogy(prte_session_t *s,
                                          const pmix_nspace_t nspace)
{
    int k;
    prte_job_t *j;

    if (PMIX_CHECK_NSPACE(s->owner, nspace)) {
        return true;
    }
    if (NULL != s->owner_job) {
        return job_subtree_contains(s->owner_job, nspace);
    }
    for (k = 0; k < s->jobs->size; k++) {
        j = (prte_job_t *) pmix_pointer_array_get_item(s->jobs, k);
        if (NULL != j && job_subtree_contains(j, nspace)) {
            return true;
        }
    }
    return false;
}

/* True if the owning namespace or any of its derived children is still
 * running - i.e. the CHILD-flavored reservation has not yet drained. */
static bool reservation_has_running_descendant(prte_session_t *s)
{
    int k;
    prte_job_t *j;

    if (NULL != s->owner_job) {
        return job_subtree_running(s->owner_job);
    }
    for (k = 0; k < s->jobs->size; k++) {
        j = (prte_job_t *) pmix_pointer_array_get_item(s->jobs, k);
        if (NULL != j && job_subtree_running(j)) {
            return true;
        }
    }
    return false;
}
#endif

void prte_ras_base_check_reservations_on_term(prte_job_t *jdata)
{
    int i;
    prte_session_t *s;

    if (NULL == prte_sessions || NULL == jdata) {
        return;
    }

    for (i = 0; i < prte_sessions->size; i++) {
        s = (prte_session_t *) pmix_pointer_array_get_item(prte_sessions, i);
        if (NULL == s || s == prte_default_session) {
            continue;
        }
        if (!(s->flags & PRTE_SESSION_FLAG_RESERVED)) {
            continue;
        }
        /* a session separated from its parent (PMIX_SESSION_SEP) terminates
         * independently of the namespace that owns it, so no namespace
         * termination fires its inheritance disposition. It is released only
         * by an explicit PMIX_SESSION_TERMINATE or at DVM teardown. */
        if (s->flags & PRTE_SESSION_FLAG_DETACHED) {
            continue;
        }

        switch (s->inheritance) {
#if defined(PMIX_ALLOC_INHERIT_NONE)
        case PMIX_ALLOC_INHERIT_NONE:
            /* release to scheduler when the owning namespace terminates */
            if (PMIX_CHECK_NSPACE(s->owner, jdata->nspace)) {
                prte_ras_base_teardown_reservation(s, true);
            }
            break;
#endif
#if defined(PMIX_ALLOC_INHERIT_CHILD)
        case PMIX_ALLOC_INHERIT_CHILD:
            /* release to scheduler once the last derived child terminates */
            if (reservation_term_in_genealogy(s, jdata->nspace) &&
                !reservation_has_running_descendant(s)) {
                prte_ras_base_teardown_reservation(s, true);
            }
            break;
#endif
#if defined(PMIX_ALLOC_INHERIT_CHILD_DEFAULT)
        case PMIX_ALLOC_INHERIT_CHILD_DEFAULT:
            /* unreserve into the session once the last derived child terminates */
            if (reservation_term_in_genealogy(s, jdata->nspace) &&
                !reservation_has_running_descendant(s)) {
                prte_ras_base_teardown_reservation(s, false);
            }
            break;
#endif
        default:
            /* PMIX_ALLOC_INHERIT_DEFAULT (also the absent-attribute default):
             * unreserve into the session when the owning namespace terminates */
            if (PMIX_CHECK_NSPACE(s->owner, jdata->nspace)) {
                prte_ras_base_teardown_reservation(s, false);
            }
            break;
        }
    }
}

pmix_status_t prte_ras_base_parse_node_list(pmix_info_t *info, char **ndstring)
{
    pmix_status_t rc;
    char **nodes = NULL;

    *ndstring = NULL;
    if (PMIX_STRING == info->value.type ||
        PMIX_REGEX == info->value.type) {
        rc = pmix_preg.parse_nodes(info->value.data.string, &nodes);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        *ndstring = PMIx_Argv_join(nodes, ',');
        PMIx_Argv_free(nodes);
        if (NULL == *ndstring) {
            return PMIX_ERR_NOMEM;
        }
        return PMIX_SUCCESS;
    }

#if PRTE_PMIX_HAVE_REGEX2
    if (PMIX_REGEX2 == info->value.type) {
        /* this is a regex value identifying the nodes that were allocated by
         * the scheduler (may match what we requested) */
        rc = PMIx_parse_regex2(info->value.data.regex2, NULL, 0, ndstring);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }
#endif

    /* we only support those options */
    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t ras_base_prepare_grow(prte_pmix_server_req_t *req,
                                           prte_session_t **dest_out)
{
    size_t n;
    char *target = NULL;        /* PMIX_ALLOC_TARGET namespace, if any */
    bool share = false;         /* default: reserve (do not share) */
    bool have_share = false;
    uint8_t inherit = PRTE_INHERIT_DEFAULT_VALUE;
    bool have_inherit = false;
    char *alloc_id = NULL;      /* PMIX_ALLOC_ID (scheduler-assigned) */
    char *req_id = NULL;        /* PMIX_ALLOC_REQ_ID (user-provided) */
    const char *owner_nspace;
    prte_session_t *dest = NULL;
    prte_job_t *reqjob;
    bool is_tool;

    /* scan the request for the reservation-routing directives so we can
     * resolve which session the new nodes will join before inserting
     * them. The PMIX_ALLOC_NODE_LIST entries are handled in the loop
     * below; PMIX_ALLOC_WARN_TIMEOUT is intentionally left in req->info
     * to be forwarded verbatim to the scheduler. */
    for (n = 0; n < req->ninfo; n++) {
#if defined(PMIX_ALLOC_TARGET)
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_TARGET)) {
            target = req->info[n].value.data.string;
            continue;
        }
#endif
#if defined(PMIX_ALLOC_SHARE)
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_SHARE)) {
            share = PMIX_INFO_TRUE(&req->info[n]);
            have_share = true;
            continue;
        }
#endif
#if defined(PMIX_ALLOC_INHERITANCE)
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_INHERITANCE)) {
            inherit = req->info[n].value.data.uint8;
            have_inherit = true;
            continue;
        }
#endif
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_ID)) {
            alloc_id = req->info[n].value.data.string;
            continue;
        }
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_REQ_ID)) {
            req_id = req->info[n].value.data.string;
        }
    }
    (void) have_share;

#if !defined(PMIX_ALLOC_INHERITANCE)
    /* an inheritance value this build cannot honor is rejected rather than
     * silently dropped (it may have arrived over the wire from a newer
     * peer) */
    if (have_inherit && PRTE_INHERIT_DEFAULT_VALUE != inherit) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
#endif

    reqjob = prte_get_job_data_object(req->tproc.nspace);
    is_tool = (NULL == reqjob) || PRTE_FLAG_TEST(reqjob, PRTE_JOB_FLAG_TOOL);
    /* the namespace the reservation is created for / must be owned by */
    owner_nspace = (NULL != target) ? target : req->tproc.nspace;

    if (have_share && share) {
        dest = prte_default_session;            /* general use */
    } else if (NULL != target && !is_tool) {
        return PMIX_ERR_NO_PERMISSIONS;         /* app may not retarget */
    } else if (PMIX_ALLOC_EXTEND == req->allocdir) {
        /* extend an existing reservation named by either identifier */
        if (NULL == alloc_id && NULL == req_id) {
            return PMIX_ERR_BAD_PARAM;          /* nothing names the target */
        }
        dest = (NULL != alloc_id) ? prte_get_session_object_from_id(alloc_id)
                                  : NULL;
        if (NULL == dest && NULL != req_id) {
            dest = prte_get_session_object_from_refid(req_id);
        }
        if (NULL == dest ||
            !prte_session_is_owned_by(dest, req->tproc.nspace)) {
            return (NULL == dest) ? PMIX_ERR_NOT_FOUND : PMIX_ERR_NO_PERMISSIONS;
        }
        /* a new inheritance value on EXTEND updates the disposition */
        if (have_inherit) {
            dest->inheritance = inherit;
        }
        /* refresh the requestor so a timeout warning follows the most
         * recent requester */
        PMIX_XFER_PROCID(&dest->requestor, &req->tproc);
    } else {                                    /* PMIX_ALLOC_NEW */
        dest = create_reservation(owner_nspace, inherit, &req->tproc,
                                  alloc_id, req_id);
        if (NULL == dest) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
    }

    *dest_out = dest;
    return PMIX_SUCCESS;
}

int prte_ras_base_insert_node_string(char *ndstring, prte_session_t *dest)
{
    pmix_list_t ndlist;
    prte_node_t *snap;
    char **rsv_names = NULL;
    int ret;

    /* add these nodes to our node pool */
    PMIX_CONSTRUCT(&ndlist, pmix_list_t);
    ret = prte_util_add_dash_host_nodes(&ndlist, ndstring);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_LIST_DESTRUCT(&ndlist);
        return ret;
    }

    /* prte_ras_base_node_insert() drains ndlist into the global
     * pool, so snapshot the node names first: add_nodes_to_session()
     * below needs them to re-find the pool objects and set their
     * session backpointer (which carries the requestor for the
     * phase-two completion event). */
    PMIX_LIST_FOREACH(snap, &ndlist, prte_node_t) {
        PMIx_Argv_append_nosize(&rsv_names, snap->name);
        /* mark as newly added so the DVM extension will include it
         * despite any static -host filter given when the DVM was
         * started */
        snap->state = PRTE_NODE_STATE_ADDED;
    }

    ret = prte_ras_base_node_insert(&ndlist, NULL);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_LIST_DESTRUCT(&ndlist);
        PMIx_Argv_free(rsv_names);
        return ret;
    }

    /* when reserving, withhold these nodes from the default pool by
     * registering them with the destination session */
    add_nodes_to_session(rsv_names, dest);
    PMIx_Argv_free(rsv_names);
    PMIX_LIST_DESTRUCT(&ndlist);
    return PRTE_SUCCESS;
}

void prte_ras_base_activate_dvm_grow(void)
{
    prte_job_t *daemons;

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    /* mark that we need to extend the DVM */
    prte_set_attribute(&daemons->attributes, PRTE_JOB_EXTEND_DVM,
                       PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
    /* mark that an updated nidmap must be communicated to existing daemons */
    prte_nidmap_communicated = false;
    PRTE_ACTIVATE_JOB_STATE(daemons, PRTE_JOB_STATE_LAUNCH_DAEMONS);
}

static void ras_base_set_alloc_response(prte_pmix_server_req_t *req,
                                        prte_session_t *dest)
{
    pmix_info_t *rinfo;
    size_t rn;

    if (NULL == dest || dest == prte_default_session ||
        NULL == dest->alloc_refid) {
        return;
    }

    /* report the destination allocation id back to the requester so it can
     * later target, extend, or release the reservation. The default
     * (shared) session carries no allocation id, so nothing is reported. */
    rn = (NULL != dest->user_refid) ? 2 : 1;
    PMIX_INFO_CREATE(rinfo, rn);
    PMIX_INFO_LOAD(&rinfo[0], PMIX_ALLOC_ID, dest->alloc_refid, PMIX_STRING);
    if (2 == rn) {
        PMIX_INFO_LOAD(&rinfo[1], PMIX_ALLOC_REQ_ID, dest->user_refid,
                       PMIX_STRING);
    }
    /* Repoint to our response array and let the req destructor free it.
     * The original req->info is usually borrowed from the PMIx caller and
     * needs no action, but a request that already owns its array (one
     * relayed from a remote peer, or one an RM module replaced with a
     * scheduler answer) would strand it here. */
    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    req->info = rinfo;
    req->ninfo = rn;
    req->copy = true;
}

static void ras_base_complete_grow_request(prte_pmix_server_req_t *req)
{
    prte_session_t *dest = NULL;
    pmix_status_t rc;
    size_t n;
    bool found = false;

    /* an add-host/add-hostfile request (from the --add-host and
     * --add-hostfile cmd line options) grows the DVM's general pool.
     * The nodes were already inserted by the module that claimed the
     * request, and there is no reservation to route them to - so all
     * that remains is to extend the DVM across the new nodes. The
     * grow must be activated even if the request only adjusted slots
     * on existing nodes: initiating the request marked the DVM as
     * not-ready and parked the requesting job in the job cache, and
     * it is the VM_READY re-entry at the end of the (possibly empty)
     * daemon launch that marks the DVM ready again and releases the
     * cached jobs */
    for (n = 0; n < req->ninfo; n++) {
        if (PMIx_Check_key(req->info[n].key, PMIX_ADD_HOST) ||
            PMIx_Check_key(req->info[n].key, PMIX_ADD_HOSTFILE)) {
            prte_ras_base_activate_dvm_grow();
            return;
        }
    }

    rc = ras_base_prepare_grow(req, &dest);
    if (PMIX_SUCCESS != rc) {
        req->pstatus = rc;
        return;
    }

    for (n = 0; n < req->ninfo; n++) {
        char *ndstring = NULL;
        int ret;

        if (!PMIx_Check_key(req->info[n].key, PMIX_ALLOC_NODE_LIST)) {
            continue;
        }

        rc = prte_ras_base_parse_node_list(&req->info[n], &ndstring);
        if (PMIX_SUCCESS != rc) {
            req->pstatus = rc;
            return;
        }

        ret = prte_ras_base_insert_node_string(ndstring, dest);
        free(ndstring);
        if (PRTE_SUCCESS != ret) {
            req->pstatus = prte_pmix_convert_rc(ret);
            return;
        }
        found = true;
    }

    if (found) {
        prte_ras_base_activate_dvm_grow();
    }
    ras_base_set_alloc_response(req, dest);
}

static int ras_base_find_shrink_targets(char **nodes, pmix_rank_t **ranks_out,
                                        int32_t *nranks_out)
{
    pmix_rank_t *ranks;
    int32_t cnt, m;
    prte_node_t *node;

    cnt = PMIx_Argv_count(nodes);
    if (0 == cnt) {
        return PRTE_ERR_NOT_FOUND;
    }

    ranks = (pmix_rank_t *) malloc(cnt * sizeof(pmix_rank_t));
    if (NULL == ranks) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    m = 0;
    for (int32_t n = 0; NULL != nodes[n]; n++) {
        /* find this node in our global resource pool */
        node = prte_node_match(NULL, nodes[n]);
        if (NULL == node) {
            free(ranks);
            return PRTE_ERR_NOT_FOUND;
        }
        if (NULL == node->daemon) {
            /* node doesn't have a daemon yet */
            continue;
        }
        ranks[m++] = node->daemon->name.rank;
    }

    *ranks_out = ranks;
    *nranks_out = m;
    return PRTE_SUCCESS;
}

static int ras_base_create_shrink_campaign(prte_pmix_server_req_t *req,
                                           pmix_rank_t *ranks, int32_t nranks,
                                           prte_shrink_campaign_t **campaign)
{
    prte_shrink_campaign_t *camp;

    *campaign = NULL;
    if (!prte_elastic_mode || 0 >= nranks) {
        return PRTE_SUCCESS;
    }

    /* Record the shrink campaign before freeing the ranks array.  Only in
     * elastic mode: outside it the DVM is fixed-size, so the release still
     * xcasts the shrink command below but no campaign is recorded and the
     * fence is never raised — the legacy fire-and-forget behavior.  Also
     * skip when the release removes no daemons (nranks == 0): an empty
     * campaign would never drain (no target ever departs on the comm-failure
     * path), so prte_shrink_campaigns would stay non-empty forever and stall
     * every later job at the LAUNCH_APPS hold, and no completion event would
     * fire. This mirrors the grow path's num_new_daemons > 0 guard and the
     * spec's "no event when nothing changes" clause. */
    camp = PMIX_NEW(prte_shrink_campaign_t);
    if (NULL == camp) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    camp->targets = (pmix_rank_t *) malloc(nranks * sizeof(pmix_rank_t));
    if (NULL == camp->targets) {
        PMIX_RELEASE(camp);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    memcpy(camp->targets, ranks, nranks * sizeof(pmix_rank_t));
    camp->ntargets = nranks;
    camp->pending = nranks;
    /* record the requester so the phase-two completion event can be
     * directed at the process that issued this PMIX_ALLOC_RELEASE */
    PMIX_XFER_PROCID(&camp->requester, &req->tproc);
    for (size_t n = 0; n < req->ninfo; n++) {
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_ID)) {
            camp->alloc_id = strdup(req->info[n].value.data.string);
        } else if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_REQ_ID)) {
            camp->req_id = strdup(req->info[n].value.data.string);
        }
    }
    camp->have_requester = true;
    pmix_list_append(&prte_shrink_campaigns, &camp->super);
    prte_dvm_launch_fence += nranks;

    *campaign = camp;
    return PRTE_SUCCESS;
}

static void ras_base_abort_dvm_shrink(prte_shrink_campaign_t *camp,
                                      bool notify, pmix_status_t status)
{
    if (NULL == camp) {
        return;
    }

    pmix_list_remove_item(&prte_shrink_campaigns, &camp->super);
    prte_dvm_launch_fence -= camp->pending;
    if (notify && camp->have_requester) {
        prte_plm_base_dvm_mod_notify(&camp->requester, camp->alloc_id,
                                     camp->req_id, false, status);
    }
    PMIX_RELEASE(camp);
}

static int ras_base_send_dvm_shrink(prte_shrink_campaign_t *camp,
                                    pmix_rank_t *ranks, int32_t nranks,
                                    bool report_xcast_failure)
{
    pmix_data_buffer_t msg;
    prte_daemon_cmd_flag_t cmd = PRTE_DAEMON_SHRINK_CMD;
    pmix_status_t rc;

    /* create the request */
    PMIX_DATA_BUFFER_CONSTRUCT(&msg);
    /* pack the command */
    rc = PMIx_Data_pack(NULL, &msg, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS == rc) {
        /* pack the daemon ranks to be removed from DVM */
        rc = PMIx_Data_pack(NULL, &msg, &nranks, 1, PMIX_INT32);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &msg, ranks, nranks, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
        if (NULL != camp) {
            ras_base_abort_dvm_shrink(camp, false, PMIX_SUCCESS);
        }
        return prte_pmix_convert_status(rc);
    }

    /* goes to all daemons.  When a campaign was recorded, request a
     * completion callback so the collective shrink-completion handler runs
     * exactly once — when the whole DVM has received the command — driving a
     * single routing-tree repair and one completion event, rather than
     * discovering the departures one daemon at a time. */
    if (NULL != camp) {
        rc = prte_grpcomm_xcast_nb(PRTE_RML_TAG_DAEMON, &msg,
                                   shrink_xcast_complete, camp);
    } else {
        rc = prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON, &msg);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&msg);

    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        /* undo the campaign we just added (only if one was created), and
         * tell the requester the DVM modification failed */
        if (NULL != camp) {
            ras_base_abort_dvm_shrink(camp, true, prte_pmix_convert_rc(rc));
        }
        return report_xcast_failure ? rc : PRTE_SUCCESS;
    }

    return PRTE_SUCCESS;
}

int prte_ras_base_prepare_dvm_shrink(prte_pmix_server_req_t *req,
                                     pmix_rank_t *ranks, int32_t nranks,
                                     prte_shrink_campaign_t **campaign)
{
    if (NULL == campaign) {
        return PRTE_ERR_BAD_PARAM;
    }

    return ras_base_create_shrink_campaign(req, ranks, nranks, campaign);
}

int prte_ras_base_commit_dvm_shrink(prte_shrink_campaign_t *campaign)
{
    if (NULL == campaign) {
        return PRTE_ERR_BAD_PARAM;
    }

    return ras_base_send_dvm_shrink(campaign, campaign->targets,
                                   campaign->ntargets, true);
}

void prte_ras_base_abort_dvm_shrink(prte_shrink_campaign_t *campaign)
{
    ras_base_abort_dvm_shrink(campaign, false, PMIX_SUCCESS);
}

static int ras_base_start_dvm_shrink(prte_pmix_server_req_t *req,
                                     pmix_rank_t *ranks, int32_t nranks,
                                     prte_shrink_campaign_t **campaign,
                                     bool report_xcast_failure)
{
    prte_shrink_campaign_t *camp = NULL;
    int ret;

    if (NULL != campaign) {
        *campaign = NULL;
    }

    ret = ras_base_create_shrink_campaign(req, ranks, nranks, &camp);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    ret = ras_base_send_dvm_shrink(camp, ranks, nranks, report_xcast_failure);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    if (NULL != campaign) {
        *campaign = camp;
    }

    return PRTE_SUCCESS;
}

static bool ras_base_teardown_by_alloc_id(prte_pmix_server_req_t *req)
{
    char *rel_alloc_id = NULL;
    prte_session_t *rsession;

    for (size_t n = 0; n < req->ninfo; n++) {
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_ID)) {
            rel_alloc_id = req->info[n].value.data.string;
            break;
        }
    }
    if (NULL == rel_alloc_id) {
        return false;
    }

    /* a release naming an allocation id tears down that whole reservation:
     * any owner may release it, the scheduler may release any. The nodes
     * are returned to the scheduler (the legacy disposition for an explicit
     * release). */
    rsession = prte_get_session_object_from_id(rel_alloc_id);
    if (NULL == rsession || rsession == prte_default_session) {
        req->pstatus = PMIX_ERR_NOT_FOUND;
        return true;
    }
    if (!prte_session_is_owned_by(rsession, req->tproc.nspace)) {
        req->pstatus = PMIX_ERR_NO_PERMISSIONS;
        return true;
    }
    prte_ras_base_teardown_reservation(rsession, true);
    req->pstatus = PMIX_SUCCESS;
    return true;
}

static void ras_base_complete_release_request(prte_pmix_server_req_t *req)
{
    char *ndstring = NULL;
    char **nodes = NULL;
    pmix_rank_t *ranks = NULL;
    int32_t nranks = 0;
    pmix_status_t rc;
    int ret;

    if (ras_base_teardown_by_alloc_id(req)) {
        return;
    }

    for (size_t n = 0; n < req->ninfo; n++) {
        if (!PMIx_Check_key(req->info[n].key, PMIX_ALLOC_NODE_LIST)) {
            continue;
        }

        rc = prte_ras_base_parse_node_list(&req->info[n], &ndstring);
        if (PMIX_SUCCESS != rc) {
            req->pstatus = rc;
            return;
        }
        nodes = PMIx_Argv_split(ndstring, ',');
        free(ndstring);
        break;
    }

    if (NULL == nodes) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        req->pstatus = PMIX_ERR_NOT_FOUND;
        return;
    }

    ret = ras_base_find_shrink_targets(nodes, &ranks, &nranks);
    PMIx_Argv_free(nodes);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        req->pstatus = prte_pmix_convert_rc(ret);
        return;
    }

    ret = ras_base_start_dvm_shrink(req, ranks, nranks, NULL, false);
    free(ranks);
    if (PRTE_SUCCESS != ret) {
        req->pstatus = prte_pmix_convert_rc(ret);
    }
}

void prte_ras_base_complete_request(prte_pmix_server_req_t *req)
{
    if (PMIX_ALLOC_EXTEND == req->allocdir ||
        PMIX_ALLOC_NEW == req->allocdir) {
        ras_base_complete_grow_request(req);
    } else if (PMIX_ALLOC_RELEASE == req->allocdir) {
        ras_base_complete_release_request(req);
    }
}

/*
 * May an add-host/add-hostfile directive be honored here?
 *
 * Two different noes.  Under an allocation owned by an external resource
 * manager it is not PRRTE's to grant: only the scheduler can say which nodes
 * this DVM holds, and launching a daemon on one it did not grant does not
 * fail locally - the launch fails and takes the whole DVM with it.  That was
 * the behavior before this check existed.
 *
 * Otherwise the request is routed by name to the component that serves it, so
 * refuse if that component is not the active allocator.  A bootstrapped DVM
 * is the case that reaches this: its membership comes from a configuration
 * file every daemon reads, and ras/bootstrap outranks ras/hosts.
 */
static int ras_base_add_hosts_allowed(void)
{
    prte_ras_base_selected_module_t *mod;

    if (prte_ras_base.scheduler_owned) {
        prte_show_help("help-ras-base.txt", "ras-base:add-host-managed", true);
        return PRTE_ERR_NOT_SUPPORTED;
    }

    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
        if (0 == strcasecmp("hosts", mod->component->pmix_mca_component_name)) {
            return PRTE_SUCCESS;
        }
    }

    prte_show_help("help-ras-base.txt", "ras-base:add-host-unsupported", true);
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ras_base_add_hosts(prte_job_t *jdata)
{
    int i, rc;
    prte_app_context_t *app;
    char *hosts, **hostfiles, **addhosts, *tmp;
    prte_pmix_server_req_t *req;

    hostfiles = NULL;
    addhosts = NULL;

    // see if we have any add-hostfile or add-host directives
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE,
                       (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            // found one
            PMIx_Argv_append_nosize(&hostfiles, hosts);
            free(hosts);
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOST,
                               (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            // found one
            PMIx_Argv_append_nosize(&addhosts, hosts);
            free(hosts);
        }
    }
    if (NULL == hostfiles && NULL == addhosts) {
        // there were no directives
        return PRTE_SUCCESS;
    }

    /* Can this be served at all?  Refuse here, synchronously, rather than
     * posting a request nothing will answer: a few lines below this marks the
     * DVM not-ready and the caller parks the job in the cache, and only the
     * grow's VM_READY re-entry releases it.  A request no module serves would
     * leave the job waiting on a DVM that never becomes ready again. */
    rc = ras_base_add_hosts_allowed();
    if (PRTE_SUCCESS != rc) {
        PMIx_Argv_free(hostfiles);
        PMIx_Argv_free(addhosts);
        return rc;
    }

    // create an allocation request tracker
    req = PMIX_NEW(prte_pmix_server_req_t);
    req->key = strdup("hosts");
    req->operation = strdup("ADDHOSTS");
    req->allocdir = PMIX_ALLOC_EXTEND;
    /* the request OWNS whatever job object it carries - its destructor
     * releases it - and this one is not ours to give away: the job is
     * still on its way to launch, and the parent's child list is holding
     * the only other reference to it.  Every other producer of a request
     * hands over a job it just created (PRTE_SPN_REQ); we are borrowing a
     * live one, so take a reference of our own for the request to drop. */
    PMIX_RETAIN(jdata);
    req->jdata = jdata;
    if (NULL != hostfiles) {
        req->ninfo++;
    }
    if (NULL != addhosts) {
        req->ninfo++;
    }
    req->copy = true;
    req->info = PMIx_Info_create(req->ninfo);
    i = 0;
    if (NULL != hostfiles) {
        tmp = PMIx_Argv_join(hostfiles, ',');
        PMIX_INFO_LOAD(&req->info[i], PMIX_ADD_HOSTFILE, tmp, PMIX_STRING);
        free(tmp);
        ++i;
        PMIx_Argv_free(hostfiles);
    }
    if (NULL != addhosts) {
        tmp = PMIx_Argv_join(addhosts, ',');
        PMIX_INFO_LOAD(&req->info[i], PMIX_ADD_HOST, tmp, PMIX_STRING);
        free(tmp);
        ++i;
        PMIx_Argv_free(addhosts);
    }
    /* add this request to our local request tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    // pass this to the RAS framework for handling
    prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, prte_ras_base_modify, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&req->ev, PRTE_EV_WRITE, 1);

    // mark that the DVM is not ready so the launch does not continue
    // until we have processed the nodes
    prte_dvm_ready = false;

    return PRTE_SUCCESS;
}

/*
 * --activate: bring nodes the allocation already contains into the DVM.
 *
 * The node pool holds every node the allocation contains, but only those the
 * DVM was started across carry a daemon: a --host/--hostfile given to "prte"
 * narrows which pool entries get one, and a released reservation hands its
 * nodes back to the pool without one.  Such a node is up, allocated, and
 * unreachable - nothing in the DVM can use it, and until now nothing could
 * ask for it back.
 *
 * This is deliberately weaker than --add-host, and that is what makes it
 * safe where add-host is not.  It adds nothing to the allocation, changes no
 * slot count, and asks no scheduler for anything: it can only name nodes the
 * allocation already contains, so it is permitted even when a resource
 * manager owns the allocation.  All it does is mark the chosen pool entries
 * PRTE_NODE_STATE_ADDED - which is precisely what every other producer of a
 * grow does - and let the ordinary DVM extension launch daemons on them.
 */

/* Is this pool entry already part of the DVM?  Pool entry 0 is the HNP's own
 * node, which is in the DVM by definition - and which the grow loop in
 * prte_plm_base_setup_virtual_machine() starts past, so it could not be a
 * target even if it somehow carried no daemon. */
static bool ras_base_in_dvm(prte_node_t *node)
{
    return (NULL != node->daemon || 0 == node->index);
}

/* May a daemon be started on this pool entry?  A node already in the DVM is
 * not activatable but is not an error either - see below. */
static bool ras_base_activatable(prte_node_t *node)
{
    if (ras_base_in_dvm(node)) {
        return false;
    }
    /* ADDED means some other pending grow has already claimed it; UP is the
     * ordinary idle case.  Every other state says the node is not to be used
     * (DOWN, REBOOT, DO_NOT_USE) or has been taken back by the scheduler
     * (NOT_INCLUDED), and a daemon launched on one of those fails - taking
     * the DVM with it, since a daemon that cannot start is fatal. */
    return (PRTE_NODE_STATE_UP == node->state || PRTE_NODE_STATE_ADDED == node->state);
}

/* Read a non-negative decimal index, refusing anything that is not wholly
 * digits.  parse_dash_host() takes strtol's answer unchecked, so "+nabc"
 * quietly means "+n0" there; here that would silently activate a node
 * nobody named and report success. */
static bool ras_base_read_count(const char *str, int *val)
{
    char *endp = NULL;
    long v;

    if (NULL == str || '\0' == *str) {
        return false;
    }
    v = strtol(str, &endp, 10);
    if (NULL == endp || '\0' != *endp || 0 > v || INT_MAX < v) {
        return false;
    }
    *val = (int) v;
    return true;
}

/* Is this node already in the selection? */
static bool ras_base_activate_selected(pmix_pointer_array_t *sel, prte_node_t *node)
{
    int i;
    prte_node_t *nptr;

    for (i = 0; i < sel->size; i++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(sel, i);
        if (nptr == node) {
            return true;
        }
    }
    return false;
}

static int ras_base_activate_select(pmix_pointer_array_t *sel, prte_node_t *node,
                                    const char *token)
{
    if (ras_base_in_dvm(node)) {
        /* already in the DVM.  Not an error: --activate states what the DVM's
         * membership is to include, and for this node it already does.  A
         * relative token ("+e:2") never lands here - it selects only from
         * daemon-less entries - so this is the user naming a node twice, or
         * naming one they were not sure about. */
        return PRTE_SUCCESS;
    }
    if (!ras_base_activatable(node)) {
        prte_show_help("help-ras-base.txt", "ras-base:activate-unavailable", true,
                       node->name, prte_node_state_to_str(node->state), token);
        return PRTE_ERR_SILENT;
    }
    if (!ras_base_activate_selected(sel, node)) {
        pmix_pointer_array_add(sel, node);
    }
    return PRTE_SUCCESS;
}

/*
 * Resolve a "file=" token: read the hostfile, then look every name it holds
 * up in the node pool.  The parsed nodes are throwaway objects describing
 * what the file said; what goes into the selection is the pool's own entry
 * for each, which is the only thing a grow can act on.
 */
static int ras_base_activate_hostfile(char *hostfile, pmix_pointer_array_t *sel)
{
    pmix_list_t nodes;
    prte_node_t *nd, *node;
    int rc;

    if ('\0' == *hostfile) {
        prte_show_help("help-ras-base.txt", "ras-base:activate-nofile", true);
        return PRTE_ERR_SILENT;
    }

    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    /* the parser reports its own failures - a file it cannot open, a bad
     * line, relative syntax inside a file - through show_help */
    rc = prte_util_add_hostfile_nodes(&nodes, hostfile);
    if (PRTE_SUCCESS != rc) {
        PMIX_LIST_DESTRUCT(&nodes);
        return (PRTE_ERR_SILENT == rc) ? rc : PRTE_ERR_SILENT;
    }

    PMIX_LIST_FOREACH(nd, &nodes, prte_node_t) {
        node = prte_node_match(NULL, nd->name);
        if (NULL == node) {
            prte_show_help("help-ras-base.txt", "ras-base:activate-unknown-in-file", true,
                           nd->name, hostfile);
            PMIX_LIST_DESTRUCT(&nodes);
            return PRTE_ERR_SILENT;
        }
        rc = ras_base_activate_select(sel, node, nd->name);
        if (PRTE_SUCCESS != rc) {
            PMIX_LIST_DESTRUCT(&nodes);
            return rc;
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);

    return PRTE_SUCCESS;
}

/*
 * Resolve one --activate specification against the node pool, appending the
 * nodes it names to sel.  Nothing is modified here: a specification is either
 * wholly acceptable or wholly refused, so that a bad token in a later app
 * segment cannot leave nodes marked for a grow that never happens.
 *
 * The syntax is --host's, minus what activate cannot honor:
 *   node01,node02   - name the nodes directly
 *   +all            - every allocated node that is not in the DVM
 *   +n<K>           - the K'th node of the allocation
 *   file=<path>     - a hostfile, read exactly as --hostfile reads one
 * A ":<slots>" modifier is refused rather than ignored (see below), and so
 * is "+e" - see there for why it cannot mean anything useful here.
 */
static int ras_base_activate_spec(char *spec, pmix_pointer_array_t *sel)
{
    char **tokens;
    int rc = PRTE_SUCCESS;
    int k, n, nodeidx;
    prte_node_t *node;

    tokens = PMIx_Argv_split(spec, ',');
    if (NULL == tokens) {
        return PRTE_SUCCESS;
    }

    for (k = 0; NULL != tokens[k]; k++) {
        if (0 == strncmp(tokens[k], "file=", 5)) {
            /* a hostfile, in exactly the format --hostfile reads - which is
             * also what makes it useful here: the file that described the
             * allocation to begin with can be handed straight back.  Only
             * the node NAMES are taken from it.  A "slots=" in the file is
             * not applied, for the same reason the ":N" form below is
             * refused, and this is how a hostfile given to a tool already
             * behaves: it selects, it does not resize.
             *
             * The parser is the real one, so ^exclusion, aliases and its
             * refusal of relative syntax inside a file all come along. */
            rc = ras_base_activate_hostfile(&tokens[k][5], sel);
            if (PRTE_SUCCESS != rc) {
                goto done;
            }

        } else if ('+' != tokens[k][0]) {
            /* an explicit node name.  A ":N" slot modifier is refused, not
             * quietly dropped: activate has no authority to change what the
             * allocation grants, and a request that appears to have been
             * honored but silently was not is worse than a refusal.  Use
             * --add-host where PRRTE owns the allocation and the slot count
             * really is PRRTE's to change. */
            if (NULL != strchr(tokens[k], ':')) {
                prte_show_help("help-ras-base.txt", "ras-base:activate-slots", true, tokens[k]);
                rc = PRTE_ERR_SILENT;
                goto done;
            }
            node = prte_node_match(NULL, tokens[k]);
            if (NULL == node) {
                prte_show_help("help-ras-base.txt", "ras-base:activate-unknown", true, tokens[k]);
                rc = PRTE_ERR_SILENT;
                goto done;
            }
            rc = ras_base_activate_select(sel, node, tokens[k]);
            if (PRTE_SUCCESS != rc) {
                goto done;
            }

        } else if (0 == strcasecmp(&tokens[k][1], "all")) {
            /* every allocated node that is not in the DVM.  Finding none is
             * not an error: "activate all" is a statement about what the
             * DVM's membership should include, and if it already includes
             * everything the allocation holds then it is satisfied. */
            for (n = 0; n < prte_node_pool->size; n++) {
                node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
                if (NULL == node || !ras_base_activatable(node)) {
                    continue;
                }
                if (!ras_base_activate_selected(sel, node)) {
                    pmix_pointer_array_add(sel, node);
                }
            }

        } else if ('n' == tokens[k][1] || 'N' == tokens[k][1]) {
            /* a specific node of the allocation, by index */
            if (!ras_base_read_count(&tokens[k][2], &nodeidx)) {
                prte_show_help("help-dash-host.txt", "dash-host:invalid-relative-node-syntax",
                               true, tokens[k]);
                rc = PRTE_ERR_SILENT;
                goto done;
            }
            /* pool entry 0 is the HNP's own node, which is part of the
             * allocation only when the resource manager included it - the
             * same offset parse_dash_host() applies to this syntax */
            if (!prte_hnp_is_allocated) {
                ++nodeidx;
            }
            if (nodeidx >= prte_node_pool->size) {
                prte_show_help("help-dash-host.txt", "dash-host:relative-node-out-of-bounds",
                               true, nodeidx, tokens[k]);
                rc = PRTE_ERR_SILENT;
                goto done;
            }
            node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, nodeidx);
            if (NULL == node) {
                prte_show_help("help-dash-host.txt", "dash-host:relative-node-not-found",
                               true, nodeidx, tokens[k]);
                rc = PRTE_ERR_SILENT;
                goto done;
            }
            rc = ras_base_activate_select(sel, node, tokens[k]);
            if (PRTE_SUCCESS != rc) {
                goto done;
            }

        } else if ('e' == tokens[k][1] || 'E' == tokens[k][1]) {
            /* "+e" is valid --host syntax, so it gets its own refusal rather
             * than the generic "that is not relative node syntax".  It means
             * "nodes with no application process running on them", which is
             * not a question about DVM membership at all: the nodes it picks
             * are mostly ones the DVM is already on, so honoring it here
             * would launch nothing and report success. */
            prte_show_help("help-ras-base.txt", "ras-base:activate-empty", true, tokens[k]);
            rc = PRTE_ERR_SILENT;
            goto done;

        } else {
            prte_show_help("help-dash-host.txt", "dash-host:invalid-relative-node-syntax",
                           true, tokens[k]);
            rc = PRTE_ERR_SILENT;
            goto done;
        }
    }

done:
    PMIx_Argv_free(tokens);
    return rc;
}

int prte_ras_base_activate_hosts(prte_job_t *jdata)
{
    int i, rc, nactivated;
    prte_app_context_t *app;
    char *spec;
    prte_node_t *node;
    pmix_pointer_array_t sel;
    bool found = false, adding = false;

    PMIX_CONSTRUCT(&sel, pmix_pointer_array_t);
    pmix_pointer_array_init(&sel, 8, INT_MAX, 8);

    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }
        /* note whether this same request also grows the allocation - it
         * decides who activates the grow, below */
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOST, NULL, PMIX_STRING) ||
            prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE, NULL, PMIX_STRING)) {
            adding = true;
        }
        spec = NULL;
        if (!prte_get_attribute(&app->attributes, PRTE_APP_ACTIVATE_HOSTS,
                                (void **) &spec, PMIX_STRING) ||
            NULL == spec) {
            continue;
        }
        found = true;
        rc = ras_base_activate_spec(spec, &sel);
        free(spec);
        if (PRTE_SUCCESS != rc) {
            PMIX_DESTRUCT(&sel);
            return rc;
        }
    }

    if (!found) {
        PMIX_DESTRUCT(&sel);
        return PRTE_SUCCESS;
    }

    /* every token resolved, so the selection can now be committed */
    nactivated = 0;
    for (i = 0; i < sel.size; i++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(&sel, i);
        if (NULL == node) {
            continue;
        }
        if (PRTE_NODE_STATE_ADDED == node->state) {
            /* already claimed by a pending grow, which will launch it */
            continue;
        }
        node->state = PRTE_NODE_STATE_ADDED;
        ++nactivated;
    }
    PMIX_DESTRUCT(&sel);

    if (0 == nactivated) {
        /* every named node is already in the DVM, or already on its way in.
         * There is nothing to launch, so leave the DVM ready and let the job
         * proceed - marking it not-ready here would park the job waiting on
         * a grow that never runs. */
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:activate_hosts nothing to activate",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:base:activate_hosts activating %d node%s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nactivated,
                         (1 == nactivated) ? "" : "s"));

    if (adding) {
        /* This same request carries --add-host/--add-hostfile, and
         * prte_ras_base_add_hosts() has already posted the asynchronous
         * request that inserts those nodes and then activates a grow.  That
         * grow launches on every node marked PRTE_NODE_STATE_ADDED, ours
         * included, so it is the one to wait for.  Activating a second grow
         * here would race ahead of the insertion and extend the DVM before
         * the added nodes existed. */
        return PRTE_SUCCESS;
    }

    /* mark that the DVM is not ready so the launch does not continue until
     * the new daemons are up - the VM_READY re-entry at the end of the grow
     * marks it ready again and releases the job from the cache */
    prte_dvm_ready = false;
    prte_ras_base_activate_dvm_grow();

    return PRTE_SUCCESS;
}
