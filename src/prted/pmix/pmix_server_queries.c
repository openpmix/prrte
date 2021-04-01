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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include "src/util/argv.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void qrel(void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRTE_RELEASE(cd);
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
    prte_list_t results, stack;
    size_t m, n, p;
    uint32_t key, nodeid;
    char **nspaces, *hostname, *uri;
    char *cmdline;
    char **ans, *tmp;
    prte_app_context_t *app;
    int matched;
    pmix_proc_info_t *procinfo;
    pmix_info_t *info;
    pmix_data_array_t *darray;
    prte_proc_t *proct;
    size_t sz;

    PRTE_ACQUIRE_OBJECT(cd);

    prte_output_verbose(2, prte_pmix_server_globals.output, "%s processing query",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PRTE_CONSTRUCT(&results, prte_list_t);

    /* see what they wanted */
    for (m = 0; m < cd->nqueries; m++) {
        q = &cd->queries[m];
        hostname = NULL;
        nodeid = UINT32_MAX;
        /* default to the requestor's jobid */
        PMIX_LOAD_NSPACE(jobid, cd->proct.nspace);
        /* see if they provided any qualifiers */
        if (NULL != q->qualifiers && 0 < q->nqual) {
            for (n = 0; n < q->nqual; n++) {
                prte_output_verbose(2, prte_pmix_server_globals.output,
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
                        jdata = (prte_job_t *) prte_pointer_array_get_item(prte_job_data, k);
                        if (NULL != jdata
                            && PMIX_CHECK_NSPACE(q->qualifiers[n].value.data.string,
                                                 jdata->nspace)) {
                            matched = 1;
                            break;
                        }
                    }
                    if (0 == matched) {
                        prte_output_verbose(
                            2, prte_pmix_server_globals.output,
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
                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_HOSTNAME)) {
                    hostname = q->qualifiers[n].value.data.string;
                } else if (PMIX_CHECK_KEY(&q->qualifiers[n], PMIX_NODEID)) {
                    PMIX_VALUE_GET_NUMBER(rc, &q->qualifiers[n].value, nodeid, uint32_t);
                }
            }
        }
        for (n = 0; NULL != q->keys[n]; n++) {
            prte_output_verbose(2, prte_pmix_server_globals.output, "%s processing key %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), q->keys[n]);
            if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACES)) {
                /* get the current jobids */
                nspaces = NULL;
                PRTE_CONSTRUCT(&stack, prte_list_t);
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) prte_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    /* don't show the requestor's job or non-launcher tools */
                    if (!PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)
                        && (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)
                            || PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_LAUNCHER))) {
                        prte_argv_append_nosize(&nspaces, jdata->nspace);
                    }
                }
                /* join the results into a single comma-delimited string */
                kv = PRTE_NEW(prte_info_item_t);
                tmp = prte_argv_join(nspaces, ',');
                prte_argv_free(nspaces);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                prte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACE_INFO)) {
                /* get the current jobids */
                PRTE_CONSTRUCT(&stack, prte_list_t);
                for (k = 0; k < prte_job_data->size; k++) {
                    jdata = (prte_job_t *) prte_pointer_array_get_item(prte_job_data, k);
                    if (NULL == jdata) {
                        continue;
                    }
                    /* don't show the requestor's job or non-launcher tools */
                    if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)
                        || PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_LAUNCHER)) {
                        kv = PRTE_NEW(prte_info_item_t);
                        (void) strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                        prte_list_append(&stack, &kv->super);
                        /* create the array to hold the nspace and the cmd */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        kv->info.value.data.darray = darray;
                        info = (pmix_info_t *) darray->array;
                        /* add the nspace name */
                        PMIX_INFO_LOAD(&info[0], PMIX_NSPACE, jdata->nspace, PMIX_STRING);
                        /* add the cmd line */
                        app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, 0);
                        if (NULL == app) {
                            ret = PMIX_ERR_NOT_FOUND;
                            goto done;
                        }
                        cmdline = prte_argv_join(app->argv, ' ');
                        PMIX_INFO_LOAD(&info[1], PMIX_CMD_LINE, cmdline, PMIX_STRING);
                        free(cmdline);
                    }
                }
                kv = PRTE_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_NAMESPACE_INFO, PMIX_MAX_KEYLEN);
                kv->info.value.type = PMIX_DATA_ARRAY;
                m = prte_list_get_size(&stack);
                PMIX_DATA_ARRAY_CREATE(darray, m, PMIX_INFO);
                kv->info.value.data.darray = darray;
                prte_list_append(&results, &kv->super);
                /* join the results into an array */
                info = (pmix_info_t *) darray->array;
                p = 0;
                while (NULL != (kv = (prte_info_item_t *) prte_list_remove_first(&stack))) {
                    PMIX_INFO_XFER(&info[p], &kv->info);
                    PRTE_RELEASE(kv);
                    ++p;
                }
                PRTE_LIST_DESTRUCT(&stack);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_SPAWN_SUPPORT)) {
                ans = NULL;
                prte_argv_append_nosize(&ans, PMIX_HOST);
                prte_argv_append_nosize(&ans, PMIX_HOSTFILE);
                prte_argv_append_nosize(&ans, PMIX_ADD_HOST);
                prte_argv_append_nosize(&ans, PMIX_ADD_HOSTFILE);
                prte_argv_append_nosize(&ans, PMIX_PREFIX);
                prte_argv_append_nosize(&ans, PMIX_WDIR);
                prte_argv_append_nosize(&ans, PMIX_MAPPER);
                prte_argv_append_nosize(&ans, PMIX_PPR);
                prte_argv_append_nosize(&ans, PMIX_MAPBY);
                prte_argv_append_nosize(&ans, PMIX_RANKBY);
                prte_argv_append_nosize(&ans, PMIX_BINDTO);
                prte_argv_append_nosize(&ans, PMIX_COSPAWN_APP);
                /* create the return kv */
                kv = PRTE_NEW(prte_info_item_t);
                tmp = prte_argv_join(ans, ',');
                prte_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                prte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                prte_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_INIT);
                prte_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_APP);
