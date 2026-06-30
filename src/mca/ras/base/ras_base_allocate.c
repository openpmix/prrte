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

#include <string.h>

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
    } else {
        prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, tmp2);
    }
    prte_set_attribute(&jdata->attributes, PRTE_JOB_ALLOC_DISPLAYED, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
}

static void display_cpus(prte_topology_t *t,
                         prte_job_t *jdata,
                         char *node)
{
    char tmp[2048];
    unsigned pkg, npkgs;
    bool bits_as_cores = false, use_hwthread_cpus = prte_hwloc_default_use_hwthread_cpus;
    unsigned npus, ncores;
    hwloc_obj_t obj;
    hwloc_cpuset_t avail = NULL;
    hwloc_cpuset_t allowed;
    hwloc_cpuset_t coreset = NULL;
    bool parsable;

    parsable = prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT, NULL, PMIX_BOOL);

    npus = prte_hwloc_base_get_nbobjs_by_type(t->topo, HWLOC_OBJ_PU);
    ncores = prte_hwloc_base_get_nbobjs_by_type(t->topo, HWLOC_OBJ_CORE);
    if (npus == ncores && !use_hwthread_cpus) {
        /* the bits in this bitmap represent cores */
        bits_as_cores = true;
    }
    use_hwthread_cpus = prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL);
    if (!use_hwthread_cpus && !bits_as_cores) {
        coreset = hwloc_bitmap_alloc();
    }
    avail = hwloc_bitmap_alloc();

    if (parsable) {
        pmix_output(prte_clean_output, "<processors node=%s>", node);
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
                pmix_output(prte_clean_output, "    <pkg=%d cpus=%s>", pkg, "NONE");
            } else {
                pmix_output(prte_clean_output, "PKG[%d]: NONE", pkg);
            }
            continue;
        }
        if (bits_as_cores) {
            /* can just use the hwloc fn directly */
            hwloc_bitmap_list_snprintf(tmp, 2048, avail);
             if (parsable) {
                pmix_output(prte_clean_output, "    <pkg=%d cpus=%s>", pkg, tmp);
            } else {
                pmix_output(prte_clean_output, "PKG[%d]: %s", pkg, tmp);
            }
        } else if (use_hwthread_cpus) {
            /* can just use the hwloc fn directly */
            hwloc_bitmap_list_snprintf(tmp, 2048, avail);
             if (parsable) {
                pmix_output(prte_clean_output, "    <pkg=%d cpus=%s>", pkg, tmp);
            } else {
                pmix_output(prte_clean_output, "PKG[%d]: %s", pkg, tmp);
            }
        } else {
            prte_hwloc_build_map(t->topo, avail, use_hwthread_cpus | bits_as_cores, coreset);
            /* now print out the string */
            hwloc_bitmap_list_snprintf(tmp, 2048, coreset);
             if (parsable) {
                pmix_output(prte_clean_output, "    <pkg=%d cpus=%s>", pkg, tmp);
            } else {
                pmix_output(prte_clean_output, "PKG[%d]: %s", pkg, tmp);
            }
        }
    }
    hwloc_bitmap_free(avail);
    if (NULL != coreset) {
        hwloc_bitmap_free(coreset);
    }
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
    for (j=0; NULL != nodes[j]; j++) {
        moveon = false;
        for (i=0; i < prte_node_pool->size && !moveon; i++) {
            nptr = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, i);
            if (NULL == nptr) {
                continue;
            }
            if (0 == strcmp(nptr->name, nodes[j])) {
                display_cpus(nptr->topology, jdata, nodes[j]);
                break;
            }
            if (NULL == nptr->aliases) {
                continue;
            }
            /* no choice but an exhaustive search - fortunately, these lists are short! */
            for (m = 0; NULL != nptr->aliases[m]; m++) {
                if (0 == strcmp(nodes[j], nptr->aliases[m])) {
                    /* this is the node! */
                    display_cpus(nptr->topology, jdata, nodes[j]);
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

    /* In an unmanaged allocation, the nodes discovered for the DVM's
     * initial (daemon-job) allocation constitute the fixed base allocation
     * for the entire session - exactly as if a scheduler had provided them.
     * Once that base has been established, a subsequent job (for example,
     * the child of a PMIx_Spawn) must not re-run discovery: doing so re-reads
     * the default hostfile and overwrites the established per-node slot counts
     * while clearing PRTE_NODE_FLAG_SLOTS_GIVEN. That in turn lets the node be
     * re-sized to its core count, which hides genuine oversubscription from
     * the mapper and causes spawned processes to be bound on a node that is
     * actually oversubscribed. The only sanctioned way to change an unmanaged
     * allocation is an explicit add-host/add-hostfile request, which is
     * handled separately (prte_ras_base_add_hosts -> prte_ras_base_modify)
     * before we ever reach this point. So if the base allocation already
     * exists and this is not the DVM's own daemon job, simply reuse it.
     *
     * The "established" test is deliberately independent of whether the HNP
     * node is part of the allocation (prte_ras_base.allocation_established is
     * set when the first allocation completes), so the protection holds even
     * for allocations that exclude the head node. */
    if (!prte_managed_allocation && prte_ras_base.allocation_established &&
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
            pmix_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
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
        if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PMIX_RELEASE(caddy);
        }
        PMIX_INFO_DESTRUCT(&info);
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
                if (NULL == node) {
                    continue;
                }
                pmix_output(prte_clean_output,
                            "=================================================================\n");
                pmix_output(prte_clean_output, "TOPOLOGY FOR NODE %s", node->name);
                prte_hwloc_print(&ptr, NULL, node->topology->topo);
                pmix_output(prte_clean_output, "%s", ptr);
                free(ptr);
                pmix_output(prte_clean_output,
                            "=================================================================\n");
            }
            PMIx_Argv_free(hostlist);
        } else {
            for (j=0; j < prte_node_pool->size; j++) {
                node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, j);
                if (NULL == node) {
                    continue;
                }
                pmix_output(prte_clean_output,
                            "=================================================================\n");
                pmix_output(prte_clean_output, "TOPOLOGY FOR NODE %s", node->name);
                prte_hwloc_print(&ptr, NULL, node->topology->topo);
                pmix_output(prte_clean_output, "%s", ptr);
                free(ptr);
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

static void localrelease(void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

void prte_ras_base_modify(int fd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    prte_ras_base_selected_module_t *mod;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

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

    // execute the callback
    if (NULL != req->infocbfunc) {
        req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata, localrelease, req);
        return;
    }

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);

    PMIX_RELEASE(req);
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

/* Register the nodes named in ndlist with the destination reservation: set
 * each node's session backpointer and store a retained reference in the
 * reservation. The node objects themselves live in the global pool. */
static void add_nodes_to_session(pmix_list_t *ndlist, prte_session_t *dest)
{
    prte_node_t *nd, *gnode;
    int k;
    bool present;

    if (NULL == dest || dest == prte_default_session) {
        return;
    }
    PMIX_LIST_FOREACH(nd, ndlist, prte_node_t) {
        gnode = prte_node_match(NULL, nd->name);
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
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &msg))) {
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

void prte_ras_base_complete_request(prte_pmix_server_req_t *req)
{
    prte_job_t *daemons;
    pmix_status_t rc;
    pmix_data_buffer_t msg;
    prte_daemon_cmd_flag_t cmd = PRTE_DAEMON_SHRINK_CMD;
    size_t n;
    char **nodes, *ndstring;
    int32_t cnt=0, m;
    int ret;
    prte_node_t *node;
    pmix_rank_t *ranks;
    pmix_list_t ndlist;
    bool found;
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

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (PMIX_ALLOC_EXTEND == req->allocdir ||
        PMIX_ALLOC_NEW == req->allocdir) {
        /* scan the request for the reservation-routing directives so we can
         * resolve which session the new nodes will join before inserting
         * them. The PMIX_ALLOC_NODE_LIST entries are handled in the loop
         * below; PMIX_ALLOC_WARN_TIMEOUT is intentionally left in req->info
         * to be forwarded verbatim to the scheduler. */
        for (n=0; n < req->ninfo; n++) {
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
                continue;
            }
        }
        (void) have_share;

        /* an inheritance value this build cannot honor is rejected rather than
         * silently dropped (it may have arrived over the wire from a newer
         * peer) */
#if !defined(PMIX_ALLOC_INHERITANCE)
        if (have_inherit && PRTE_INHERIT_DEFAULT_VALUE != inherit) {
            req->pstatus = PMIX_ERR_NOT_SUPPORTED;
            return;
        }
#endif

        reqjob = prte_get_job_data_object(req->tproc.nspace);
        is_tool = (NULL == reqjob) || PRTE_FLAG_TEST(reqjob, PRTE_JOB_FLAG_TOOL);
        /* the namespace the reservation is created for / must be owned by */
        owner_nspace = (NULL != target) ? target : req->tproc.nspace;

        if (have_share && share) {
            dest = prte_default_session;            /* general use */
        } else if (NULL != target && !is_tool) {
            req->pstatus = PMIX_ERR_NO_PERMISSIONS; /* app may not retarget */
            return;
        } else if (PMIX_ALLOC_EXTEND == req->allocdir) {
            /* extend an existing reservation named by either identifier */
            if (NULL == alloc_id && NULL == req_id) {
                req->pstatus = PMIX_ERR_BAD_PARAM;  /* nothing names the target */
                return;
            }
            dest = (NULL != alloc_id) ? prte_get_session_object_from_id(alloc_id)
                                      : NULL;
            if (NULL == dest && NULL != req_id) {
                dest = prte_get_session_object_from_refid(req_id);
            }
            if (NULL == dest ||
                !prte_session_is_owned_by(dest, req->tproc.nspace)) {
                req->pstatus = (NULL == dest) ? PMIX_ERR_NOT_FOUND
                                              : PMIX_ERR_NO_PERMISSIONS;
                return;
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
                req->pstatus = PMIX_ERR_OUT_OF_RESOURCE;
                return;
            }
        }

        found = false;
        for (n=0; n < req->ninfo; n++) {
            if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_NODE_LIST)) {
                if (PMIX_STRING == req->info[n].value.type ||
                    PMIX_REGEX == req->info[n].value.type) {
                    rc = pmix_preg.parse_nodes(req->info[n].value.data.string, &nodes);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        req->pstatus = rc;
                        return;
                    }
                    ndstring = PMIx_Argv_join(nodes, ',');
                    PMIx_Argv_free(nodes);

#if PRTE_PMIX_HAVE_REGEX2
                } else if (PMIX_REGEX2 == req->info[n].value.type) {
                    // this is a regex value identifying the nodes that were
                    // allocated by the scheduler (may match what we requested)
                    rc = PMIx_parse_regex2(req->info[n].value.data.regex2, NULL, 0, &ndstring);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        req->pstatus = rc;
                        return;
                    }
#endif

                } else {
                    // we only support those options
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    req->pstatus = PMIX_ERR_BAD_PARAM;
                    return;
                }
                // add these nodes to our node pool
                PMIX_CONSTRUCT(&ndlist, pmix_list_t);
                ret = prte_util_add_dash_host_nodes(&ndlist, ndstring, true);
                if (PRTE_SUCCESS != ret) {
                    PRTE_ERROR_LOG(ret);
                    req->pstatus = prte_pmix_convert_rc(ret);
                    free(ndstring);
                    PMIX_LIST_DESTRUCT(&ndlist);
                    return;
                }
                free(ndstring);
                ret = prte_ras_base_node_insert(&ndlist, NULL);
                if (PRTE_SUCCESS != ret) {
                    PRTE_ERROR_LOG(ret);
                    PMIX_LIST_DESTRUCT(&ndlist);
                    req->pstatus = prte_pmix_convert_rc(ret);
                    return;
                }
                /* when reserving, withhold these nodes from the default pool by
                 * registering them with the destination session */
                add_nodes_to_session(&ndlist, dest);
                PMIX_LIST_DESTRUCT(&ndlist);
                found = true;
            }
        }
        if (found) {
            // mark that we need to extend the DVM
            prte_set_attribute(&daemons->attributes, PRTE_JOB_EXTEND_DVM, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
            /* mark that an updated nidmap must be communicated to existing daemons */
            prte_nidmap_communicated = false;
            PRTE_ACTIVATE_JOB_STATE(daemons, PRTE_JOB_STATE_LAUNCH_DAEMONS);
        }

        /* report the destination allocation id back to the requester so it can
         * later target, extend, or release the reservation. The default
         * (shared) session carries no allocation id, so nothing is reported. */
        if (NULL != dest && dest != prte_default_session &&
            NULL != dest->alloc_refid) {
            pmix_info_t *rinfo;
            size_t rn = (NULL != dest->user_refid) ? 2 : 1;

            PMIX_INFO_CREATE(rinfo, rn);
            PMIX_INFO_LOAD(&rinfo[0], PMIX_ALLOC_ID, dest->alloc_refid, PMIX_STRING);
            if (2 == rn) {
                PMIX_INFO_LOAD(&rinfo[1], PMIX_ALLOC_REQ_ID, dest->user_refid, PMIX_STRING);
            }
            /* the original req->info is borrowed from the PMIx caller; repoint
             * to our response array and let the req destructor free it */
            req->info = rinfo;
            req->ninfo = rn;
            req->copy = true;
        }

    } else if (PMIX_ALLOC_RELEASE == req->allocdir) {

        /* a release naming an allocation id tears down that whole reservation:
         * any owner may release it, the scheduler may release any. The nodes
         * are returned to the scheduler (the legacy disposition for an explicit
         * release). */
        char *rel_alloc_id = NULL;
        for (n=0; n < req->ninfo; n++) {
            if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_ID)) {
                rel_alloc_id = req->info[n].value.data.string;
                break;
            }
        }
        if (NULL != rel_alloc_id) {
            prte_session_t *rsession = prte_get_session_object_from_id(rel_alloc_id);
            if (NULL == rsession || rsession == prte_default_session) {
                req->pstatus = PMIX_ERR_NOT_FOUND;
                return;
            }
            if (!prte_session_is_owned_by(rsession, req->tproc.nspace)) {
                req->pstatus = PMIX_ERR_NO_PERMISSIONS;
                return;
            }
            prte_ras_base_teardown_reservation(rsession, true);
            req->pstatus = PMIX_SUCCESS;
            return;
        }

        // create the request
        PMIX_DATA_BUFFER_CONSTRUCT(&msg);
        /* pack the command */
        rc = PMIx_Data_pack(NULL, &msg, &cmd, 1, PMIX_UINT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&msg);
            req->pstatus = rc;
            return;
        }
        // pack the daemon ranks to be removed from DVM
        for (n=0; n < req->ninfo; n++) {
            if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_NODE_LIST)) {
                if (PMIX_STRING == req->info[n].value.type ||
                    PMIX_REGEX == req->info[n].value.type) {
                    rc = pmix_preg.parse_nodes(req->info[n].value.data.string, &nodes);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        req->pstatus = rc;
                        return;
                    }
                    ndstring = PMIx_Argv_join(nodes, ',');
                    PMIx_Argv_free(nodes);

#if PRTE_PMIX_HAVE_REGEX2
                } else if (PMIX_REGEX2 == req->info[n].value.type) {
                    // this is a regex value identifying the nodes that were
                    // allocated by the scheduler (may match what we requested)
                    rc = PMIx_parse_regex2(req->info[n].value.data.regex2, NULL, 0, &ndstring);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        req->pstatus = prte_pmix_convert_rc(rc);
                        return;
                    }
