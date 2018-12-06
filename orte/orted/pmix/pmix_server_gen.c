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
 * Copyright (c) 2013-2018 Intel, Inc. All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
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

static void _client_conn(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    orte_job_t *jdata;
    orte_proc_t *p, *ptr;
    int i;

    ORTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the orte_proc_t */
        p = (orte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = orte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                continue;
            }
            if (cd->proc.jobid != ptr->name.jobid) {
                continue;
            }
            if (cd->proc.vpid == ptr->name.vpid) {
                p = ptr;
                break;
            }
        }
    }
    if (NULL != p) {
        ORTE_FLAG_SET(p, ORTE_PROC_FLAG_REG);
        ORTE_ACTIVATE_PROC_STATE(&p->name, ORTE_PROC_STATE_REGISTERED);
    }
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

pmix_status_t pmix_server_client_connected_fn(const pmix_proc_t *proc, void* server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_process_name_t name;
    int rc;

    OPAL_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (OPAL_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    ORTE_PMIX_THREADSHIFT(&name, server_object, OPAL_SUCCESS, NULL,
                          NULL, 0, _client_conn, cbfunc, cbdata);
    return ORTE_SUCCESS;
}

static void _client_finalized(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    orte_job_t *jdata;
    orte_proc_t *p, *ptr;
    int i;

    ORTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the orte_proc_t */
        p = (orte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = orte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                continue;
            }
            if (cd->proc.jobid != ptr->name.jobid) {
                continue;
            }
            if (cd->proc.vpid == ptr->name.vpid) {
                p = ptr;
                break;
            }
        }
        /* if we came thru this code path, then this client must be an
         * independent tool that connected to us - i.e., it wasn't
         * something we spawned. For accounting purposes, we have to
         * ensure the job complete procedure is run - otherwise, slots
         * and other resources won't correctly be released */
        ORTE_FLAG_SET(p, ORTE_PROC_FLAG_IOF_COMPLETE);
        ORTE_FLAG_SET(p, ORTE_PROC_FLAG_WAITPID);
        ORTE_ACTIVATE_PROC_STATE(&cd->proc, ORTE_PROC_STATE_TERMINATED);
    }
    if (NULL != p) {
        ORTE_FLAG_SET(p, ORTE_PROC_FLAG_HAS_DEREG);
        /* release the caller */
        if (NULL != cd->cbfunc) {
            cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
        }
    }
    OBJ_RELEASE(cd);
}

pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void* server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_process_name_t name;
    int rc;

    OPAL_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (OPAL_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    ORTE_PMIX_THREADSHIFT(&name, server_object, OPAL_SUCCESS, NULL,
                          NULL, 0, _client_finalized, cbfunc, cbdata);
    return ORTE_SUCCESS;

}

