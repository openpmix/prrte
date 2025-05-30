/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2024      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void qrel(void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

static void _query(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_pmix_server_op_caddy_t *rcd;
    pmix_query_t *q;
    pmix_status_t ret = PMIX_SUCCESS;
    void *results, *plist, *stack, *cache;
    prte_info_item_t *kv;
    pmix_nspace_t jobid;
    prte_job_t *jdata;
    prte_node_t *node, *ndptr;
    int j, k, rc;
    size_t m, n, p;
    uint32_t key, nodeid, sessionid = UINT32_MAX;
    char **nspaces, *hostname, *uri;
    char *cmdline;
    char **ans, *tmp;
    char *psetname;
    prte_app_context_t *app;
    int matched;
    pmix_proc_info_t *procinfo;
    pmix_data_array_t dry;
    prte_proc_t *proct;
    pmix_proc_t *proc;
    size_t sz;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s processing query",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_INFO_LIST_START(results);

    /* see what they wanted */
    for (m = 0; m < cd->nqueries; m++) {
        q = &cd->queries[m];
        hostname = NULL;
        nodeid = UINT32_MAX;
        psetname = NULL;
        /* default to the requestor's jobid */
        PMIX_LOAD_NSPACE(jobid, cd->proct.nspace);
        /* see if they provided any qualifiers */
        if (NULL != q->qualifiers && 0 < q->nqual) {
            for (n = 0; n < q->nqual; n++) {
                pmix_output_verbose(2, prte_pmix_server_globals.output,
                                    "%s qualifier key \"%s\" : value \"%s\"",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->qualifiers[n].key,
                                    (q->qualifiers[n].value.type == PMIX_STRING
                                         ? q->qualifiers[n].value.data.string
                                         : "(not a string)"));

                if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NSPACE)) {
                    // the nspace could be NULL, indicating that this is a
                    // wildcard request - if so, then ignore it here
                    if (NULL == q->qualifiers[n].value.data.string ||
                        0 == strlen(q->qualifiers[n].value.data.string)) {
                        PMIX_LOAD_NSPACE(jobid, NULL);
                        continue;
                    }
                    /* Never trust the namespace string that is provided.
                     * First check to see if we know about this namespace. If
                     * not then return an error. If so then continue on.
                     */
                    /* Make sure the qualifier namespace exists */
                    matched = 0;
                    for (k = 0; k < prte_job_data->size; k++) {
                        jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                        if (NULL != jdata) {
                            if (PMIX_CHECK_NSPACE(q->qualifiers[n].value.data.string, jdata->nspace)) {
                                matched = 1;
                                break;
                            }
                        }
                    }
                    if (0 == matched) {
                        pmix_output_verbose(2, prte_pmix_server_globals.output,
                                            "%s qualifier key \"%s\" : value \"%s\" is an unknown namespace",
                                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->qualifiers[n].key,
                                            q->qualifiers[n].value.data.string);
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                    PMIX_LOAD_NSPACE(jobid, jdata->nspace);
                    if (PMIX_NSPACE_INVALID(jobid)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_GROUP_ID)) {
                    /* Never trust the group string that is provided.
                     * First check to see if we know about this group. If
                     * not then return an error. If so then continue on.
                     */
                    /* Make sure the qualifier group exists */
                    matched = 0;
                    pmix_server_pset_t *ps;
                    PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, pmix_server_pset_t)
                    {
                        if (PMIX_CHECK_NSPACE(q->qualifiers[n].value.data.string, ps->name)) {
                            matched = 1;
                            break;
                        }
                    }
                    if (0 == matched) {
                        pmix_output_verbose(2, prte_pmix_server_globals.output,
                                            "%s qualifier key \"%s\" : value \"%s\" is an unknown group",
                                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->qualifiers[n].key,
                                            q->qualifiers[n].value.data.string);
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    PMIX_LOAD_NSPACE(jobid, q->qualifiers[n].value.data.string);
                    if (PMIX_NSPACE_INVALID(jobid)) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_HOSTNAME)) {
                    hostname = q->qualifiers[n].value.data.string;

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NODEID)) {
                    PMIX_VALUE_GET_NUMBER(rc, &q->qualifiers[n].value, nodeid, uint32_t);

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_PSET_NAME)) {
                    psetname = q->qualifiers[n].value.data.string;

                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_SESSION_ID)) {
                    PMIX_VALUE_GET_NUMBER(rc, &q->qualifiers[n].value, sessionid, uint32_t);

                }

            }
        }
        for (n = 0; NULL != q->keys[n]; n++) {
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s processing key %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->keys[n]);

            if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACES)) {
                /* get the current jobids */
                nspaces = NULL;
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    /* don't show the requestor's job */
                    if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)) {
                        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&nspaces, jdata->nspace);
                    }
                }
                /* join the results into a single comma-delimited string */
                tmp = PMIX_ARGV_JOIN_COMPAT(nspaces, ',');
                PMIX_ARGV_FREE_COMPAT(nspaces);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACE_INFO)) {
                PMIX_INFO_LIST_START(cache);
                /* get the current jobids */
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    // if the session ID was given, then ignore jobs not from that session
                    if (UINT32_MAX != sessionid && jdata->session->session_id != sessionid) {
                        continue;
                    }
                    /* don't show the requestor's job */
                    if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)) {
                        PMIX_INFO_LIST_START(stack);
                        /* add the nspace name */
                        PMIX_INFO_LIST_ADD(rc, stack, PMIX_NSPACE, jdata->nspace, PMIX_STRING);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(stack);
                            goto done;
                        }
                        /* add the cmd line */
                        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
                        if (NULL == app) {
                            ret = PMIX_ERR_NOT_FOUND;
                            goto done;
                        }
                        cmdline = PMIX_ARGV_JOIN_COMPAT(app->argv, ' ');
                        PMIX_INFO_LIST_ADD(rc, stack, PMIX_CMD_LINE, cmdline, PMIX_STRING);
                        free(cmdline);
                        /* add the job size */
                        PMIX_INFO_LIST_ADD(rc, stack, PMIX_JOB_SIZE, &jdata->num_procs, PMIX_UINT32);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(stack);
                            goto done;
                        }
                        /* construct info on each process in the job */
                        for (j=0; j < jdata->procs->size; j++) {
                            proct = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, j);
                            if (NULL == proct) {
                                continue;
                            }
                            PMIX_INFO_LIST_START(plist);
                            /* add the proc's rank */
                            PMIX_INFO_LIST_ADD(rc, plist, PMIX_RANK, &proct->name.rank, PMIX_PROC_RANK);
                            if (PMIX_SUCCESS != rc) {
                                PMIX_ERROR_LOG(rc);
                                PMIX_INFO_LIST_RELEASE(stack);
                                PMIX_INFO_LIST_RELEASE(plist);
                                goto done;
                            }
                            /* add the proc's hostname */
                            PMIX_INFO_LIST_ADD(rc, plist, PMIX_HOSTNAME, proct->node->name, PMIX_STRING);
                            if (PMIX_SUCCESS != rc) {
                                PMIX_ERROR_LOG(rc);
                                PMIX_INFO_LIST_RELEASE(stack);
                                PMIX_INFO_LIST_RELEASE(plist);
                                goto done;
                            }
                            /* add the proc's local rank */
                            PMIX_INFO_LIST_ADD(rc, plist, PMIX_LOCAL_RANK, &proct->local_rank, PMIX_UINT16);
                            if (PMIX_SUCCESS != rc) {
                                PMIX_ERROR_LOG(rc);
                                PMIX_INFO_LIST_RELEASE(stack);
                                PMIX_INFO_LIST_RELEASE(plist);
                                goto done;
                            }
                            /* add to the stack */
                            PMIX_INFO_LIST_CONVERT(rc, plist, &dry);
                            if (PMIX_SUCCESS != rc) {
                                PMIX_ERROR_LOG(rc);
                                PMIX_INFO_LIST_RELEASE(stack);
                                PMIX_INFO_LIST_RELEASE(plist);
                                goto done;
                            }
                            PMIX_INFO_LIST_RELEASE(plist);
                            PMIX_INFO_LIST_ADD(rc, stack, PMIX_PROC_INFO_ARRAY, &dry, PMIX_DATA_ARRAY);
                            PMIX_DATA_ARRAY_DESTRUCT(&dry);
                        }
                        /* add the result to our cache */
                        PMIX_INFO_LIST_CONVERT(rc, stack, &dry);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_INFO_LIST_RELEASE(stack);
                            goto done;
                        }
                        PMIX_INFO_LIST_RELEASE(stack);
                        PMIX_INFO_LIST_ADD(rc, cache, PMIX_JOB_INFO_ARRAY, &dry, PMIX_DATA_ARRAY);
                        PMIX_DATA_ARRAY_DESTRUCT(&dry);
                    }
                }
                /* add our findings to the results */
                PMIX_INFO_LIST_CONVERT(rc, cache, &dry);
                if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
                    // if the array is empty, then there is nothing wrong - we
                    // simply didn't find any runnning jobs
                    // otherwise, report the error and abort
                    PMIX_ERROR_LOG(rc);
                    PMIX_INFO_LIST_RELEASE(cache);
                    goto done;
                }
                PMIX_INFO_LIST_RELEASE(cache);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NAMESPACE_INFO, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_SPAWN_SUPPORT)) {
                ans = NULL;
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_HOST);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_HOSTFILE);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_ADD_HOST);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_ADD_HOSTFILE);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_PREFIX);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_WDIR);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_MAPPER);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_PPR);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_MAPBY);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_RANKBY);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_BINDTO);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_COSPAWN_APP);
                /* create the return kv */
                tmp = PMIX_ARGV_JOIN_COMPAT(ans, ',');
                PMIX_ARGV_FREE_COMPAT(ans);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_DEBUG_STOP_IN_INIT);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_DEBUG_STOP_IN_APP);
