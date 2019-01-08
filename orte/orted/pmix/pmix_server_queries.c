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
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "orte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/dss/dss.h"
#include "opal/hwloc/hwloc-internal.h"
#include "opal/mca/pstat/pstat.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/iof/iof.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/schizo/schizo.h"
#include "orte/mca/state/state.h"
#include "orte/util/name_fns.h"
#include "orte/util/show_help.h"
#include "orte/util/threads.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"

#include "pmix_server_internal.h"

static void qrel(void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    OBJ_RELEASE(cd);
}

static void _query(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    orte_pmix_server_op_caddy_t *rcd;
    pmix_query_t *q;
    pmix_status_t ret = PMIX_SUCCESS;
    opal_info_item_t *kv;
    orte_jobid_t jobid;
    orte_job_t *jdata;
    int rc;
    opal_list_t results, stack;
    size_t m, n, p;
    uint32_t key;
    void *nptr;
    char nspace[PMIX_MAX_NSLEN+1], *cmdline, **nspaces;
    char **ans, *tmp;
    orte_process_name_t requestor;
    orte_app_context_t *app;
#if OPAL_PMIX_VERSION >= 3
    opal_pstats_t pstat;
    float pss;
    bool local_only;
    orte_namelist_t *nm;
    opal_list_t targets;
    int i, k, num_replies;
    orte_proc_t *proct;
    pmix_proc_info_t *procinfo;
    pmix_info_t *info;
    pmix_data_array_t *darray;
#endif
#if OPAL_PMIX_VERSION >= 4
    size_t sz;
#endif

    ORTE_ACQUIRE_OBJECT(cd);

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s processing query",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    OBJ_CONSTRUCT(&results, opal_list_t);

    OPAL_PMIX_CONVERT_PROCT(rc, &requestor, cd->procs);
    if (OPAL_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
    }

    /* see what they wanted */
    for (m=0; m < cd->nqueries; m++) {
        q = &cd->queries[m];
        /* default to the requestor's jobid */
        jobid = requestor.jobid;
        /* see if they provided any qualifiers */
        if (NULL != q->qualifiers && 0 < q->nqual) {
            for (n=0; n < q->nqual; n++) {
                if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NSPACE)) {
                    OPAL_PMIX_CONVERT_NSPACE(rc, &jobid, q->qualifiers[n].value.data.string);
                    if (ORTE_JOBID_INVALID == jobid) {
                        rc = PMIX_ERR_BAD_PARAM;
                        goto done;
                    }
                }
            }
        }
        for (n=0; NULL != q->keys[n]; n++) {
            opal_output_verbose(2, orte_pmix_server_globals.output,
                                "%s processing key %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), q->keys[n]);
            if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACES)) {
                /* get the current jobids */
                nspaces = NULL;
                OBJ_CONSTRUCT(&stack, opal_list_t);
                rc = opal_hash_table_get_first_key_uint32(orte_job_data, &key, (void **)&jdata, &nptr);
                while (OPAL_SUCCESS == rc) {
                    /* don't show the requestor's job or non-launcher tools */
                    if (ORTE_PROC_MY_NAME->jobid != jdata->jobid &&
                        (!ORTE_FLAG_TEST(jdata, ORTE_JOB_FLAG_TOOL) || ORTE_FLAG_TEST(jdata, ORTE_JOB_FLAG_LAUNCHER))) {
                        memset(nspace, 0, PMIX_MAX_NSLEN);
                        OPAL_PMIX_CONVERT_JOBID(nspace, jdata->jobid);
                        opal_argv_append_nosize(&nspaces, nspace);
                    }
                    rc = opal_hash_table_get_next_key_uint32(orte_job_data, &key, (void **)&jdata, nptr, &nptr);
                }
                /* join the results into a single comma-delimited string */
                kv = OBJ_NEW(opal_info_item_t);
                tmp = opal_argv_join(nspaces, ',');
                opal_argv_free(nspaces);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                opal_list_append(&results, &kv->super);
#if OPAL_PMIX_VERSION >= 4
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACE_INFO)) {
                /* get the current jobids */
                OBJ_CONSTRUCT(&stack, opal_list_t);
                rc = opal_hash_table_get_first_key_uint32(orte_job_data, &key, (void **)&jdata, &nptr);
                while (OPAL_SUCCESS == rc) {
                    /* don't show the requestor's job or non-launcher tools */
                    if (!ORTE_FLAG_TEST(jdata, ORTE_JOB_FLAG_TOOL) || ORTE_FLAG_TEST(jdata, ORTE_JOB_FLAG_LAUNCHER)) {
                        kv = OBJ_NEW(opal_info_item_t);
                        (void)strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                        opal_list_append(&stack, &kv->super);
                        /* create the array to hold the nspace and the cmd */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        kv->info.value.data.darray = darray;
                        PMIX_INFO_CREATE(info, 2);
                        darray->array = info;
                        /* add the nspace name */
                        memset(nspace, 0, PMIX_MAX_NSLEN);
                        OPAL_PMIX_CONVERT_JOBID(nspace, jdata->jobid);
                        PMIX_INFO_LOAD(&info[0], PMIX_NSPACE, nspace, PMIX_STRING);
                        /* add the cmd line */
                        app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0);
                        cmdline = opal_argv_join(app->argv, ' ');
                        PMIX_INFO_LOAD(&info[1], PMIX_CMD_LINE, cmdline, PMIX_STRING);
                        free(cmdline);
                    }
                    rc = opal_hash_table_get_next_key_uint32(orte_job_data, &key, (void **)&jdata, nptr, &nptr);
                }
                kv = OBJ_NEW(opal_info_item_t);
                (void)strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                kv->info.value.type = PMIX_DATA_ARRAY;
                m = opal_list_get_size(&stack);
                PMIX_DATA_ARRAY_CREATE(darray, m, PMIX_INFO);
                kv->info.value.data.darray = darray;
                opal_list_append(&results, &kv->super);
                /* join the results into an array */
                if (0 < m) {
                    PMIX_INFO_CREATE(info, m);
                    darray->array = info;
                    p=0;
                    while (NULL != (kv = (opal_info_item_t*)opal_list_remove_first(&stack))) {
                        PMIX_INFO_XFER(&info[p], &kv->info);
                        OBJ_RELEASE(kv);
                        ++p;
                    }
                }
                OPAL_LIST_DESTRUCT(&stack);