static void _client_abort(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    orte_job_t *jdata;
    orte_proc_t *p, *ptr;
    int i;

    ORTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        p = (orte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = orte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                continue;
            }
            if (cd->proc.jobid != ptr->name.jobid) {
                continue;
            }
            if (cd->proc.vpid == ptr->name.vpid) {
                p = ptr;
                break;
            }
        }
    }
    if (NULL != p) {
        p->exit_code = cd->status;
        ORTE_ACTIVATE_PROC_STATE(&p->name, ORTE_PROC_STATE_CALLED_ABORT);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(OPAL_SUCCESS, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object,
                                   int status, const char msg[],
                                   pmix_proc_t procs[], size_t nprocs,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_process_name_t name;
    int rc;

    OPAL_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (OPAL_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    ORTE_PMIX_THREADSHIFT(&name, server_object, status, msg,
                          procs, nprocs, _client_abort, cbfunc, cbdata);
    return ORTE_SUCCESS;
}

static void _register_events(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;

    ORTE_ACQUIRE_OBJECT(cd);

    /* need to implement this */

    if (NULL != cd->cbfunc) {
        cd->cbfunc(ORTE_SUCCESS, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

/* hook for the local PMIX server to pass event registrations
 * up to us - we will assume the responsibility for providing
 * notifications for registered events */
pmix_status_t pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->info = (pmix_info_t*)info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    opal_event_set(orte_event_base, &(cd->ev), -1,
                   OPAL_EV_WRITE, _register_events, cd);
    opal_event_set_priority(&(cd->ev), ORTE_MSG_PRI);
    ORTE_POST_OBJECT(cd);
    opal_event_active(&(cd->ev), OPAL_EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void _deregister_events(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;

    ORTE_ACQUIRE_OBJECT(cd);

    /* need to implement this */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(ORTE_SUCCESS, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}
/* hook for the local PMIX server to pass event deregistrations
 * up to us */
pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes, size_t ncodes,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    opal_event_set(orte_event_base, &(cd->ev), -1,
                   OPAL_EV_WRITE, _deregister_events, cd);
    opal_event_set_priority(&(cd->ev), ORTE_MSG_PRI);
    ORTE_POST_OBJECT(cd);
    opal_event_active(&(cd->ev), OPAL_EV_WRITE, 1);
    return ORTE_SUCCESS;
}

static void _notify_release(int status, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;

    ORTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    OBJ_RELEASE(cd);
}

/* someone has sent us an event that we need to distribute
 * to our local clients */
void pmix_server_notify(int status, orte_process_name_t* sender,
                        opal_buffer_t *buffer,
                        orte_rml_tag_t tg, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd;
    int cnt, rc;
    pmix_proc_t source, psender;
    opal_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    size_t ninfo;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s PRRTE Notification received from %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(sender));

    /* unpack the byte object payload */
    cnt = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &boptr, &cnt, OPAL_BYTE_OBJECT))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    free(boptr);

    /* convert the sender */
    OPAL_PMIX_CONVERT_NAME(&psender, sender);

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &code, &cnt, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    /* unpack the source */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &source, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    /* unpack the range */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &range, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    cd = OBJ_NEW(orte_pmix_server_op_caddy_t);

    /* unpack the #infos that were provided */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &cd->ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        OBJ_RELEASE(cd);
        return;
    }
    /* reserve a spot for an additional flag */
    ninfo = cd->ninfo + 1;
    /* create the space */
    PMIX_INFO_CREATE(cd->info, ninfo);

    if (0 < cd->ninfo) {
        /* unpack into it */
        cnt = cd->ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, cd->info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_INFO_FREE(cd->info, cd->ninfo);
            OBJ_RELEASE(cd);
            return;
        }
    }
    cd->ninfo = ninfo;
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

    /* protect against infinite loops by marking that this notification was
     * passed down to the server by me */
    PMIX_INFO_LOAD(&cd->info[ninfo-1], "orte.notify.donotloop", NULL, PMIX_BOOL);

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s NOTIFYING PMIX SERVER OF STATUS %s SOURCE %s RANGE %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), PMIx_Error_string(code), source.nspace, PMIx_Data_range_string(range));

    ret = PMIx_Notify_event(code, &source, range, cd->info, cd->ninfo, _notify_release, cd);
    if (PMIX_SUCCESS != ret) {
        if (PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        OBJ_RELEASE(cd);
    }
}

pmix_status_t pmix_server_notify_event(pmix_status_t code,
                                       const pmix_proc_t *source,
                                       pmix_data_range_t range,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    opal_buffer_t *buf;
    int rc;
    orte_grpcomm_signature_t *sig;
    opal_byte_object_t *boptr, bo;
    pmix_byte_object_t pbo;
    pmix_data_buffer_t pbkt;
    pmix_proc_t psender;
    pmix_status_t ret;
    size_t n;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s local process %s:%d generated event code %d range %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        source->nspace, source->rank, code, PMIx_Data_range_string(range));

    /* check to see if this is one we sent down */
    for (n=0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, "orte.notify.donotloop")) {
            /* yep - do not process */
            goto done;
        }
    }

    /* a local process has generated an event - we need to xcast it
     * to all the daemons so it can be passed down to their local
     * procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    /* convert the sender */
    OPAL_PMIX_CONVERT_NAME(&psender, ORTE_PROC_MY_NAME);

    /* pack the status code */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the source */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, (pmix_proc_t*)source, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    if (0 < ninfo) {
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return ret;
        }
    }
    /* unload the pmix buffer */
    PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
    bo.bytes = (uint8_t*)pbo.bytes;
    bo.size = pbo.size;

    /* pack it into our reply */
    buf = OBJ_NEW(opal_buffer_t);
    boptr = &bo;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(buf, &boptr, 1, OPAL_BYTE_OBJECT))) {
        ORTE_ERROR_LOG(rc);
        free(bo.bytes);
        OBJ_RELEASE(buf);
        return PMIX_ERR_PACK_FAILURE;
    }
    free(bo.bytes);

    /* goes to all daemons */
    sig = OBJ_NEW(orte_grpcomm_signature_t);
    if (NULL == sig) {
        OBJ_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    sig->signature = (orte_process_name_t*)malloc(sizeof(orte_process_name_t));
    if (NULL == sig->signature) {
        OBJ_RELEASE(buf);
        OBJ_RELEASE(sig);
        return PMIX_ERR_NOMEM;
    }
    sig->signature[0].jobid = ORTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = ORTE_VPID_WILDCARD;
    sig->sz = 1;
    if (ORTE_SUCCESS != (rc = orte_grpcomm.xcast(sig, ORTE_RML_TAG_NOTIFICATION, buf))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(buf);
        OBJ_RELEASE(sig);
        return PMIX_ERROR;
    }
    OBJ_RELEASE(buf);
    /* maintain accounting */
    OBJ_RELEASE(sig);

  done:
    /* we do not need to execute a callback as we did this atomically */
    return PMIX_OPERATION_SUCCEEDED;
}

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
    opal_ds_info_t *kv;
    orte_job_t *jdata;
    int rc;
    opal_list_t *results;
    size_t m, n;
    uint32_t key;
    void *nptr;
    char **nspaces, nspace[PMIX_MAX_NSLEN+1];
    char **ans, *tmp;
