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
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
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
    prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, tmp2);
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

    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
        // give each module an opportunity to try to make the allocation
        if (NULL == mod->module->allocate) {
            continue;
        }
        rc = mod->module->allocate(jdata, &nodes);
        if (PRTE_SUCCESS == rc) {
            // got an allocation, so we are done
            goto DISPLAY;
        }
        if (PRTE_ERR_ALLOCATION_PENDING == rc) {
            /* an allocation request is underway, so just do nothing */
            PMIX_DESTRUCT(&nodes);
            PMIX_RELEASE(caddy);
            return;
        } else if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
            // this module didn't contribute anything
            continue;
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
    /* shall we display the results? */
    if (4 < pmix_output_get_verbosity(prte_ras_base_framework.framework_output)) {
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

static void localrelease(void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

void prte_ras_base_modify(int fd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    prte_job_t *daemons;
    prte_ras_base_selected_module_t *mod;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    // set the default response
    req->pstatus = PMIX_ERR_NOT_SUPPORTED;

    // cycle across the modules and give each a chance to execute request
    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules, prte_ras_base_selected_module_t) {
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

    // if we met the request, then we need to launch any new daemons
    if (PMIX_SUCCESS == req->pstatus) {
        daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        PRTE_ACTIVATE_JOB_STATE(daemons, PRTE_JOB_STATE_LAUNCH_DAEMONS);
    }

    // execute the callback
    if (NULL != req->infocbfunc) {
        req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata, localrelease, req);
        return;
    }

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);

    PMIX_RELEASE(req);
}

int prte_ras_base_add_hosts(prte_job_t *jdata)
{
    int rc;
    pmix_list_t nodes;
    int i, k, m, n, slots;
    prte_app_context_t *app;
    prte_node_t *node, *next, *nptr;
    char *hosts, *line, *cptr, *ptr, **hostfiles, *nm;
    FILE *fp;
    bool addslots, found;
    bool extend = false;
    int default_slots = -1;

    PMIX_CONSTRUCT(&nodes, pmix_list_t);

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
            free(hosts);
            goto proceed;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOST,
                               (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            // found one
            free(hosts);
            goto proceed;
        }
    }
    // if we get here, then there were no directives
    return PRTE_SUCCESS;