#if PRTE_HAVE_STOP_ON_EXEC
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_DEBUG_STOP_ON_EXEC);
#endif
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, PMIX_DEBUG_TARGET);
                /* create the return kv */
                tmp = PMIX_ARGV_JOIN_COMPAT(ans, ',');
                PMIX_ARGV_FREE_COMPAT(ans);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V1)) {
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    kv = PMIX_NEW(prte_info_item_t);
                    /* get it from the v2 API */
                    if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len,
                                                             HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
                        PMIX_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LIST_ADD(rc, results, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        goto done;
                    }
                }

            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V2)) {
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    kv = PMIX_NEW(prte_info_item_t);
                    if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len, 0)) {
                        PMIX_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LIST_ADD(rc, results, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        goto done;
                    }
                }

            } else if (0 == strcmp(q->keys[n], PMIX_PROC_URI)) {
                /* they want our URI */
                PMIX_INFO_LIST_ADD(rc, results, PMIX_PROC_URI, prte_process_info.my_hnp_uri, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_SERVER_URI)) {
                /* they want the PMIx URI */
                if (NULL != hostname) {
                    /* find the node object */
                    node = NULL;
                    for (k = 0; k < prte_node_pool->size; k++) {
                        ndptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, k);
                        if (NULL == ndptr) {
                            continue;
                        }
                        if (0 == strcmp(hostname, ndptr->name)) {
                            node = ndptr;
                            break;
                        }
                    }
                    if (NULL == node) {
                        /* unknown node */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    /* we want the info for the server on that node */
                    if (NULL == node->daemon) {
                        /* not found */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    proct = node->daemon;
                } else if (UINT32_MAX != nodeid) {
                    /* get the node object at that index */
                    node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, nodeid);
                    if (NULL == node) {
                        /* bad index */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    /* we want the info for the server on that node */
                    if (NULL == node->daemon) {
                        /* not found */
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                    proct = node->daemon;
                } else {
                    /* send them ours */
                    proct = prte_get_proc_object(PRTE_PROC_MY_NAME);
                }
                /* get the server uri value - we can block here as we are in
                 * an PRTE progress thread */
                PRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_SERVER_URI, &proct->name,
                                               (char **) &uri, PMIX_STRING);
                if (PRTE_SUCCESS != rc) {
                    ret = prte_pmix_convert_rc(rc);
                    goto done;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_SERVER_URI, uri, PMIX_STRING);
                free(uri);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PROC_TABLE)) {
                /* construct a list of values with prte_proc_info_t
                 * entries for each proc in the indicated job */
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* Check if there are any entries in global proctable */
                if (0 == jdata->num_procs) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CONSTRUCT(&dry, jdata->num_procs, PMIX_PROC_INFO);
                procinfo = (pmix_proc_info_t*)dry.array;
                p = 0;
                for (k = 0; k < jdata->procs->size; k++) {
                    proct = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, k);
                    if (NULL == proct) {
                        continue;
                    }
                    PMIX_LOAD_PROCID(&procinfo[p].proc, proct->name.nspace, proct->name.rank);
                    if (NULL != proct->node && NULL != proct->node->name) {
                        procinfo[p].hostname = strdup(proct->node->name);
                    }
                    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps,
                                                                             proct->app_idx);
                    if (NULL != app && NULL != app->app) {
                        if (pmix_path_is_absolute(app->app)) {
                            procinfo[p].executable_name = strdup(app->app);
                        } else {
                            procinfo[p].executable_name = pmix_os_path(false, app->cwd, app->app, NULL);
                        }
                    }
                    procinfo[p].pid = proct->pid;
                    procinfo[p].exit_code = proct->exit_code;
                    procinfo[p].state = prte_pmix_convert_state(proct->state);
                    ++p;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_PROC_TABLE, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_LOCAL_PROC_TABLE)) {
                /* construct a list of values with prte_proc_info_t
                 * entries for each LOCAL proc in the indicated job */
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* Check if there are any entries in local proctable */
                if (0 == jdata->num_local_procs) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CONSTRUCT(&dry, jdata->num_local_procs, PMIX_PROC_INFO);
                procinfo = (pmix_proc_info_t *) dry.array;
                p = 0;
                for (k = 0; k < jdata->procs->size; k++) {
                    proct = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, k);
                    if (NULL == proct) {
                        continue;
                    }
                    if (PRTE_FLAG_TEST(proct, PRTE_PROC_FLAG_LOCAL)) {
                        PMIX_LOAD_PROCID(&procinfo[p].proc, proct->name.nspace, proct->name.rank);
                        if (NULL != proct->node && NULL != proct->node->name) {
                            procinfo[p].hostname = strdup(proct->node->name);
                        }
                        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps,
                                                                                 proct->app_idx);
                        if (NULL != app && NULL != app->app) {
                            if (pmix_path_is_absolute(app->app)) {
                                procinfo[p].executable_name = strdup(app->app);
                            } else {
                                procinfo[p].executable_name = pmix_os_path(false, app->cwd, app->app, NULL);
                            }
                        }
                        procinfo[p].pid = proct->pid;
                        procinfo[p].exit_code = proct->exit_code;
                        procinfo[p].state = prte_pmix_convert_state(proct->state);
                        ++p;
                    }
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_LOCAL_PROC_TABLE, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_PSETS)) {
                sz = pmix_list_get_size(&prte_pmix_server_globals.psets);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NUM_PSETS, &sz, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PSET_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.psets, pmix_server_pset_t)
                {
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, ps->name);
                }
                if (NULL == ans) {
                    tmp = NULL;;
                } else {
                    tmp = PMIX_ARGV_JOIN_COMPAT(ans, ',');
                    PMIX_ARGV_FREE_COMPAT(ans);
                    ans = NULL;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_PSET_NAMES, tmp, PMIX_STRING);
                if (NULL != tmp) {
                    free(tmp);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PSET_MEMBERSHIP)) {
                pmix_server_pset_t *ps, *psptr;
                /* must have provided us with a pset name qualifier */
                if (NULL == psetname) {
                    ret = PMIX_ERR_BAD_PARAM;
                    goto done;
                }
                ans = NULL;
                /* find the referenced pset */
                psptr = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.psets, pmix_server_pset_t) {
                    if (0 == strcmp(psetname, ps->name)) {
                        psptr = ps;
                        break;
                    }
                }
                if (NULL == psptr) {
                    /* we don't know that pset */
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* define the array that holds the membership - no need to allocate anything if we are careful */
                dry.array = psptr->members;
                dry.type = PMIX_PROC;
                dry.size = psptr->num_members;
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_PSET_MEMBERSHIP, &dry, PMIX_DATA_ARRAY);
                dry.array = NULL;  /* say no to array destructor freeing the pset members array */
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_JOB_SIZE)) {
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                key = jdata->num_procs;
                PMIX_INFO_LIST_ADD(rc, results, PMIX_JOB_SIZE, &key, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_GROUPS)) {
                sz = pmix_list_get_size(&prte_pmix_server_globals.groups);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_NUM_GROUPS, &sz, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_GROUP_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, pmix_server_pset_t)
                {
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, ps->name);
                }
                tmp = PMIX_ARGV_JOIN_COMPAT(ans, ',');
                PMIX_ARGV_FREE_COMPAT(ans);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_GROUP_NAMES, tmp, PMIX_STRING);
                free(tmp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_GROUP_MEMBERSHIP)) {
                /* construct a list of values with pmix_proc_t
                 * entries for each proc in the indicated group */
                pmix_server_pset_t *ps, *grp = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, pmix_server_pset_t)
                {
                    if (PMIX_CHECK_NSPACE(ps->name, jobid)) {
                        grp = ps;
                        break;
                    }
                }
                if (NULL == grp) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* Check if there are any entries in the group */
                if (0 == grp->num_members) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CONSTRUCT(&dry, grp->num_members, PMIX_PROC);
                proc = (pmix_proc_t *) dry.array;
                memcpy(proc, grp->members, grp->num_members * sizeof(pmix_proc_t));
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_GROUP_MEMBERSHIP, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }

#if PMIX_NUMERIC_VERSION >= 0x00050000
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_ALLOCATION)) {
                /* collect all the node info */
                void *nodelist, *nodeinfolist;
                char *str;
                prte_topology_t *topo;
                int len;

                PMIX_INFO_LIST_START(nodelist);
                p = 0;
                for (k=0; k < prte_node_pool->size; k++) {
                    node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, k);
                    if (NULL == node) {
                        continue;
                    }
                    PMIX_INFO_LIST_START(nodeinfolist);
                    /* start with the node name */
                    PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_HOSTNAME, node->name, PMIX_STRING);
                    /* add any aliases */
                    if (NULL != node->aliases) {
                        str = PMIX_ARGV_JOIN_COMPAT(node->aliases, ',');
                        PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_HOSTNAME_ALIASES, str, PMIX_STRING);
                        free(str);
                    }
                    /* add topology index */
                    PMIX_INFO_LIST_ADD(rc, nodeinfolist, PMIX_TOPOLOGY_INDEX, &node->topology->index, PMIX_INT);
                    /* convert to array */
                    PMIX_INFO_LIST_CONVERT(rc, nodeinfolist, &dry);
                    PMIX_INFO_LIST_RELEASE(nodeinfolist);
                    /* now add the entry to the main list */
                    PMIX_INFO_LIST_ADD(rc, nodelist, PMIX_NODE_INFO, &dry, PMIX_DATA_ARRAY);
                    ++p;
                    PMIX_DATA_ARRAY_DESTRUCT(&dry);
                }
                /* add topology info */
                for (k=0; k < prte_node_topologies->size; k++) {
                    topo = (prte_topology_t*)pmix_pointer_array_get_item(prte_node_topologies, k);
                    if (NULL == topo) {
                        continue;
                    }
                    /* convert the topology to XML representation */
                    if (0 != hwloc_topology_export_xmlbuffer(topo->topo, &str, &len, 0)) {
                        continue;
                    }
                    PMIX_INFO_LIST_ADD(rc, nodelist, PMIX_HWLOC_XML_V2, str, PMIX_STRING);
                    free(str);
                }
                /* convert list to array */
                PMIX_INFO_LIST_CONVERT(rc, nodelist, &dry);
                PMIX_INFO_LIST_RELEASE(nodelist);
                /* add to results */
                PMIX_INFO_LIST_ADD(rc, nodelist, PMIX_QUERY_ALLOCATION, &dry, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_DESTRUCT(&dry);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
#endif

#ifdef PMIX_QUERY_AVAILABLE_SLOTS
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_AVAILABLE_SLOTS)) {
                /* compute the slots currently available for assignment. Note that
                 * this is purely a point-in-time measurement as jobs may be working
                 * there way thru the state machine for mapping, and more jobs may
                 * be submitted at any moment.
                 */
                p = 0;
                for (k=0; k < prte_node_pool->size; k++) {
                    node = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, k);
                    if (NULL == node) {
                        continue;
                    }
                    /* ignore nodes that are non-usable */
                    if (PRTE_FLAG_TEST(node, PRTE_NODE_NON_USABLE)) {
                        continue;
                    }
                    // ignore nodes that are down
                    if (PRTE_NODE_STATE_DOWN == node->state) {
                        continue;
                    }
                    // ignore nodes that are at/above max
                    if (0 != node->slots_max && node->slots_inuse >= node->slots_max) {
                        continue;
                    }
                    /* if the hnp was not allocated, then ignore it here */
                    if (!prte_hnp_is_allocated && 0 == node->index) {
                            continue;
                    }
                    // ignore oversubscribed nodes
                    if (node->slots <= node->slots_inuse) {
                        continue;
                    }
                    p += node->slots - node->slots_inuse;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_AVAILABLE_SLOTS, &p, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
#endif

#ifdef PMIX_MEM_ALLOC_KIND
            } else if (0 == strcmp(q->keys[n], PMIX_MEM_ALLOC_KIND)) {
                pmix_proc_t pproc;
                pmix_info_t info;
                pmix_value_t *value;
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                PMIX_LOAD_PROCID(&pproc, jobid, PMIX_RANK_WILDCARD);
                PMIX_INFO_LOAD(&info, PMIX_IMMEDIATE, NULL, PMIX_BOOL);
                ret = PMIx_Get(&pproc, PMIX_MEM_ALLOC_KIND, &info, 1, &value);
                if (PMIX_SUCCESS != ret) {
                    goto done;
                }
                PMIX_INFO_LIST_ADD(rc, results, PMIX_MEM_ALLOC_KIND, value->data.string, PMIX_STRING);
                PMIX_VALUE_RELEASE(value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto done;
                }
#endif

#ifdef PMIX_QUERY_RESOLVE_PEERS
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_RESOLVE_PEERS)) {
                char *nm;
                pmix_list_t procs;
                int idx;
                prte_proc_t *p2;
                bool found = false;
                // must at least have given us a hostname
                if (NULL == hostname) {
                    ret = PMIX_ERR_BAD_PARAM;
                    goto done;
                }
                /* does the name refer to me? */
                if (prte_check_host_is_local(hostname)) {
                    nm = prte_process_info.nodename;
                } else {
                    nm = hostname;
                }
                PMIX_CONSTRUCT(&procs, pmix_list_t);
                // could ask for info on all jobs, so have to check for that case
                for (k = 1; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    if (NULL == jdata->map) {
                        continue;
                    }
                    if (!PMIX_NSPACE_INVALID(jobid) &&
                        !PMIX_CHECK_NSPACE(jobid, jdata->nspace)) {
                        continue;
                    }
                    // see if this job has any procs on the indicated node
                    for (j=0; j < jdata->map->nodes->size; j++) {
                        node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, j);
                        if (NULL == node) {
                            continue;
                        }
                        if (0 != strcmp(node->name, nm)) {
                            if (NULL == node->aliases) {
                                continue;
                            }
                            found = false;
                            for (p = 0; NULL != node->aliases[p]; p++) {
                                if (0 == strcmp(nm, node->aliases[p])) {
                                    /* this is the node! */
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                continue;
                            }
                        }
                        // we want the procs from this node
                        for (idx=0; idx < node->procs->size; idx++) {
                            proct = (prte_proc_t*)pmix_pointer_array_get_item(node->procs, idx);
                            if (NULL == proct) {
                                continue;
                            }
                            if (PMIX_CHECK_NSPACE(jdata->nspace, proct->name.nspace)) {
                                pmix_list_append(&procs, &proct->super);
                            }
                        }
                    }
                }
                sz = pmix_list_get_size(&procs);
                if (0 < sz) {
                    PMIX_PROC_CREATE(proc, sz);
                    idx = 0;
                    PMIX_LIST_FOREACH_SAFE(proct, p2, &procs, prte_proc_t) {
                        memcpy(&proc[idx], &proct->name, sizeof(pmix_proc_t));
                        pmix_list_remove_item(&procs, &proct->super);
                        ++idx;
                    }
                } else {
                    proc = NULL;
                }
                dry.type = PMIX_PROC;
                dry.array = proc;
                dry.size = sz;
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_RESOLVE_PEERS, &dry, PMIX_DATA_ARRAY);
                if (NULL != proc) {
                    free(proc);
                }
                PMIX_DESTRUCT(&procs);