#if OPAL_PMIX_VERSION >= 3
    opal_pstats_t pstat;
    float pss;
    bool local_only;
    orte_namelist_t *nm;
    size_t p;
    opal_list_t targets;
    int i, k, num_replies;
    orte_proc_t *proct;
    orte_app_context_t *app;
    orte_jobid_t jobid;
    pmix_proc_info_t *procinfo;
    pmix_info_t *info;
    pmix_data_array_t *darray;
#endif

    ORTE_ACQUIRE_OBJECT(cd);

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s processing query",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    results = OBJ_NEW(opal_list_t);

    /* see what they wanted */
    for (m=0; m < cd->nqueries; m++) {
        q = &cd->queries[m];
        for (n=0; NULL != q->keys[n]; n++) {
            opal_output_verbose(2, orte_pmix_server_globals.output,
                                "%s processing key %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), q->keys[n]);
            if (0 == strcmp(q->keys[n], PMIX_QUERY_NAMESPACES)) {
                /* get the current jobids */
                nspaces = NULL;
                rc = opal_hash_table_get_first_key_uint32(orte_job_data, &key, (void **)&jdata, &nptr);
                while (OPAL_SUCCESS == rc) {
                    if (ORTE_PROC_MY_NAME->jobid != jdata->jobid) {
                        memset(nspace, 0, PMIX_MAX_NSLEN);
                        OPAL_PMIX_CONVERT_JOBID(nspace, jdata->jobid);
                        opal_argv_append_nosize(&nspaces, nspace);
                    }
                    rc = opal_hash_table_get_next_key_uint32(orte_job_data, &key, (void **)&jdata, nptr, &nptr);
                }
                /* join the results into a single comma-delimited string */
                kv = OBJ_NEW(opal_ds_info_t);
                tmp = opal_argv_join(nspaces, ',');
                opal_argv_free(nspaces);
                PMIX_INFO_CREATE(kv->info, 1);
                PMIX_INFO_LOAD(kv->info, PMIX_QUERY_NAMESPACES, tmp, PMIX_STRING);
                free(tmp);
                opal_list_append(results, &kv->super);
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
                kv = OBJ_NEW(opal_ds_info_t);
                tmp = opal_argv_join(ans, ',');
                opal_argv_free(ans);
                PMIX_INFO_CREATE(kv->info, 1);
                PMIX_INFO_LOAD(kv->info, PMIX_QUERY_SPAWN_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                opal_list_append(results, &kv->super);
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_DEBUG_SUPPORT)) {
                ans = NULL;
                opal_argv_append_nosize(&ans, PMIX_DEBUG_STOP_IN_INIT);
                opal_argv_append_nosize(&ans, PMIX_DEBUG_JOB);
                opal_argv_append_nosize(&ans, PMIX_DEBUG_WAIT_FOR_NOTIFY);
                /* create the return kv */
                kv = OBJ_NEW(opal_ds_info_t);
                tmp = opal_argv_join(ans, ',');
                opal_argv_free(ans);
                PMIX_INFO_CREATE(kv->info, 1);
                PMIX_INFO_LOAD(kv->info, PMIX_QUERY_DEBUG_SUPPORT, tmp, PMIX_STRING);
                free(tmp);
                opal_list_append(results, &kv->super);
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
                        kv = OBJ_NEW(opal_ds_info_t);
                        PMIX_INFO_CREATE(kv->info, 1);
                        (void)strncpy(kv->info->key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                        opal_list_append(results, &kv->super);
                        /* create an entry for myself plus the avg of all local procs */
                        PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
                        kv->info->value.type = PMIX_DATA_ARRAY;
                        kv->info->value.data.darray = darray;
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
            } else if (0 == strncmp(q->keys[n], PMIX_TIME_REMAINING, PMIX_MAX_KEYLEN)) {
                if (ORTE_SUCCESS == orte_schizo.get_remaining_time(&key)) {
                    kv = OBJ_NEW(opal_ds_info_t);
                    PMIX_INFO_CREATE(kv->info, 1);
                    PMIX_INFO_LOAD(kv->info, PMIX_TIME_REMAINING, &key, PMIX_UINT32);
                    opal_list_append(results, &kv->super);
                }
            } else if (0 == strncmp(q->keys[n], PMIX_HWLOC_XML_V1, PMIX_MAX_KEYLEN)) {
                if (NULL != opal_hwloc_topology) {
                    char *xmlbuffer=NULL;
                    int len;
                    kv = OBJ_NEW(opal_ds_info_t);
                    PMIX_INFO_CREATE(kv->info, 1);
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
                    PMIX_INFO_LOAD(kv->info, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    opal_list_append(results, &kv->super);
                }
            } else if (0 == strncmp(q->keys[n], PMIX_HWLOC_XML_V2, PMIX_MAX_KEYLEN)) {
                /* we cannot provide it if we are using v1.x */
            #if HWLOC_API_VERSION >= 0x20000
                if (NULL != opal_hwloc_topology) {
                    char *xmlbuffer=NULL;
                    int len;
                    kv = OBJ_NEW(opal_ds_info_t);
                    PMIX_INFO_CREATE(kv->info, 1);
                    if (0 != hwloc_topology_export_xmlbuffer(opal_hwloc_topology, &xmlbuffer, &len, 0)) {
                        OBJ_RELEASE(kv);
                        continue;
                    }
                    PMIX_INFO_LOAD(kv->info, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
                    free(xmlbuffer);
                    opal_list_append(results, &kv->super);
                }
            #endif
            } else if (0 == strncmp(q->keys[n], PMIX_PROC_URI, PMIX_MAX_KEYLEN)) {
                /* they want our URI */
                kv = OBJ_NEW(opal_ds_info_t);
                PMIX_INFO_CREATE(kv->info, 1);
                PMIX_INFO_LOAD(kv->info, PMIX_PROC_URI, orte_process_info.my_hnp_uri, PMIX_STRING);
                opal_list_append(results, &kv->super);
            } else if (0 == strncmp(q->keys[n], PMIX_SERVER_URI, PMIX_MAX_KEYLEN)) {
                /* they want our PMIx URI */
                kv = OBJ_NEW(opal_ds_info_t);
                PMIX_INFO_CREATE(kv->info, 1);
                PMIX_INFO_LOAD(kv->info, PMIX_SERVER_URI, orte_process_info.my_hnp_uri, PMIX_STRING);
                opal_list_append(results, &kv->super);
    #if OPAL_PMIX_VERSION >= 3
            } else if (0 == strcmp(q->keys[n], PMIX_QUERY_PROC_TABLE)) {
                /* the job they are asking about is in the qualifiers */
                jobid = ORTE_JOBID_INVALID;
                for (k=0; k < (int)q->nqual; k++) {
                    if (0 == strncmp(q->qualifiers[k].key, PMIX_NSPACE, PMIX_MAX_KEYLEN)) {
                                /* save the id */
                        OPAL_PMIX_CONVERT_NSPACE(rc, &jobid, q->qualifiers[k].value.data.string);
                        if (OPAL_SUCCESS != rc) {
                            ORTE_ERROR_LOG(rc);
                        }
                        break;
                    }
                }
                if (ORTE_JOBID_INVALID == jobid) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* construct a list of values with opal_proc_info_t
                 * entries for each proc in the indicated job */
                jdata = orte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    ret = PMIX_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = OBJ_NEW(opal_ds_info_t);
                PMIX_INFO_CREATE(kv->info, 1);
                (void)strncpy(kv->info->key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
                opal_list_append(results, &kv->super);
                 /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_procs, PMIX_PROC_INFO);
                kv->info->value.type = PMIX_DATA_ARRAY;
                kv->info->value.data.darray = darray;
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
            } else if (0 == strncmp(q->keys[n], PMIX_QUERY_LOCAL_PROC_TABLE, PMIX_MAX_KEYLEN)) {
                /* the job they are asking about is in the qualifiers */
                jobid = ORTE_JOBID_INVALID;
                for (k=0; k < (int)q->nqual; k++) {
                    if (0 == strncmp(q->qualifiers[k].key, PMIX_NSPACE, PMIX_MAX_KEYLEN)) {
                                /* save the id */
                        OPAL_PMIX_CONVERT_NSPACE(rc, &jobid, q->qualifiers[k].value.data.string);
                        if (OPAL_SUCCESS != rc) {
                            ORTE_ERROR_LOG(rc);
                        }
                        break;
                    }
                }
                if (ORTE_JOBID_INVALID == jobid) {
                    ret = PMIX_ERR_BAD_PARAM;
                    goto done;
                }
                /* construct a list of values with opal_proc_info_t
                 * entries for each LOCAL proc in the indicated job */
                jdata = orte_get_job_data_object(jobid);
                if (NULL == jdata) {
                    rc = ORTE_ERR_NOT_FOUND;
                    goto done;
                }
                /* setup the reply */
                kv = OBJ_NEW(opal_ds_info_t);
                PMIX_INFO_CREATE(kv->info, 1);
                (void)strncpy(kv->info->key, PMIX_QUERY_LOCAL_PROC_TABLE, PMIX_MAX_KEYLEN);
                opal_list_append(results, &kv->super);
                /* cycle thru the job and create an entry for each proc */
                PMIX_DATA_ARRAY_CREATE(darray, jdata->num_local_procs, PMIX_PROC_INFO);
                kv->info->value.type = PMIX_DATA_ARRAY;
                kv->info->value.data.darray = darray;
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
            }
        } // for
    } // for

#if OPAL_PMIX_VERSION >= 3
  done:
#endif
    rcd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    if (PMIX_SUCCESS == ret) {
        if (0 == opal_list_get_size(results)) {
            ret = PMIX_ERR_NOT_FOUND;
        } else {
            if (opal_list_get_size(results) < cd->ninfo) {
                ret = PMIX_QUERY_PARTIAL_SUCCESS;
            } else {
                ret = PMIX_SUCCESS;
            }
        /* convert the list of results to an info array */
            rcd->ninfo = opal_list_get_size(results);
            PMIX_INFO_CREATE(rcd->info, rcd->ninfo);
            n=0;
            OPAL_LIST_FOREACH(kv, results, opal_ds_info_t) {
                PMIX_INFO_XFER(&rcd->info[n], kv->info);
                n++;
            }
        }
    }
    cd->infocbfunc(ret, rcd->info, rcd->ninfo, cd->cbdata, qrel, rcd);
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

static void _toolconn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *cd = (pmix_server_req_t*)cbdata;
    orte_job_t *jdata = NULL;
    char *hostname = NULL;
    orte_process_name_t tool = {ORTE_JOBID_INVALID, ORTE_VPID_INVALID};
    int rc;
    uid_t uid=0;
    gid_t gid=0;
    size_t n;
    bool flag = true;
    pmix_proc_t pname;
    opal_buffer_t *buf;
    orte_plm_cmd_flag_t command = ORTE_PLM_ALLOC_JOBID_CMD;
    pmix_status_t xrc;

    ORTE_ACQUIRE_OBJECT(cd);

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s TOOL CONNECTION PROCESSING",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* check for directives */
    if (NULL != cd->info) {
        for (n=0; n < cd->ninfo; n++) {
            if (0 == strncmp(cd->info[n].key, PMIX_EVENT_SILENT_TERMINATION, PMIX_MAX_KEYLEN)) {
                flag = PMIX_INFO_TRUE(&cd->info[n]);
            } else if (0 == strncmp(cd->info[n].key, PMIX_VERSION_INFO, PMIX_MAX_KEYLEN)) {
                /* we ignore this for now */
            } else if (0 == strncmp(cd->info[n].key, PMIX_USERID, PMIX_MAX_KEYLEN)) {
                uid = cd->info[n].value.data.uint32;
            } else if (0 == strncmp(cd->info[n].key, PMIX_GRPID, PMIX_MAX_KEYLEN)) {
                gid = cd->info[n].value.data.uint32;
            } else if (0 == strncmp(cd->info[n].key, PMIX_NSPACE, PMIX_MAX_KEYLEN)) {
                 OPAL_PMIX_CONVERT_NSPACE(rc, &tool.jobid, cd->info[n].value.data.string);
                 if (ORTE_SUCCESS != rc) {
                    ORTE_ERROR_LOG(rc);
                 }
            } else if (0 == strncmp(cd->info[n].key, PMIX_RANK, PMIX_MAX_KEYLEN)) {
                OPAL_PMIX_CONVERT_RANK(tool.vpid, cd->info[n].value.data.rank);
            } else if (0 == strncmp(cd->info[n].key, PMIX_HOSTNAME, PMIX_MAX_KEYLEN)) {
                hostname = cd->info[n].value.data.string;
            }
        }
    }

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s TOOL CONNECTION FROM UID %d GID %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), uid, gid);

    /* if we are not the HNP or master, and the tool doesn't
     * already have a name (i.e., we didn't spawn it), then
     * there is nothing we can currently do.
     * Eventually, when we switch to nspace instead of an
     * integer jobid, we'll just locally assign this value */
    if (ORTE_JOBID_INVALID == tool.jobid ||
        ORTE_VPID_INVALID == tool.vpid) {
       /* if we are the HNP, we can directly assign the jobid */
        if (ORTE_PROC_IS_HNP || ORTE_PROC_IS_MASTER) {
            jdata = OBJ_NEW(orte_job_t);
            rc = orte_plm_base_create_jobid(jdata);
            if (ORTE_SUCCESS != rc) {
                OBJ_RELEASE(jdata);
                if (NULL != cd->toolcbfunc) {
                    OPAL_PMIX_CONVERT_NAME(&pname, &tool);
                    cd->toolcbfunc(PMIX_ERROR, &pname, cd->cbdata);
                }
                OBJ_RELEASE(cd);
                return;
            }
            tool.jobid = jdata->jobid;
            tool.vpid = 0;
        } else {
            if (OPAL_SUCCESS != (rc = opal_hotel_checkin(&orte_pmix_server_globals.reqs, cd, &cd->room_num))) {
                orte_show_help("help-orted.txt", "noroom", true, cd->operation, orte_pmix_server_globals.num_rooms);
                goto callback;
            }
            /* we need to send this to the HNP for a jobid */
            if (NULL != hostname) {
                cd->operation = strdup(hostname);  // pass the hostname
            }
            cd->flag = flag;
            buf = OBJ_NEW(opal_buffer_t);
            opal_dss.pack(buf, &command, 1, ORTE_PLM_CMD);
            opal_dss.pack(buf, &cd->room_num, 1, OPAL_INT);
            /* send it to the HNP for processing - might be myself! */
            if (ORTE_SUCCESS != (rc = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                                              ORTE_PROC_MY_HNP, buf,
                                                              ORTE_RML_TAG_PLM,
                                                              orte_rml_send_callback, NULL))) {
                ORTE_ERROR_LOG(rc);
                xrc = opal_pmix_convert_rc(rc);
                opal_hotel_checkout_and_return_occupant(&orte_pmix_server_globals.reqs, cd->room_num, (void**)&cd);
                OBJ_RELEASE(buf);
                if (NULL != cd->toolcbfunc) {
                    cd->toolcbfunc(xrc, NULL, cd->cbdata);
                }
                OBJ_RELEASE(cd);
            }
            return;
        }
    } else {
        jdata = OBJ_NEW(orte_job_t);
        jdata->jobid = tool.jobid;
    }
    orte_pmix_server_tool_conn_complete(jdata, hostname, tool.vpid);
    /* if they indicated a preference for termination, set it */
    if (flag) {
        orte_set_attribute(&jdata->attributes, ORTE_JOB_SILENT_TERMINATION,
                           ORTE_ATTR_GLOBAL, NULL, OPAL_BOOL);
    }
    rc = ORTE_SUCCESS;

  callback:
    if (NULL != cd->toolcbfunc) {
        OPAL_PMIX_CONVERT_NAME(&pname, &tool);
        xrc = opal_pmix_convert_rc(rc);
        cd->toolcbfunc(xrc, &pname, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

void orte_pmix_server_tool_conn_complete(orte_job_t *jdata,
                                         char *hostname,
                                         orte_vpid_t vpid)
{
    orte_app_context_t *app;
    orte_proc_t *proc;
    orte_node_t *node, *nptr;
    int i;

    opal_hash_table_set_value_uint32(orte_job_data, jdata->jobid, jdata);
    /* setup some required job-level fields in case this
     * tool calls spawn, or uses some other functions that
     * need them */
    /* must create a map for it (even though it has no
     * info in it) so that the job info will be picked
     * up in subsequent pidmaps or other daemons won't
     * know how to route
     */
    jdata->map = OBJ_NEW(orte_job_map_t);

    /* setup an app_context for the singleton */
    app = OBJ_NEW(orte_app_context_t);
    app->app = strdup("tool");
    app->num_procs = 1;
    opal_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;

    /* setup a proc object for the singleton - since we
     * -must- be the HNP, and therefore we stored our
     * node on the global node pool, and since the singleton
     * -must- be on the same node as us, indicate that
     */
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = jdata->jobid;
    proc->name.vpid = vpid;
    proc->parent = ORTE_PROC_MY_NAME->vpid;
    ORTE_FLAG_SET(proc, ORTE_PROC_FLAG_ALIVE);
    ORTE_FLAG_SET(proc, ORTE_PROC_FLAG_TOOL);
    proc->state = ORTE_PROC_STATE_RUNNING;
    /* set the trivial */
    proc->local_rank = 0;
    proc->node_rank = 0;
    proc->app_rank = 0;
    proc->app_idx = 0;
    if (NULL == hostname) {
        /* it is on my node */
        node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, 0);
        ORTE_FLAG_SET(proc, ORTE_PROC_FLAG_LOCAL);
    } else {
        /* we need to locate it */
        node = NULL;
        for (i=0; i < orte_node_pool->size; i++) {
            if (NULL == (nptr = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, i))) {
                continue;
            }
            if (0 == strcmp(hostname, nptr->name)) {
                node = nptr;
                break;
            }
        }
        if (NULL == node) {
            /* not in our allocation - which is still okay */
            node = OBJ_NEW(orte_node_t);
            node->name = strdup(hostname);
            ORTE_FLAG_SET(node, ORTE_NODE_NON_USABLE);
            opal_pointer_array_add(orte_node_pool, node);
        }
    }
    proc->node = node;
    OBJ_RETAIN(node);  /* keep accounting straight */
    opal_pointer_array_add(jdata->procs, proc);
    jdata->num_procs = 1;
    /* add the node to the job map */
    OBJ_RETAIN(node);
    opal_pointer_array_add(jdata->map->nodes, node);
    jdata->map->num_nodes++;
    /* and it obviously is on the node - note that
     * we do _not_ increment the #procs on the node
     * as the tool doesn't count against the slot
     * allocation */
    OBJ_RETAIN(proc);
    opal_pointer_array_add(node->procs, proc);
}

void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                            pmix_tool_connection_cbfunc_t cbfunc,
                            void *cbdata)
{
    pmix_server_req_t *cd;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s TOOL CONNECTION REQUEST RECVD",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = OBJ_NEW(pmix_server_req_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;

    opal_event_set(orte_event_base, &(cd->ev), -1,
                   OPAL_EV_WRITE, _toolconn, cd);
    opal_event_set_priority(&(cd->ev), ORTE_MSG_PRI);
    ORTE_POST_OBJECT(cd);
    opal_event_active(&(cd->ev), OPAL_EV_WRITE, 1);

}

