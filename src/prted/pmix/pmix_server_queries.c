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
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/dss/dss.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/pstat/pstat.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/iof.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/rml/rml.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"

#include "pmix_server_internal.h"

static void qrel(void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRRTE_RELEASE(cd);
}

static void _query(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    prrte_pmix_server_op_caddy_t *rcd;
    pmix_query_t *q;
    pmix_status_t ret = PMIX_SUCCESS;
    prrte_info_item_t *kv;
    prrte_jobid_t jobid;
    prrte_job_t *jdata;
    prrte_node_t *node, *ndptr;
    int k, rc;
    prrte_list_t results, stack;
    size_t m, n, p;
    uint32_t key, nodeid;
    void *nptr;
    char nspace[PMIX_MAX_NSLEN+1], **nspaces, *hostname, *uri;
#ifdef PMIX_QUERY_NAMESPACE_INFO
    char *cmdline;
#endif
    char **ans, *tmp;
    prrte_process_name_t requestor;
    prrte_app_context_t *app;
    prrte_pstats_t pstat;
    float pss;
    bool local_only;
    prrte_namelist_t *nm;
    prrte_list_t targets;
    int i, num_replies;
    pmix_proc_info_t *procinfo;
    pmix_info_t *info;
    pmix_data_array_t *darray;
    prrte_proc_t *proct;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    size_t sz;
#endif

    PRRTE_ACQUIRE_OBJECT(cd);

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s processing query",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    PRRTE_CONSTRUCT(&results, prrte_list_t);

    PRRTE_PMIX_CONVERT_PROCT(rc, &requestor, &cd->proct);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        ret = PMIX_ERR_BAD_PARAM;
        goto done;
    }

    /* see what they wanted */
    for (m=0; m < cd->nqueries; m++) {
        q = &cd->queries[m];
        hostname = NULL;
        nodeid = UINT32_MAX;
        /* default to the requestor's jobid */
        jobid = requestor.jobid;
        /* see if they provided any qualifiers */
        if (NULL != q->qualifiers && 0 < q->nqual) {
            for (n=0; n < q->nqual; n++) {
                if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NSPACE)) {
                    PRRTE_PMIX_CONVERT_NSPACE(rc, &jobid, q->qualifiers[n].value.data.string);
                    if (PRRTE_JOBID_INVALID == jobid || PRRTE_SUCCESS != rc) {
                        ret = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_HOSTNAME)) {
                    hostname = q->qualifiers[n].value.data.string;
                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NODEID)) {
                    PMIX_VALUE_GET_NUMBER(rc, &q->qualifiers[n].value, nodeid, uint32_t);
                }
            }
        }
        for (n=0; NULL != q->keys[n]; n++) {
            prrte_output_verbose(2, prrte_pmix_server_globals.output,
                                "%s processing key %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), q->keys[n]);
            if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACES)) {
                /* get the current jobids */
                nspaces = NULL;
                PRRTE_CONSTRUCT(&stack, prrte_list_t);
                rc = prrte_hash_table_get_first_key_uint32(prrte_job_data, &key, (void **)&jdata, &nptr);
                while (PRRTE_SUCCESS == rc) {
                    /* don't show the requestor's job or non-launcher tools */
                    if (PRRTE_PROC_MY_NAME->jobid != jdata->jobid &&
                        (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_TOOL) || PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_LAUNCHER))) {
                        memset(nspace, 0, PMIX_MAX_NSLEN);
                        PRRTE_PMIX_CONVERT_JOBID(nspace, jdata->jobid);
                        prrte_argv_append_nosize(&nspaces, nspace);
                    }
                    rc = prrte_hash_table_get_next_key_uint32(prrte_job_data, &key, (void **)&jdata, nptr, &nptr);
                }
                /* join the results into a single comma-delimited string */
                kv = PRRTE_NEW(prrte_info_item_t);
                tmp = prrte_argv_join(nspaces, ',');
                prrte_argv_free(nspaces);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                prrte_list_append(&results, &kv->super);
