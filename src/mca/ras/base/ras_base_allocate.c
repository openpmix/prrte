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

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (PMIX_ALLOC_EXTEND == req->allocdir ||
        PMIX_ALLOC_NEW == req->allocdir) {
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

    } else if (PMIX_ALLOC_RELEASE == req->allocdir) {

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
        free(ranks);

        /* goes to all daemons */
        if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON, &msg))) {
            PRTE_ERROR_LOG(rc);
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