static void lgcbfn(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

void pmix_server_log_fn(const pmix_proc_t *client,
                        const pmix_info_t data[], size_t ndata,
                        const pmix_info_t directives[], size_t ndirs,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n, cnt;
    opal_buffer_t *buf;
    opal_byte_object_t bo, *boptr;
    int rc = ORTE_SUCCESS;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t pbo;
    pmix_proc_t psender;
    pmix_status_t ret;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s logging info",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    OPAL_PMIX_CONVERT_NAME(&psender, ORTE_PROC_MY_NAME);
    cnt = 0;

    for (n=0; n < ndata; n++) {
        if (0 == strncmp(data[n].key, ORTE_PMIX_SHOW_HELP, PMIX_MAX_KEYLEN)) {
            /* pull out the blob */
            if (PMIX_BYTE_OBJECT != data[n].value.type) {
                continue;
            }
            buf = OBJ_NEW(opal_buffer_t);
            opal_dss.load(buf, data[n].value.data.bo.bytes, data[n].value.data.bo.size);
            if (ORTE_SUCCESS != (rc = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                                              ORTE_PROC_MY_HNP, buf,
                                                              ORTE_RML_TAG_SHOW_HELP,
                                                              orte_rml_send_callback, NULL))) {
                ORTE_ERROR_LOG(rc);
                buf->base_ptr = NULL;
                OBJ_RELEASE(buf);
            }
        } else if (ORTE_PROC_IS_HNP || ORTE_PROC_IS_MASTER) {
            /* we can't support this */
            rc = ORTE_ERR_NOT_SUPPORTED;
        } else {
            /* we need to ship this to our HNP/MASTER for processing */
            ret = PMIx_Data_pack(&psender, &pbuf, (pmix_info_t*)&data[n], 1, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            }
            ++cnt;
        }
    }
    if (0 < cnt) {
        buf = OBJ_NEW(opal_buffer_t);
        opal_dss.pack(buf, &cnt, 1, OPAL_SIZE);
        PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;
        boptr = &bo;
        opal_dss.pack(buf, &boptr, 1, OPAL_BYTE_OBJECT);
        free(bo.bytes);
        rc = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                     ORTE_PROC_MY_HNP, buf,
                                     ORTE_RML_TAG_LOGGING,
                                     orte_rml_send_callback, NULL);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(buf);
        }
    }
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    ORTE_PMIX_THREADSHIFT(ORTE_NAME_WILDCARD, NULL, rc,
                          NULL, NULL, 0, lgcbfn,
                          cbfunc, cbdata);
}

pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor,
                                      const pmix_proc_t targets[], size_t ntargets,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int rc, j;
    size_t m, n;
    orte_proc_t *proc;
    opal_pointer_array_t parray, *ptrarray;
    orte_process_name_t name;
    opal_buffer_t *cmd;
    orte_daemon_cmd_flag_t cmmnd = ORTE_DAEMON_HALT_VM_CMD;
    orte_grpcomm_signature_t *sig;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m=0; m < ndirs; m++) {
        if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_KILL, PMIX_MAX_KEYLEN)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                OBJ_CONSTRUCT(&parray, opal_pointer_array_t);
                for (n=0; n < ntargets; n++) {
                    OPAL_PMIX_CONVERT_PROCT(rc, &name, &targets[n]);
                    if (OPAL_SUCCESS != rc) {
                        ORTE_ERROR_LOG(rc);
                        return PMIX_ERR_BAD_PARAM;
                    }
                    if (ORTE_VPID_WILDCARD == name.vpid) {
                        /* create an object */
                        proc = OBJ_NEW(orte_proc_t);
                        proc->name.jobid = name.jobid;
                        proc->name.vpid = ORTE_VPID_WILDCARD;
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = orte_get_proc_object(&name))) {
                            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
                            continue;
                        }
                        OBJ_RETAIN(proc);
                    }
                    opal_pointer_array_add(&parray, proc);
                }
                ptrarray = &parray;
            }
            if (ORTE_SUCCESS != (rc = orte_plm.terminate_procs(ptrarray))) {
                ORTE_ERROR_LOG(rc);
            }
            if (NULL != ptrarray) {
                /* cleanup the array */
                for (j=0; j < parray.size; j++) {
                    if (NULL != (proc = (orte_proc_t*)opal_pointer_array_get_item(&parray, j))) {
                        OBJ_RELEASE(proc);
                    }
                }
                OBJ_DESTRUCT(&parray);
            }
            continue;
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_TERMINATE, PMIX_MAX_KEYLEN)) {
            if (NULL == targets) {
                /* terminate the daemons and all running jobs */
                cmd = OBJ_NEW(opal_buffer_t);
                /* pack the command */
                if (ORTE_SUCCESS != (rc = opal_dss.pack(cmd, &cmmnd, 1, ORTE_DAEMON_CMD))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_RELEASE(cmd);
                    return rc;
                }
                /* goes to all daemons */
                sig = OBJ_NEW(orte_grpcomm_signature_t);
                sig->signature = (orte_process_name_t*)malloc(sizeof(orte_process_name_t));
                sig->signature[0].jobid = ORTE_PROC_MY_NAME->jobid;
                sig->signature[0].vpid = ORTE_VPID_WILDCARD;
                if (ORTE_SUCCESS != (rc = orte_grpcomm.xcast(sig, ORTE_RML_TAG_DAEMON, cmd))) {
                    ORTE_ERROR_LOG(rc);
                }
                OBJ_RELEASE(cmd);
                OBJ_RELEASE(sig);
            }
        }
    }

    return PMIX_OPERATION_SUCCEEDED;
}