#ifdef PMIX_QUERY_NAMESPACE_INFO
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACE_INFO)) {
                /* get the current jobids */
                PRRTE_CONSTRUCT(&stack, prrte_list_t);
                rc = prrte_hash_table_get_first_key_uint32(prrte_job_data, &key, (void **)&jdata, &nptr);
                while (PRRTE_SUCCESS == rc) {
                    /* don't show the requestor's job or non-launcher tools */
                    if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_TOOL) || PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_LAUNCHER)) {
                        kv = PRRTE_NEW(prrte_info_item_t);
                        (void)strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                        prrte_list_append(&stack, &kv->super);
                        /* create the array to hold the nspace and the cmd */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        kv->info.value.data.darray = darray;
                        info = (pmix_info_t*)darray->array;
                        /* add the nspace name */
                        memset(nspace, 0, PMIX_MAX_NSLEN);
                        PRRTE_PMIX_CONVERT_JOBID(nspace, jdata->jobid);
                        PMIX_INFO_LOAD(&info[0], PMIX_NSPACE, nspace, PMIX_STRING);
                        /* add the cmd line */
                        app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0);
                        cmdline = prrte_argv_join(app->argv, ' ');
                        PMIX_INFO_LOAD(&info[1], PMIX_CMD_LINE, cmdline, PMIX_STRING);
                        free(cmdline);
                    }
                    rc = prrte_hash_table_get_next_key_uint32(prrte_job_data, &key, (void **)&jdata, nptr, &nptr);
                }
                kv = PRRTE_NEW(prrte_info_item_t);
                (void)strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                kv->info.value.type = PMIX_DATA_ARRAY;
                m = prrte_list_get_size(&stack);
                PMIX_DATA_ARRAY_CREATE(darray, m, PMIX_INFO);
                kv->info.value.data.darray = darray;
                prrte_list_append(&results, &kv->super);
                /* join the results into an array */
                info = (pmix_info_t*)darray->array;
                p=0;
                while (NULL != (kv = (prrte_info_item_t*)prrte_list_remove_first(&stack))) {
                    PMIX_INFO_XFER(&info[p], &kv->info);
                    PRRTE_RELEASE(kv);
                    ++p;
                }
                PRRTE_LIST_DESTRUCT(&stack);
#endif
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_SPAWN_SUPPORT)) {
                ans = NULL;
                prrte_argv_append_nosize(&ans, PMIX_HOST);
                prrte_argv_append_nosize(&ans, PMIX_HOSTFILE);
                prrte_argv_append_nosize(&ans, PMIX_ADD_HOST);
                prrte_argv_append_nosize(&ans, PMIX_ADD_HOSTFILE);
                prrte_argv_append_nosize(&ans, PMIX_PREFIX);
                prrte_argv_append_nosize(&ans, PMIX_WDIR);
                prrte_argv_append_nosize(&ans, PMIX_MAPPER);
                prrte_argv_append_nosize(&ans, PMIX_PPR);
                prrte_argv_append_nosize(&ans, PMIX_MAPBY);
                prrte_argv_append_nosize(&ans, PMIX_RANKBY);
                prrte_argv_append_nosize(&ans, PMIX_BINDTO);
                prrte_argv_append_nosize(&ans, PMIX_COSPAWN_APP);
                /* create the return kv */
                kv = PRRTE_NEW(prrte_info_item_t);
                tmp = prrte_argv_join(ans, ',');
                prrte_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                prrte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                prrte_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_INIT);
                prrte_argv_append_nosize(&ans, PMIX_DEBUG_JOB);
                prrte_argv_append_nosize(&ans, PMIX_DEBUG_WAIT_FOR_NOTIFY);
                /* create the return kv */
                kv = PRRTE_NEW(prrte_info_item_t);
                tmp = prrte_argv_join(ans, ',');
                prrte_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                prrte_list_append(&results, &kv->super);