#if PRTE_HAVE_STOP_ON_EXEC
                prte_argv_append_nosize(&ans, PMIX_DEBUG_STOP_ON_EXEC);
#endif
                prte_argv_append_nosize(&ans, PMIX_DEBUG_TARGET);
                /* create the return kv */
                kv = PRTE_NEW(prte_info_item_t);
                tmp = prte_argv_join(ans, ',');
                prte_argv_free(ans);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                prte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V1)) {
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    kv = PRTE_NEW(prte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
                    /* get this from the v1.x API */
                    if (0
                        != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len)) {
                        PRTE_RELEASE(kv);
                        continue;
                    }
#else
                    /* get it from the v2 API */
                    if (0
                        != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len,
                                                           HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
                        PRTE_RELEASE(kv);
                        continue;
                    }
#endif
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    prte_list_append(&results, &kv->super);
                }
            } else if (0 == strcmp(q->keys[n], PMIX_HWLOC_XML_V2)) {
                /* we cannot provide it if we are using v1.x */
#if HWLOC_API_VERSION >= 0x20000
                if (NULL != prte_hwloc_topology) {
                    char *xmlbuffer = NULL;
                    int len;
                    kv = PRTE_NEW(prte_info_item_t);
                    if (0
                        != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len,
                                                           0)) {
                        PRTE_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    prte_list_append(&results, &kv->super);
                }
#endif
            } else if (0 == strcmp(q->keys[n], PMIX_PROC_URI)) {
                /* they want our URI */
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_PROC_URI, prte_process_info.my_hnp_uri, PMIX_STRING);
                prte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_SERVER_URI)) {
                /* they want the PMIx URI */
                if (NULL != hostname) {
                    /* find the node object */
                    node = NULL;
                    for (k = 0; k < prte_node_pool->size; k++) {
                        if (NULL
                            == (ndptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool,
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
                    node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, nodeid);
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
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_URI, uri, PMIX_STRING);
                free(uri);
                prte_list_append(&results, &kv->super);
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
                kv = PRTE_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                prte_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                procinfo = (pmix_proc_info_t *) darray->array;
                p = 0;
                for (k = 0; k < jdata->procs->size; k++) {
                    if (NULL
                        == (proct = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, k))) {
                        continue;
                    }
                    PMIX_LOAD_PROCID(&procinfo[p].proc, proct->name.nspace, proct->name.rank);
                    if (NULL != proct->node && NULL != proct->node->name) {
                        procinfo[p].hostname = strdup(proct->node->name);
                    }
                    app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps,
                                                                             proct->app_idx);
                    if (NULL != app && NULL != app->app) {
                        procinfo[p].executable_name = strdup(app->app);
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
                kv = PRTE_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_QUERY_LOCAL_PROC_TABLE, PMIX_MAX_KEYLEN);
                prte_list_append(&results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_local_procs, PMIX_PROC_INFO);
                kv->info.value.type = PMIX_DATA_ARRAY;
                kv->info.value.data.darray = darray;
                procinfo = (pmix_proc_info_t *) darray->array;
                p = 0;
                for (k = 0; k < jdata->procs->size; k++) {
                    if (NULL
                        == (proct = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, k))) {
                        continue;
                    }
                    if (PRTE_FLAG_TEST(proct, PRTE_PROC_FLAG_LOCAL)) {
                        PMIX_LOAD_PROCID(&procinfo[p].proc, proct->name.nspace, proct->name.rank);
                        if (NULL != proct->node && NULL != proct->node->name) {
                            procinfo[p].hostname = strdup(proct->node->name);
                        }
                        app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps,
                                                                                 proct->app_idx);
                        if (NULL != app && NULL != app->app) {
                            procinfo[p].executable_name = strdup(app->app);
                        }
                        procinfo[p].pid = proct->pid;
                        procinfo[p].exit_code = proct->exit_code;
                        procinfo[p].state = prte_pmix_convert_state(proct->state);
                        ++p;
                    }
                }
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_NUM_PSETS)) {
                kv = PRTE_NEW(prte_info_item_t);
                sz = prte_list_get_size(&prte_pmix_server_globals.psets);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_NUM_PSETS, &sz, PMIX_SIZE);
                prte_list_append(&results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PSET_NAMES)) {
                pmix_server_pset_t *ps;
                ans = NULL;
                PRTE_LIST_FOREACH(ps, &prte_pmix_server_globals.psets, pmix_server_pset_t)
                {
                    prte_argv_append_nosize(&ans, ps->name);
                }
                tmp = prte_argv_join(ans, ',');
                prte_argv_free(ans);
                ans = NULL;
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_QUERY_PSET_NAMES, tmp, PMIX_STRING);
                prte_list_append(&results, &kv->super);
                free(tmp);
            } else if (0 == strcmp(q->keys[n], PMIX_JOB_SIZE)) {
                jdata = prte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PRTE_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = PRTE_NEW(prte_info_item_t);
                (void) strncpy(kv->info.key, PMIX_JOB_SIZE, PMIX_MAX_KEYLEN);
                key = jdata->num_procs;
                PMIX_INFO_LOAD(&kv->info, PMIX_JOB_SIZE, &key, PMIX_UINT32);
                prte_list_append(&results, &kv->super);
            } else {
                fprintf(stderr, "Query for unrecognized attribute: %s\n", q->keys[n]);
            }
        } // for
    }     // for