#if OPAL_PMIX_VERSION >= 4
static void relcb(void *cbdata)
{
    orte_pmix_mdx_caddy_t *cd=(orte_pmix_mdx_caddy_t*)cbdata;

    OBJ_RELEASE(cd);
}
static void group_release(int status, opal_buffer_t *buf, void *cbdata)
{
    orte_pmix_mdx_caddy_t *cd = (orte_pmix_mdx_caddy_t*)cbdata;
    int32_t cnt;
    int rc;
    pmix_status_t ret;
    size_t cid, ninfo = 0;
    pmix_info_t *info = NULL;

    ORTE_ACQUIRE_OBJECT(cd);

    if (ORTE_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    /* if a context id was provided, get it */
    cnt = 1;
    rc = opal_dss.unpack(buf, &cid, &cnt, OPAL_SIZE);
    /* it is okay if they didn't */
    if (ORTE_SUCCESS != rc && ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        ORTE_ERROR_LOG(rc);
        goto complete;
    }
    if (ORTE_SUCCESS == rc) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
    }
    if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        rc = ORTE_SUCCESS;
    }

  complete:
    ret = opal_pmix_convert_rc(rc);
    /* return to the local procs in the collective */
    if (NULL != cd->infocbfunc) {
        cd->infocbfunc(ret, info, ninfo, cd->cbdata, relcb, cd);
    } else {
        if (NULL != info) {
            PMIX_INFO_FREE(info, ninfo);
        }
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    orte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i, mode = 0;

    if (PMIX_GROUP_CONSTRUCT == op) {
        /* check the directives for a request to assign a context id */
        for (i=0; i < ndirs; i++) {
            if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
                if (PMIX_INFO_TRUE(&directives[i])) {
                    mode = 1;
                }
                break;
            }
        }
    }

    cd = OBJ_NEW(orte_pmix_mdx_caddy_t);
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

   /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = OBJ_NEW(orte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (orte_process_name_t*)malloc(cd->sig->sz * sizeof(orte_process_name_t));
        memset(cd->sig->signature, 0, cd->sig->sz * sizeof(orte_process_name_t));
        for (i=0; i < nprocs; i++) {
            OPAL_PMIX_CONVERT_PROCT(rc, &cd->sig->signature[i], &procs[i]);
            if (OPAL_SUCCESS != rc) {
                OPAL_ERROR_LOG(rc);
                OBJ_RELEASE(cd);
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }
    cd->buf = OBJ_NEW(opal_buffer_t);

    /* pass it to the global collective algorithm */
    if (ORTE_SUCCESS != (rc = orte_grpcomm.allgather(cd->sig, cd->buf, mode,
                                                     group_release, cd))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}
#endif