#endif
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_SPAWN_SUPPORT)) {
                ans = NULL;
                opal_argv_append_nosize(&ans, PMIX_HOST);
                opal_argv_append_nosize(&ans, PMIX_HOSTFILE);
                opal_argv_append_nosize(&ans, PMIX_ADD_HOST);
                opal_argv_append_nosize(&ans, PMIX_ADD_HOSTFILE);
                opal_argv_append_nosize(&ans, PMIX_PREFIX);
                opal_argv_append_nosize(&ans, PMIX_WDIR);
                opal_argv_append_nosize(&ans, PMIX_MAPPER);
                opal_argv_append_nosize(&ans, PMIX_PPR);
                opal_argv_append_nosize(&ans, PMIX_MAPBY);
                opal_argv_append_nosize(&ans, PMIX_RANKBY);
                opal_argv_append_nosize(&ans, PMIX_BINDTO);
                opal_argv_append_nosize(&ans, PMIX_COSPAWN_APP);
                /* create the return kv */
                kv = OBJ_NEW(opal_info_item_t);
                tmp = opal_argv_join(ans, ',');
                opal_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                opal_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                opal_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_INIT);
                opal_argv_append_nosize(&ans, PMIX_DEBUG_JOB);
                opal_argv_append_nosize(&ans, PMIX_DEBUG_WAIT_FOR_NOTIFY);
                /* create the return kv */
                kv = OBJ_NEW(opal_info_item_t);
                tmp = opal_argv_join(ans, ',');
                opal_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                opal_list_append(&results, &kv->super);