proceed:
    /* Individual add-hostfile names, if given, are included
     * in the app_contexts for this job. We therefore need to
     * retrieve the app_contexts for the job, and then cycle
     * through them to see if anything is there. The parser will
     * add the nodes found in each add-hostfile to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * in add-hostfiles from across all app_contexts
     *
     * Note that any relative node syntax found in the add-hostfiles will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */

    /* if we are in a managed allocation, the best we can do for nodes
     * that do not include a specific slot assignment is to (a) check
     * to see if there is a uniform assignment on existing nodes and
     * use that, or (b) generate an error as we cannot know what the
     * host environment might have set
     */
    if (prte_managed_allocation) {
        for (n = 0; n < prte_node_pool->size; n++) {
            nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
            if (NULL == nptr) {
                continue;
            }
            if (-1 == default_slots) {
                default_slots = nptr->slots;
                continue;
            }
            if (default_slots != nptr->slots) {
                // generate an error message
                pmix_show_help("help-ras-base.txt", "ras-base:nonuniform-slots", true,
                               default_slots, nptr->name, nptr->slots);
                PMIX_LIST_DESTRUCT(&nodes);
                return PRTE_ERR_SILENT;
            }
        }
    }

    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE,
                               (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:add_hosts checking add-hostfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

            prte_remove_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE);

            hostfiles = PMIx_Argv_split(hosts, ',');
            free(hosts);

            for (k=0; NULL != hostfiles[k]; k++) {
                /* hostfile was specified - parse it and add it to the list. We
                 * don't use the hostfile parsing code in src/util because it
                 * uses flex and that has problems handling the range of allowed
                 * syntax here */
                fp = fopen(hostfiles[k], "r");
                if (NULL == fp) {
                    pmix_show_help("help-ras-base.txt", "ras-base:addhost-not-found", true, hostfiles[k]);
                    PMIx_Argv_free(hostfiles);
                    PMIX_LIST_DESTRUCT(&nodes);
                    return PRTE_ERR_SILENT;
                }

                while (NULL != (line = pmix_getline(fp))) {
                    // ignore comments and blank lines
                    if (0 == strlen(line)) {
                        free(line);
                        continue;
                    }
                    // remove leading whitespace
                    cptr = line;
                    while (isspace(*cptr)) {
                        ++cptr;
                    }
                    if ('#' == *cptr) {
                        free(line);
                        continue;
                    }
                    addslots = false;
                    // because there can be arbitrary whitespace around keywords,
                    // we manually parse the line to get the directives
                    ptr = cptr;
                    while ('\0' != *ptr && !isspace(*ptr)) {
                        ++ptr;
                    }
                    if ('\0' == *ptr) {
                        // end of the line - just the node name was given
                        slots = default_slots;
                        goto process;
                    }
                    *ptr = '\0'; // terminate the name
                    // find the '=' sign
                    ++ptr;
                    while ('\0' != *ptr && ('=' != *ptr || isspace(*ptr))) {
                        ++ptr;
                    }
                    if ('\0' == *ptr) {
                        // didn't specify slots - use the default value
                        slots = default_slots;
                        goto process;
                    }
                    // find the value
                    ++ptr;
                    while ('\0' != *ptr && isspace(*ptr)) {
                        ++ptr;
                    }
                    if ('\0' == *ptr) {
                        // bad syntax
                        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                        fclose(fp);
                        free(line);
                        PMIx_Argv_free(hostfiles);
                        PMIX_LIST_DESTRUCT(&nodes);
                        return PRTE_ERR_SILENT;
                    }
                    // if it is a '+' or '-', then we are adjusting
                    // the #slots
                    if ('+' == *ptr || '-' == *ptr) {
                        addslots = true;
                    }
                    slots = strtol(ptr, NULL, 10);

            process:
                    // see if we have this node
                    found = false;
                    // does the name refer to me?
                        if (prte_check_host_is_local(cptr)) {
                            nm = prte_process_info.nodename;
                        } else {
                            nm = cptr;
                        }

                    for (n = 0; !found && n < prte_node_pool->size; n++) {
                        nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
                        if (NULL == nptr) {
                            continue;
                        }
                        if (0 == strcmp(nm, nptr->name)) {
                            // we have the node
                            if (addslots) {
                                nptr->slots += slots;
                                if (0 > nptr->slots) {
                                    nptr->slots = 0;
                                }
                            }
                            found = true;
                            break;
                        } else if (NULL != nptr->aliases) {
                            /* no choice but an exhaustive search - fortunately, these lists are short! */
                            for (m = 0; NULL != nptr->aliases[m]; m++) {
                                if (0 == strcmp(cptr, nptr->aliases[m])) {
                                    if (addslots) {
                                        nptr->slots += slots;
                                        if (0 > nptr->slots) {
                                            nptr->slots = 0;
                                        }
                                    }
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!found) {
                        // this is a new node - add it
                        node = PMIX_NEW(prte_node_t);
                        node->name = strdup(cptr);
                        node->slots = slots;
                        node->state = PRTE_NODE_STATE_ADDED;
                        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
                        pmix_list_append(&nodes, &node->super);
                    }
                    free(line);
                }
                fclose(fp);
            }
            PMIx_Argv_free(hostfiles);
        }
    }
    if (!pmix_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
        }
        /* mark that an updated nidmap must be communicated to existing daemons */
        prte_nidmap_communicated = false;
        extend = true;
    }

    /* We next check for and add any add-host options. Note this is
     * a -little- different than dash-host in that (a) we add these
     * nodes to the global pool (avoiding duplication),
     * and (b) as a result, any job and/or app_context can access them.
     *
     * Note that any relative node syntax found in the add-host lists will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOST,
                               (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            pmix_output_verbose(5, prte_ras_base_framework.framework_output,
                                "%s ras:base:add_hosts checking add-host %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts);
            if (PRTE_SUCCESS != (rc = prte_util_add_dash_host_nodes(&nodes, hosts, true))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DESTRUCT(&nodes);
                free(hosts);
                return rc;
            }
            prte_remove_attribute(&app->attributes, PRTE_APP_ADD_HOST);
            free(hosts);
        }
    }

    /* if something was found, we add that to our global pool */
    if (!pmix_list_is_empty(&nodes)) {
        /* the node insert code doesn't check for uniqueness, so we will
         * do so here - yes, this is an ugly, non-scalable loop, but this
         * is the exception case and so we can do it here */
        PMIX_LIST_FOREACH_SAFE(node, next, &nodes, prte_node_t)
        {
            node->state = PRTE_NODE_STATE_ADDED;
            found = false;
            for (n = 0; !found && n < prte_node_pool->size; n++) {
                nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
                if (NULL == nptr) {
                    continue;
                }
                if (0 == strcmp(node->name, nptr->name)) {
                    if (prte_get_attribute(&node->attributes, PRTE_NODE_ADD_SLOTS, NULL, PMIX_BOOL)) {
                        nptr->slots += node->slots;
                        prte_remove_attribute(&node->attributes, PRTE_NODE_ADD_SLOTS);
                    } else {
                        nptr->slots = node->slots;
                    }
                    pmix_list_remove_item(&nodes, &node->super);
                    PMIX_RELEASE(node);
                    found = true;
                } else if (NULL != nptr->aliases) {
                    /* no choice but an exhaustive search - fortunately, these lists are short! */
                    for (m = 0; !found && NULL != nptr->aliases[m]; m++) {
                        if (0 == strcmp(node->name, nptr->aliases[m])) {
                            if (prte_get_attribute(&node->attributes, PRTE_NODE_ADD_SLOTS, NULL, PMIX_BOOL)) {
                                nptr->slots += node->slots;
                                prte_remove_attribute(&node->attributes, PRTE_NODE_ADD_SLOTS);
                            } else {
                                nptr->slots = node->slots;
                            }
                            pmix_list_remove_item(&nodes, &node->super);
                            PMIX_RELEASE(node);
                            found = true;
                        }
                    }
                }
            }
        }
        if (!pmix_list_is_empty(&nodes)) {
            /* store the results in the global resource pool - this removes the
             * list items
             */
            if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
                PRTE_ERROR_LOG(rc);
            }
            /* mark that an updated nidmap must be communicated to existing daemons */
            prte_nidmap_communicated = false;
            extend = true;
        }
    }
    /* cleanup */
    PMIX_LIST_DESTRUCT(&nodes);

    if (extend) {
        // mark that we need to extend the DVM
        prte_set_attribute(&jdata->attributes, PRTE_JOB_EXTEND_DVM, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
    }

    /* shall we display the results? */
    if (0 < pmix_output_get_verbosity(prte_ras_base_framework.framework_output) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_ALLOC, NULL, PMIX_BOOL)) {
        prte_ras_base_display_alloc(jdata);
    }

    return PRTE_SUCCESS;
}