done:
    rcd = PRTE_NEW(prte_pmix_server_op_caddy_t);
    if (PMIX_SUCCESS == ret) {
        if (0 == prte_list_get_size(&results)) {
            ret = PMIX_ERR_NOT_FOUND;
        } else {
            if (prte_list_get_size(&results) < cd->ninfo) {
                ret = PMIX_QUERY_PARTIAL_SUCCESS;
            } else {
                ret = PMIX_SUCCESS;
            }
            /* convert the list of results to an info array */
            rcd->ninfo = prte_list_get_size(&results);
            PMIX_INFO_CREATE(rcd->info, rcd->ninfo);
            n = 0;
            PRTE_LIST_FOREACH(kv, &results, prte_info_item_t)
            {
                PMIX_INFO_XFER(&rcd->info[n], &kv->info);
                n++;
            }
        }
    }
    PRTE_LIST_DESTRUCT(&results);
    cd->infocbfunc(ret, rcd->info, rcd->ninfo, cd->cbdata, qrel, rcd);
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_query_fn(pmix_proc_t *proct, pmix_query_t *queries, size_t nqueries,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    if (NULL == queries || NULL == cbfunc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);
    memcpy(&cd->proct, proct, sizeof(pmix_proc_t));
    cd->queries = queries;
    cd->nqueries = nqueries;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _query, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}