#if PMIX_VERSION_MAJOR >= 3
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_MEMORY_USAGE)) {
                OBJ_CONSTRUCT(&targets, opal_list_t);
                /* scan the qualifiers */
                local_only = false;
                for (k=0; k < (int)q->nqual; k++) {
                    if (0 == strncmp(q->qualifiers[k].key, PMIX_QUERY_LOCAL_ONLY, PMIX_MAX_KEYLEN)) {
                        local_only = PMIX_INFO_TRUE(&q->qualifiers[k]);
                    } else if (0 == strncmp(q->qualifiers[k].key, PMIX_PROCID, PMIX_MAX_KEYLEN)) {
                        /* save this directive on our list of targets */
                        nm = OBJ_NEW(orte_namelist_t);
                        OPAL_PMIX_CONVERT_PROCT(rc, &nm->name, q->qualifiers[n].value.data.proc);
                        if (OPAL_SUCCESS != rc) {
                            ORTE_ERROR_LOG(rc);
                        }
                        opal_list_append(&targets, &nm->super);
                    }
                }
                /* if they have asked for only our local procs or daemon,
                 * then we can just get the data directly */
                if (local_only) {
                    if (0 == opal_list_get_size(&targets)) {
                        kv = OBJ_NEW(opal_info_item_t);
                        (void)strncpy(kv->info.key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                        opal_list_append(&results, &kv->super);
                        /* create an entry for myself plus the avg of all local procs */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        kv->info.value.data.darray = darray;
                        PMIX_INFO_CREATE(info, 2);
                        darray->array = info;
                        /* collect my memory usage */
                        OBJ_CONSTRUCT(&pstat, opal_pstats_t);
                        opal_pstat.query(orte_process_info.pid, &pstat, NULL);
                        PMIX_INFO_LOAD(&info[0], PMIX_DAEMON_MEMORY, &pstat.pss, PMIX_FLOAT);
                        OBJ_DESTRUCT(&pstat);
                        /* collect the memory usage of all my children */
                        pss = 0.0;
                        num_replies = 0;
                        for (i=0; i < orte_local_children->size; i++) {
                            if (NULL != (proct = (orte_proc_t*)opal_pointer_array_get_item(orte_local_children, i)) &&
                                ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_ALIVE)) {
                                /* collect the stats on this proc */
                                OBJ_CONSTRUCT(&pstat, opal_pstats_t);
                                if (OPAL_SUCCESS == opal_pstat.query(proct->pid, &pstat, NULL)) {
                                    pss += pstat.pss;
                                    ++num_replies;
                                }
                                OBJ_DESTRUCT(&pstat);
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
                if (ORTE_SUCCESS == orte_schizo.get_remaining_time(&key)) {
                    kv = OBJ_NEW(opal_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_TIME_REMAINING, &key, PMIX_UINT32);
                    opal_list_append(&results, &kv->super);
                }
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V1)) {
                if (NULL != opal_hwloc_topology) {
                    char *xmlbuffer=NULL;
                    int len;
                    kv = OBJ_NEW(opal_info_item_t);
            #if HWLOC_API_VERSION < 0x20000
                    /* get this from the v1.x API */
                    if (0 != hwloc_topology_export_xmlbuffer(opal_hwloc_topology, &xmlbuffer, &len)) {
                        OBJ_RELEASE(kv);
                        continue;
                    }
            #else
                    /* get it from the v2 API */
                    if (0 != hwloc_topology_export_xmlbuffer(opal_hwloc_topology, &xmlbuffer, &len,
                                                             HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
                        OBJ_RELEASE(kv);
                        continue;
                    }
            #endif
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    opal_list_append(&results, &kv->super);
                }
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V2)) {
                /* we cannot provide it if we are using v1.x */
            #if HWLOC_API_VERSION >= 0x20000
                if (NULL != opal_hwloc_topology) {
                    char *xmlbuffer=NULL;
                    int len;
                    kv = OBJ_NEW(opal_info_item_t);
                    if (0 != hwloc_topology_export_xmlbuffer(opal_hwloc_topology, &xmlbuffer, &len, 0)) {
                        OBJ_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    opal_list_append(&results, &kv->super);
                }
            #endif
            } else if (0 == strcmp(q->keys[n], PMIX_PROC_URI)) {
                /* they want our URI */
                kv = OBJ_NEW(opal_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_PROC_URI, orte_process_info.my_hnp_uri, PMIX_STRING);
                opal_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_SERVER_URI)) {
                /* they want our PMIx URI */
                kv = OBJ_NEW(opal_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_URI, orte_process_info.my_hnp_uri, PMIX_STRING);
                opal_list_append(&results, &kv->super);
    #if PMIX_VERSION_MAJOR >= 3
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PROC_TABLE)) {
                /* construct a list of values with opal_proc_info_t
                 * entries for each proc in the indicated job */
                jdata = orte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = OBJ_NEW(opal_info_item_t);
                (void)strncpy(kv->info.key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                opal_list_append(&results, &kv->super);
                 /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                PMIX_PROC_INFO_CREATE(procinfo, jdata->num_local_procs);
                darray->array = procinfo;
                p = 0;
                for (k=0; k < jdata->procs->size; k++) {
                    if (NULL == (proct = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, k))) {
                        continue;
                    }
                    OPAL_PMIX_CONVERT_NAME(&procinfo[p].proc, &proct->name);
                    if (NULL != proct->node && NULL != proct->node->name) {
                        procinfo[p].hostname = strdup(proct->node->name);
                    }
                    app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, proct->app_idx);
                    if (NULL != app && NULL != app->app) {
                        procinfo[p].executable_name = strdup(app->app);
                    }
                    procinfo[p].pid = proct->pid;
                    procinfo[p].exit_code = proct->exit_code;
                    procinfo[p].state = opal_pmix_convert_state(proct->state);
                    ++p;
                }
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_LOCAL_PROC_TABLE)) {
                /* construct a list of values with opal_proc_info_t
                 * entries for each LOCAL proc in the indicated job */
                jdata = orte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    rc = ORTE_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = OBJ_NEW(opal_info_item_t);
                (void)strncpy(kv->info.key, PMIX_QUERY_LOCAL_PROC_TABLE, PMIX_MAX_KEYLEN);
                opal_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_local_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                PMIX_PROC_INFO_CREATE(procinfo, jdata->num_local_procs);
                darray->array = procinfo;
                p = 0;
                for (k=0; k < jdata->procs->size; k++) {
                    if (NULL == (proct = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, k))) {
                        continue;
                    }
                    if (ORTE_FLAG_TEST(proct, ORTE_PROC_FLAG_LOCAL)) {
                        OPAL_PMIX_CONVERT_NAME(&procinfo[p].proc, &proct->name);
                        if (NULL != proct->node && NULL != proct->node->name) {
                            procinfo[p].hostname = strdup(proct->node->name);
                        }
                        app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, proct->app_idx);
                        if (NULL != app && NULL != app->app) {
                            procinfo[p].executable_name = strdup(app->app);
                        }
                        procinfo[p].pid = proct->pid;
                        procinfo[p].exit_code = proct->exit_code;
                        procinfo[p].state = opal_pmix_convert_state(proct->state);
                        ++p;
                    }
                }
    #endif
    #if PMIX_VERSION_MAJOR >= 4
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_PSETS)) {
                kv = OBJ_NEW(opal_info_item_t);
                sz = opal_list_get_size(&orte_pmix_server_globals.psets);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NUM_PSETS, &sz, PMIX_SIZE);
                opal_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PSET_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                OPAL_LIST_FOREACH(ps, &orte_pmix_server_globals.psets, pmix_server_pset_t) {
                    opal_argv_append_nosize(&ans, ps->name);
                }
                tmp = opal_argv_join(ans, ',');
                opal_argv_free(ans);
                ans = NULL;
                kv = OBJ_NEW(opal_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_PSET_NAMES, tmp, PMIX_STRING);
                opal_list_append(&results, &kv->super);
                free(tmp);
    #endif
            } else if (0 == strcmp(q->keys[n], PMIX_JOB_SIZE)) {
                jdata = orte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    rc = ORTE_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = OBJ_NEW(opal_info_item_t);
                (void)strncpy(kv->info.key, PMIX_JOB_SIZE, PMIX_MAX_KEYLEN);
                key = jdata->num_procs;
                PMIX_INFO_LOAD(&kv->info, PMIX_JOB_SIZE, &key, PMIX_UINT32);
                opal_list_append(&results, &kv->super);
            } else {
                fprintf(stderr, "Query for unrecognized attribute: %s\n", q->keys[n]);
            }
        } // for
    } // for