#endif

                } else {
                    // we only support those two options
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    req->pstatus = PMIX_ERR_BAD_PARAM;
                    return;
                }
                nodes = PMIx_Argv_split(ndstring, ',');
                free(ndstring);
                cnt = PMIx_Argv_count(nodes);
                break;
            }
        }
        if (0 == cnt) {
            PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
            PMIX_DATA_BUFFER_DESTRUCT(&msg);
            req->pstatus = PMIX_ERR_NOT_FOUND;
            return;
        }
        // setup the array of ranks
        ranks = (pmix_rank_t*)malloc(cnt * sizeof(pmix_rank_t));
        m = 0;
        for (n=0; NULL != nodes[n]; n++) {
            // find this node in our global resource pool
            node = prte_node_match(NULL, nodes[n]);
            if (NULL == node) {
                PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
                PMIX_DATA_BUFFER_DESTRUCT(&msg);
                PMIx_Argv_free(nodes);
                free(ranks);
                req->pstatus = PMIX_ERR_NOT_FOUND;
                return;
            }
            if (NULL == node->daemon) {
                // node doesn't have a daemon yet
                continue;
            }
            ranks[m] = node->daemon->name.rank;
            ++m;
        }
        PMIx_Argv_free(nodes);
        rc = PMIx_Data_pack(NULL, &msg, &m, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&msg);
            free(ranks);
            req->pstatus = rc;
            return;
        }
        rc = PMIx_Data_pack(NULL, &msg, ranks, m, PMIX_PROC_RANK);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&msg);
            free(ranks);
            req->pstatus = rc;
            return;
        }
        /* Record the shrink campaign before freeing the ranks array.  Skip
         * entirely when the release removes no daemons (m == 0): an empty
         * campaign would never drain (no target ever departs on the comm-
         * failure path), so prte_shrink_campaigns would stay non-empty forever
         * and stall every later job at the LAUNCH_APPS hold, and no completion
         * event would fire.  This mirrors the grow path's num_new_daemons > 0
         * guard and the spec's "no event when nothing changes" clause. */
        if (0 < m) {
            prte_shrink_campaign_t *_camp = PMIX_NEW(prte_shrink_campaign_t);
            _camp->targets = (pmix_rank_t *) malloc(m * sizeof(pmix_rank_t));
            memcpy(_camp->targets, ranks, m * sizeof(pmix_rank_t));
            _camp->ntargets = m;
            _camp->pending  = m;
            /* record the requester so the phase-two completion event can be
             * directed at the process that issued this PMIX_ALLOC_RELEASE */
            PMIX_XFER_PROCID(&_camp->requester, &req->tproc);
            for (n = 0; n < req->ninfo; n++) {
                if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_ID)) {
                    _camp->alloc_id = strdup(req->info[n].value.data.string);
                } else if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_REQ_ID)) {
                    _camp->req_id = strdup(req->info[n].value.data.string);
                }
            }
            _camp->have_requester = true;
            pmix_list_append(&prte_shrink_campaigns, &_camp->super);
            prte_dvm_launch_fence += m;
        }
        free(ranks);

        /* goes to all daemons */
        if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &msg))) {
            PRTE_ERROR_LOG(rc);
            /* undo the campaign we just added (only if one was created), and
             * tell the requester the DVM modification failed */
            if (0 < m) {
                prte_shrink_campaign_t *_camp =
                    (prte_shrink_campaign_t *) pmix_list_remove_last(&prte_shrink_campaigns);
                prte_dvm_launch_fence -= _camp->pending;
                if (_camp->have_requester) {
                    prte_plm_base_dvm_mod_notify(&_camp->requester, _camp->alloc_id,
                                                 _camp->req_id, false,
                                                 prte_pmix_convert_rc(rc));
                }
                PMIX_RELEASE(_camp);
            }
        }
        PMIX_DATA_BUFFER_DESTRUCT(&msg);
    }

}

int prte_ras_base_add_hosts(prte_job_t *jdata)
{
    int i;
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

    // create an allocation request tracker
    req = PMIX_NEW(prte_pmix_server_req_t);
    req->key = strdup("hosts");
    req->operation = strdup("ADDHOSTS");
    req->allocdir = PMIX_ALLOC_EXTEND;
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