#ifdef PMIX_QUERY_MEMORY_USAGE
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_MEMORY_USAGE)) {
                PRRTE_CONSTRUCT(&targets, prrte_list_t);
                /* scan the qualifiers */
                local_only = false;
                for (k=0; k < (int)q->nqual; k++) {
                    if (0 == strncmp(q->qualifiers[k].key, PMIX_QUERY_LOCAL_ONLY, PMIX_MAX_KEYLEN)) {
                        local_only = PMIX_INFO_TRUE(&q->qualifiers[k]);
                    } else if (0 == strncmp(q->qualifiers[k].key, PMIX_PROCID, PMIX_MAX_KEYLEN)) {
                        /* save this directive on our list of targets */
                        nm = PRRTE_NEW(prrte_namelist_t);
                        PRRTE_PMIX_CONVERT_PROCT(rc, &nm->name, q->qualifiers[n].value.data.proc);
                        if (PRRTE_SUCCESS != rc) {
                            PRRTE_ERROR_LOG(rc);
                        }
                        prrte_list_append(&targets, &nm->super);
                    }
                }
                /* if they have asked for only our local procs or daemon,
                 * then we can just get the data directly */
                if (local_only) {
                    if (0 == prrte_list_get_size(&targets)) {
                        kv = PRRTE_NEW(prrte_info_item_t);
                        (void)strncpy(kv->info.key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                        prrte_list_append(&results, &kv->super);
                        /* create an entry for myself plus the avg of all local procs */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        kv->info.value.data.darray = darray;
#if PMIX_NUMERIC_VERSION < 0x00030100
                        PMIX_INFO_CREATE(darray->array, 2);
#endif
                        info = (pmix_info_t*)darray->array;
                        /* collect my memory usage */
                        PRRTE_CONSTRUCT(&pstat, prrte_pstats_t);
                        prrte_pstat.query(prrte_process_info.pid, &pstat, NULL);
                        PMIX_INFO_LOAD(&info[0], PMIX_DAEMON_MEMORY, &pstat.pss, PMIX_FLOAT);
                        PRRTE_DESTRUCT(&pstat);
                        /* collect the memory usage of all my children */
                        pss = 0.0;
                        num_replies = 0;
                        for (i=0; i < prrte_local_children->size; i++) {
                            if (NULL != (proct = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                                PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_ALIVE)) {
                                /* collect the stats on this proc */
                                PRRTE_CONSTRUCT(&pstat, prrte_pstats_t);
                                if (PRRTE_SUCCESS == prrte_pstat.query(proct->pid, &pstat, NULL)) {
                                    pss += pstat.pss;
                                    ++num_replies;
                                }
                                PRRTE_DESTRUCT(&pstat);
                            }
                        }
                        /* compute the average value */
                        if (0 < num_replies) {
                            pss /= (float)num_replies;
                        }
                        PMIX_INFO_LOAD(&info[1], PMIX_CLIENT_AVG_MEMORY, &pss, PMIX_FLOAT);
                    }
                }
#endif
            } else if (0 == strcmp(q->keys[n], PMIX_TIME_REMAINING)) {
                if (PRRTE_SUCCESS == prrte_schizo.get_remaining_time(&key)) {
                    kv = PRRTE_NEW(prrte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_TIME_REMAINING, &key, PMIX_UINT32);
                    prrte_list_append(&results, &kv->super);
                }
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V1)) {
                if (NULL != prrte_hwloc_topology) {
                    char *xmlbuffer=NULL;
                    int len;
                    kv = PRRTE_NEW(prrte_info_item_t);
            #if HWLOC_API_VERSION < 0x20000
                    /* get this from the v1.x API */
                    if (0 != hwloc_topology_export_xmlbuffer(prrte_hwloc_topology, &xmlbuffer, &len)) {
                        PRRTE_RELEASE(kv);
                        continue;
                    }
            #else
                    /* get it from the v2 API */
                    if (0 != hwloc_topology_export_xmlbuffer(prrte_hwloc_topology, &xmlbuffer, &len,
                                                             HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
                        PRRTE_RELEASE(kv);
                        continue;
                    }
            #endif
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    prrte_list_append(&results, &kv->super);
                }
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V2)) {
                /* we cannot provide it if we are using v1.x */
            #if HWLOC_API_VERSION >= 0x20000
                if (NULL != prrte_hwloc_topology) {
                    char *xmlbuffer=NULL;
                    int len;
                    kv = PRRTE_NEW(prrte_info_item_t);
                    if (0 != hwloc_topology_export_xmlbuffer(prrte_hwloc_topology, &xmlbuffer, &len, 0)) {
                        PRRTE_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    prrte_list_append(&results, &kv->super);
                }
            #endif
            } else if (0 == strcmp(q->keys[n], PMIX_PROC_URI)) {
                /* they want our URI */
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_PROC_URI, prrte_process_info.my_hnp_uri, PMIX_STRING);
                prrte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_SERVER_URI)) {
                /* they want the PMIx URI */
                if (NULL != hostname) {
                    /* find the node object */
                    node = NULL;
                    for (k=0; k < prrte_node_pool->size; k++) {
                        if (NULL == (ndptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, k))) {
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
                    node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, nodeid);
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
                    proct = prrte_get_proc_object(PRRTE_PROC_MY_NAME);
                }
                /* get the server uri value - we can block here as we are in
                 * an PRRTE progress thread */
                PRRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_SERVER_URI, &proct->name,
                                              (char**)&uri, PRRTE_STRING);
                if (PRRTE_SUCCESS != rc) {
                    ret = prrte_pmix_convert_rc(rc);
                    goto done;
                }
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_URI, uri, PMIX_STRING);
                free(uri);
                prrte_list_append(&results, &kv->super);
    #ifdef PMIX_QUERY_PROC_TABLE
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PROC_TABLE)) {
                /* construct a list of values with prrte_proc_info_t
                 * entries for each proc in the indicated job */
                jdata = prrte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = PRRTE_NEW(prrte_info_item_t);
                (void)strncpy(kv->info.key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                prrte_list_append(&results, &kv->super);
                 /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
        #if PMIX_NUMERIC_VERSION < 0x00030100
                PMIX_PROC_INFO_CREATE(darray->array, jdata->num_local_procs);
        #endif
                procinfo = (pmix_proc_info_t*)darray->array;
                p = 0;
                for (k=0; k < jdata->procs->size; k++) {
                    if (NULL == (proct = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, k))) {
                        continue;
                    }
                    PRRTE_PMIX_CONVERT_NAME(&procinfo[p].proc, &proct->name);
                    if (NULL != proct->node && NULL != proct->node->name) {
                        procinfo[p].hostname = strdup(proct->node->name);
                    }
                    app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, proct->app_idx);
                    if (NULL != app && NULL != app->app) {
                        procinfo[p].executable_name = strdup(app->app);
                    }
                    procinfo[p].pid = proct->pid;
                    procinfo[p].exit_code = proct->exit_code;
                    procinfo[p].state = prrte_pmix_convert_state(proct->state);
                    ++p;
                }
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_LOCAL_PROC_TABLE)) {
                /* construct a list of values with prrte_proc_info_t
                 * entries for each LOCAL proc in the indicated job */
                jdata = prrte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    rc = PRRTE_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = PRRTE_NEW(prrte_info_item_t);
                (void)strncpy(kv->info.key, PMIX_QUERY_LOCAL_PROC_TABLE, PMIX_MAX_KEYLEN);
                prrte_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_local_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
        #if PMIX_NUMERIC_VERSION < 0x00030100
                PMIX_PROC_INFO_CREATE(darray->array, jdata->num_local_procs);
        #endif
                procinfo = (pmix_proc_info_t*)darray->array;
                p = 0;
                for (k=0; k < jdata->procs->size; k++) {
                    if (NULL == (proct = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, k))) {
                        continue;
                    }
                    if (PRRTE_FLAG_TEST(proct, PRRTE_PROC_FLAG_LOCAL)) {
                        PRRTE_PMIX_CONVERT_NAME(&procinfo[p].proc, &proct->name);
                        if (NULL != proct->node && NULL != proct->node->name) {
                            procinfo[p].hostname = strdup(proct->node->name);
                        }
                        app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, proct->app_idx);
                        if (NULL != app && NULL != app->app) {
                            procinfo[p].executable_name = strdup(app->app);
                        }
                        procinfo[p].pid = proct->pid;
                        procinfo[p].exit_code = proct->exit_code;
                        procinfo[p].state = prrte_pmix_convert_state(proct->state);
                        ++p;
                    }
                }
    #endif
    #ifdef PMIX_QUERY_NUM_PSETS
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_PSETS)) {
                kv = PRRTE_NEW(prrte_info_item_t);
                sz = prrte_list_get_size(&prrte_pmix_server_globals.psets);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NUM_PSETS, &sz, PMIX_SIZE);
                prrte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PSET_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                PRRTE_LIST_FOREACH(ps, &prrte_pmix_server_globals.psets, pmix_server_pset_t) {
                    prrte_argv_append_nosize(&ans, ps->name);
                }
                tmp = prrte_argv_join(ans, ',');
                prrte_argv_free(ans);
                ans = NULL;
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_PSET_NAMES, tmp, PMIX_STRING);
                prrte_list_append(&results, &kv->super);
                free(tmp);
    #endif
            } else if (0 == strcmp(q->keys[n], PMIX_JOB_SIZE)) {
                jdata = prrte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    rc = PRRTE_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = PRRTE_NEW(prrte_info_item_t);
                (void)strncpy(kv->info.key, PMIX_JOB_SIZE, PMIX_MAX_KEYLEN);
                key = jdata->num_procs;
                PMIX_INFO_LOAD(&kv->info, PMIX_JOB_SIZE, &key, PMIX_UINT32);
                prrte_list_append(&results, &kv->super);
            } else {
                fprintf(stderr, "Query for unrecognized attribute: %s\n", q->keys[n]);
            }
        } // for
    } // for

  done:
    rcd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    if (PMIX_SUCCESS == ret) {
        if (0 == prrte_list_get_size(&results)) {
            ret = PMIX_ERR_NOT_FOUND;
        } else {
            if (prrte_list_get_size(&results) < cd->ninfo) {
                ret = PMIX_QUERY_PARTIAL_SUCCESS;
            } else {
                ret = PMIX_SUCCESS;
            }
            /* convert the list of results to an info array */
            rcd->ninfo = prrte_list_get_size(&results);
            PMIX_INFO_CREATE(rcd->info, rcd->ninfo);
            n=0;
            PRRTE_LIST_FOREACH(kv, &results, prrte_info_item_t) {
                PMIX_INFO_XFER(&rcd->info[n], &kv->info);
                n++;
            }
        }
    }
    PRRTE_LIST_DESTRUCT(&results);
    cd->infocbfunc(ret, rcd->info, rcd->ninfo, cd->cbdata, qrel, rcd);
    PRRTE_RELEASE(cd);
}

pmix_status_t pmix_server_query_fn(pmix_proc_t *proct,
                                   pmix_query_t *queries, size_t nqueries,
                                   pmix_info_cbfunc_t cbfunc,
                                   void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd;

    if (NULL == queries || NULL == cbfunc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    memcpy(&cd->proct, proct, sizeof(pmix_proc_t));
    cd->queries = queries;
    cd->nqueries = nqueries;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    prrte_event_set(prrte_event_base, &(cd->ev), -1,
                   PRRTE_EV_WRITE, _query, cd);
    prrte_event_set_priority(&(cd->ev), PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(cd);
    prrte_event_active(&(cd->ev), PRRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}