#if OPAL_PMIX_VERSION >= 3
  done:
#endif
    rcd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    if (PMIX_SUCCESS == ret) {
        if (0 == opal_list_get_size(&results)) {
            ret = PMIX_ERR_NOT_FOUND;
        } else {
            if (opal_list_get_size(&results) < cd->ninfo) {
                ret = PMIX_QUERY_PARTIAL_SUCCESS;
            } else {
                ret = PMIX_SUCCESS;
            }
            /* convert the list of results to an info array */
            rcd->ninfo = opal_list_get_size(&results);
            PMIX_INFO_CREATE(rcd->info, rcd->ninfo);
            n=0;
            OPAL_LIST_FOREACH(kv, &results, opal_info_item_t) {
                PMIX_INFO_XFER(&rcd->info[n], &kv->info);
                n++;
            }
        }
    }
    OPAL_LIST_DESTRUCT(&results);
    cd->infocbfunc(ret, rcd->info, rcd->ninfo, cd->cbdata, qrel, rcd);
    OBJ_RELEASE(cd);
}

pmix_status_t pmix_server_query_fn(pmix_proc_t *proct,
                                   pmix_query_t *queries, size_t nqueries,
                                   pmix_info_cbfunc_t cbfunc,
                                   void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd;

    if (NULL == queries || NULL == cbfunc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    cd->procs = proct;
    cd->queries = queries;
    cd->nqueries = nqueries;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    opal_event_set(orte_event_base, &(cd->ev), -1,
                   OPAL_EV_WRITE, _query, cd);
    opal_event_set_priority(&(cd->ev), ORTE_MSG_PRI);
    ORTE_POST_OBJECT(cd);
    opal_event_active(&(cd->ev), OPAL_EV_WRITE, 1);

    return PMIX_SUCCESS;
}