#endif

#ifdef PMIX_QUERY_RESOLVE_NODE
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_RESOLVE_NODE)) {
                char **nodes = NULL, *nodelist;
                // could ask for info on all jobs, so have to check for that case
                for (k = 1; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    if (NULL == jdata->map) {
                        continue;
                    }
                    if (!PMIX_NSPACE_INVALID(jobid) &&
                        !PMIX_CHECK_NSPACE(jobid, jdata->nspace)) {
                        continue;
                    }
                    // assemble the nodes
                    for (j=0; j < jdata->map->nodes->size; j++) {
                        node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, j);
                        if (NULL == node) {
                            continue;
                        }
                        PMIx_Argv_append_unique_nosize(&nodes, node->name);
                    }
                }
                nodelist = PMIx_Argv_join(nodes, ',');
                PMIx_Argv_free(nodes);
                PMIX_INFO_LIST_ADD(rc, results, PMIX_QUERY_RESOLVE_NODE, nodelist, PMIX_STRING);
#endif

            } else {
                pmix_output_verbose(2, prte_pmix_server_globals.output,
                                    "%s Query for unrecognized attribute: %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    PMIx_Get_attribute_name(q->keys[n]));
                ret = PMIX_ERR_NOT_SUPPORTED;
                goto done;
            }
        } // for
    }     // for

