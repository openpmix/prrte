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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
    prte_info_item_t *kv;
    pmix_nspace_t jobid;
    prte_job_t *jdata;
    prte_node_t *node, *ndptr;
    int k, rc;
    pmix_list_t results, stack;
    size_t m, n, p;
    uint32_t key, nodeid;
    char **nspaces, *hostname, *uri;
    char *cmdline;
    char **ans, *tmp;
    char *psetname;
    prte_app_context_t *app;
    int matched;
    pmix_proc_info_t *procinfo;
    pmix_info_t *info;
    pmix_data_array_t *darray, dry;
    prte_proc_t *proct;
    pmix_proc_t *proc;
    size_t sz;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s processing query",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_CONSTRUCT(&results, pmix_list_t);

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

                    PMIX_LOAD_NSPACE(jobid, q->qualifiers[n].value.data.string);
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
                PMIX_CONSTRUCT(&stack, pmix_list_t);
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    /* don't show the requestor's job */
                    if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)) {
                        pmix_argv_append_nosize(&nspaces, jdata->nspace);
                    }
                }
                /* join the results into a single comma-delimited string */
                kv = PMIX_NEW(prte_info_item_t);
                tmp = pmix_argv_join(nspaces, ',');
                pmix_argv_free(nspaces);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                pmix_list_append(&results, &kv->super);

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACE_INFO)) {
                /* get the current jobids */
                PMIX_CONSTRUCT(&stack, pmix_list_t);
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    /* don't show the requestor's job */
                    if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)) {
                        kv = PMIX_NEW(prte_info_item_t);
                        (void) strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                        pmix_list_append(&stack, &kv->super);
                        /* create the array to hold the nspace and the cmd */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        kv->info.value.data.darray = darray;
                        info = (pmix_info_t *) darray->array;
                        /* add the nspace name */
                        PMIX_INFO_LOAD(&info[0], PMIX_NSPACE, jdata->nspace, PMIX_STRING);
                        /* add the cmd line */
                        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
                        if (NULL == app) {
                            ret = PMIX_ERR_NOT_FOUND;
                            goto done;
                        }
                        cmdline = pmix_argv_join(app->argv, ' ');
                        PMIX_INFO_LOAD(&info[1], PMIX_CMD_LINE, cmdline, PMIX_STRING);
                        free(cmdline);
                    }
                }
                kv = PMIX_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                kv->info.value.type = PMIX_DATA_ARRAY;
                m = pmix_list_get_size(&stack);
                PMIX_DATA_ARRAY_CREATE(darray, m, PMIX_INFO);
                kv->info.value.data.darray = darray;
                pmix_list_append(&results, &kv->super);
                /* join the results into an array */
                info = (pmix_info_t *) darray->array;
                p = 0;
                while (NULL != (kv = (prte_info_item_t *) pmix_list_remove_first(&stack))) {
                    PMIX_INFO_XFER(&info[p], &kv->info);
                    PMIX_RELEASE(kv);
                    ++p;
                }
                PMIX_LIST_DESTRUCT(&stack);

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_SPAWN_SUPPORT)) {
                ans = NULL;
                pmix_argv_append_nosize(&ans, PMIX_HOST);
                pmix_argv_append_nosize(&ans, PMIX_HOSTFILE);
                pmix_argv_append_nosize(&ans, PMIX_ADD_HOST);
                pmix_argv_append_nosize(&ans, PMIX_ADD_HOSTFILE);
                pmix_argv_append_nosize(&ans, PMIX_PREFIX);
                pmix_argv_append_nosize(&ans, PMIX_WDIR);
                pmix_argv_append_nosize(&ans, PMIX_MAPPER);
                pmix_argv_append_nosize(&ans, PMIX_PPR);
                pmix_argv_append_nosize(&ans, PMIX_MAPBY);
                pmix_argv_append_nosize(&ans, PMIX_RANKBY);
                pmix_argv_append_nosize(&ans, PMIX_BINDTO);
                pmix_argv_append_nosize(&ans, PMIX_COSPAWN_APP);
                /* create the return kv */
                kv = PMIX_NEW(prte_info_item_t);
                tmp = pmix_argv_join(ans, ',');
                pmix_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                pmix_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                pmix_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_INIT);
                pmix_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_APP);
#if PRTE_HAVE_STOP_ON_EXEC
                pmix_argv_append_nosize(&ans, PMIX_DEBUG_STOP_ON_EXEC);
#endif
                pmix_argv_append_nosize(&ans, PMIX_DEBUG_TARGET);
                /* create the return kv */
                kv = PMIX_NEW(prte_info_item_t);
                tmp = pmix_argv_join(ans, ',');
                pmix_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                pmix_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V1)) {
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    kv = PMIX_NEW(prte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
                    /* get this from the v1.x API */
                    if (0
                        != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len)) {
                        PMIX_RELEASE(kv);
                        continue;
                    }
#else
                    /* get it from the v2 API */
                    if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len,
                                                             HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
                        PMIX_RELEASE(kv);
                        continue;
                    }
#endif
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    pmix_list_append(&results, &kv->super);
                }
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V2)) {
                /* we cannot provide it if we are using v1.x */
#if HWLOC_API_VERSION >= 0x20000
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    kv = PMIX_NEW(prte_info_item_t);
                    if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len, 0)) {
                        PMIX_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    pmix_list_append(&results, &kv->super);
                }