done:
    rcd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    PMIX_INFO_LIST_CONVERT(rc, results, &dry);
    if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    PMIX_INFO_LIST_RELEASE(results);
    if (PMIX_ERR_NOT_SUPPORTED != ret) {
        if (PMIX_ERR_EMPTY == rc) {
            ret = PMIX_ERR_NOT_FOUND;
        } else if (PMIX_SUCCESS == ret) {
            if (0 == dry.size) {
                ret = PMIX_ERR_NOT_FOUND;
            } else {
                if (dry.size < cd->ninfo) {
                    ret = PMIX_QUERY_PARTIAL_SUCCESS;
                } else {
                    ret = PMIX_SUCCESS;
                }
            }
        }
    }
    rcd->ninfo = dry.size;
    rcd->info = (pmix_info_t*)dry.array;
    // memory allocated in the data array will be free'd when rcd is released
    cd->infocbfunc(ret, rcd->info, rcd->ninfo, cd->cbdata, qrel, rcd);
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_query_fn(pmix_proc_t *proct, pmix_query_t *queries, size_t nqueries,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    if (NULL == queries || NULL == cbfunc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    memcpy(&cd->proct, proct, sizeof(pmix_proc_t));
    cd->queries = queries;
    cd->nqueries = nqueries;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _query, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}