#endif

            } else if (0 == strcmp(q->keys[n], PMIX_PROC_URI)) {
                /* they want our URI */
                kv = PMIX_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_PROC_URI, prte_process_info.my_hnp_uri, PMIX_STRING);
                pmix_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_SERVER_URI)) {
                /* they want the PMIx URI */
                if (NULL != hostname) {
                    /* find the node object */
                    node = NULL;
                    for (k = 0; k < prte_node_pool->size; k++) {
                        if (NULL
                            == (ndptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool,
                                                                                    k))) {
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
                PRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_SERVER_URI, &proct->name, (char **) &uri,
                                               PMIX_STRING);
                if (PRTE_SUCCESS != rc) {
                    ret = prte_pmix_convert_rc(rc);
                    goto done;
                }
                kv = PMIX_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_URI, uri, PMIX_STRING);
                free(uri);
                pmix_list_append(&results, &kv->super);

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
                /* setup the reply */
                kv = PMIX_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                pmix_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                procinfo = (pmix_proc_info_t *) darray->array;
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
                /* setup the reply */
                kv = PMIX_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_LOCAL_PROC_TABLE, PMIX_MAX_KEYLEN);
                pmix_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_local_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                procinfo = (pmix_proc_info_t *) darray->array;
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

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_PSETS)) {
                kv = PMIX_NEW(prte_info_item_t);
                sz = pmix_list_get_size(&prte_pmix_server_globals.psets);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NUM_PSETS, &sz, PMIX_SIZE);
                pmix_list_append(&results, &kv->super);

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PSET_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.psets, pmix_server_pset_t)
                {
                    pmix_argv_append_nosize(&ans, ps->name);
                }
                if (NULL == ans) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                } else {
                    tmp = pmix_argv_join(ans, ',');
                    pmix_argv_free(ans);
                    ans = NULL;
                    kv = PMIX_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_PSET_NAMES, tmp, PMIX_STRING);
                    pmix_list_append(&results, &kv->super);
                    free(tmp);
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
                /* define the array that holds the membership - no need to allocate anything */
                dry.array = psptr->members;
                dry.type = PMIX_PROC_RANK;
                dry.size = psptr->num_members;
                kv = PMIX_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_PSET_MEMBERSHIP, &dry, PMIX_DATA_ARRAY);
                pmix_list_append(&results, &kv->super);

            } else if (0 == strcmp(q->keys[n], PMIX_JOB_SIZE)) {
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = PMIX_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_JOB_SIZE, PMIX_MAX_KEYLEN);
                key = jdata->num_procs;
                PMIX_INFO_LOAD(&kv->info, PMIX_JOB_SIZE, &key, PMIX_UINT32);
                pmix_list_append(&results, &kv->super);

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_GROUPS)) {
                kv = PMIX_NEW(prte_info_item_t);
                sz = pmix_list_get_size(&prte_pmix_server_globals.groups);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NUM_GROUPS, &sz, PMIX_SIZE);
                pmix_list_append(&results, &kv->super);

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_GROUP_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                PMIX_LIST_FOREACH(ps, &prte_pmix_server_globals.groups, pmix_server_pset_t)
                {
                    pmix_argv_append_nosize(&ans, ps->name);
                }
                tmp = pmix_argv_join(ans, ',');
                pmix_argv_free(ans);
                ans = NULL;
                kv = PMIX_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_GROUP_NAMES, tmp, PMIX_STRING);
                pmix_list_append(&results, &kv->super);
                free(tmp);

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
                /* setup the reply */
                kv = PMIX_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_GROUP_MEMBERSHIP, PMIX_MAX_KEYLEN);
                pmix_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, grp->num_members, PMIX_PROC);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                proc = (pmix_proc_t *) darray->array;
                for (k = 0; k < grp->num_members; k++) {
                    PMIX_LOAD_PROCID(&proc[k], grp->members[k].nspace, grp->members[k].rank);
                }

            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_ALLOCATION)) {
                /* collect all the node info */
                void *nodelist, *nodeinfolist;
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
                    /* convert to array */
                    PMIX_INFO_LIST_CONVERT(rc, nodeinfolist, &dry);
                    PMIX_INFO_LIST_RELEASE(nodeinfolist);
                    /* now add the entry to the main list */
                    PMIX_INFO_LIST_ADD(rc, nodelist, PMIX_NODE_INFO, &dry, PMIX_DATA_ARRAY);
                    ++p;
                    PMIX_DATA_ARRAY_DESTRUCT(&dry);
                }
                /* convert list to array */
                PMIX_DATA_ARRAY_CREATE(darray, p, PMIX_INFO);
                PMIX_INFO_LIST_CONVERT(rc, nodelist, darray);
                PMIX_INFO_LIST_RELEASE(nodelist);
                /* add to results */
                kv = PMIX_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_ALLOCATION, PMIX_MAX_KEYLEN);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                pmix_list_append(&results, &kv->super);
            } else {
                fprintf(stderr, "Query for unrecognized attribute: %s\n", q->keys[n]);
            }
        } // for
    }     // for

done:
    rcd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    if (PMIX_SUCCESS == ret) {
        if (0 == pmix_list_get_size(&results)) {
            ret = PMIX_ERR_NOT_FOUND;
        } else {
            if (pmix_list_get_size(&results) < cd->ninfo) {
                ret = PMIX_QUERY_PARTIAL_SUCCESS;
            } else {
                ret = PMIX_SUCCESS;
            }
            /* convert the list of results to an info array */
            rcd->ninfo = pmix_list_get_size(&results);
            PMIX_INFO_CREATE(rcd->info, rcd->ninfo);
            n = 0;
            PMIX_LIST_FOREACH(kv, &results, prte_info_item_t)
            {
                PMIX_INFO_XFER(&rcd->info[n], &kv->info);
                n++;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&results);
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
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}
