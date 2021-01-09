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
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/mca/rml/rml.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void pmix_server_stdin_push(int sd, short args, void *cbdata);

static void _client_conn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;
    prte_job_t *jdata;
    prte_proc_t *p, *ptr;
    int i;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, i))) {
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
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_REG);
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_REGISTERED);
    }
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_client_connected_fn(const pmix_proc_t *proc, void* server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_process_name_t name;
    int rc;

    PRTE_PMIX_CONVERT_PROCT(rc, &name, (pmix_proc_t*)proc);
    if (PRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(&name, server_object, PRTE_SUCCESS, NULL,
                          NULL, 0, _client_conn, cbfunc, cbdata);
    return PRTE_SUCCESS;
}

static void _client_finalized(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;
    prte_job_t *jdata;
    prte_proc_t *p, *ptr;
    int i;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prte_get_job_data_object(cd->proc.jobid))) {
            /* this tool was not started by us and we have
             * no job record for it - this shouldn't happen,
             * so let's error log it */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            /* ensure they don't hang */
            goto release;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, i))) {
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
        if (NULL != p) {
            /* if we came thru this code path, then this client must be an
             * independent tool that connected to us - i.e., it wasn't
             * something we spawned. For accounting purposes, we have to
             * ensure the job complete procedure is run - otherwise, slots
             * and other resources won't correctly be released */
            PRTE_FLAG_SET(p, PRTE_PROC_FLAG_IOF_COMPLETE);
            PRTE_FLAG_SET(p, PRTE_PROC_FLAG_WAITPID);
        }
        PRTE_ACTIVATE_PROC_STATE(&cd->proc, PRTE_PROC_STATE_TERMINATED);
    }
    if (NULL != p) {
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_HAS_DEREG);
    }

  release:
    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void* server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_process_name_t name;
    int rc;

    PRTE_PMIX_CONVERT_PROCT(rc, &name, (pmix_proc_t*)proc);
    if (PRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(&name, server_object, PRTE_SUCCESS, NULL,
                          NULL, 0, _client_finalized, cbfunc, cbdata);
    return PRTE_SUCCESS;

}

static void _client_abort(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;
    prte_job_t *jdata;
    prte_proc_t *p, *ptr;
    int i;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        p = (prte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, i))) {
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
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_CALLED_ABORT);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object,
                                   int status, const char msg[],
                                   pmix_proc_t procs[], size_t nprocs,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_process_name_t name;
    int rc;

    PRTE_PMIX_CONVERT_PROCT(rc, &name, (pmix_proc_t*)proc);
    if (PRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(&name, server_object, status, msg,
                          procs, nprocs, _client_abort, cbfunc, cbdata);
    return PRTE_SUCCESS;
}

static void _register_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(cd);

    /* need to implement this */

    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

/* hook for the local PMIX server to pass event registrations
 * up to us - we will assume the responsibility for providing
 * notifications for registered events */
pmix_status_t pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->info = (pmix_info_t*)info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1,
                   PRTE_EV_WRITE, _register_events, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void _deregister_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(cd);

    /* need to implement this */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}
/* hook for the local PMIX server to pass event deregistrations
 * up to us */
pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes, size_t ncodes,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1,
                   PRTE_EV_WRITE, _deregister_events, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void _notify_release(int status, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRTE_RELEASE(cd);
}

/* someone has sent us an event that we need to distribute
 * to our local clients */
void pmix_server_notify(int status, prte_process_name_t* sender,
                        prte_buffer_t *buffer,
                        prte_rml_tag_t tg, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    int cnt, rc;
    pmix_proc_t source;
    prte_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    size_t ninfo;
    prte_jobid_t jobid;
    prte_job_t *jdata;
    prte_vpid_t vpid;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s PRTE Notification received from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the daemon who broadcast the event */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &vpid, &cnt, PRTE_VPID))) {
        PRTE_ERROR_LOG(rc);
        return;
    }
    /* if I am the one who sent it, then discard it */
    if (vpid == PRTE_PROC_MY_NAME->vpid) {
        return;
    }

    /* unpack the byte object payload */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &boptr, &cnt, PRTE_BYTE_OBJECT))) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    free(boptr);

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &code, &cnt, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    /* unpack the source */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &source, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    /* unpack the range */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &range, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);

    /* unpack the #infos that were provided */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &cd->ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        PRTE_RELEASE(cd);
        return;
    }
    /* reserve a spot for an additional flag */
    ninfo = cd->ninfo + 1;
    /* create the space */
    PMIX_INFO_CREATE(cd->info, ninfo);

    if (0 < cd->ninfo) {
        /* unpack into it */
        cnt = cd->ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, cd->info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_INFO_FREE(cd->info, cd->ninfo);
            PRTE_RELEASE(cd);
            return;
        }
    }
    cd->ninfo = ninfo;
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

    /* protect against infinite loops by marking that this notification was
     * passed down to the server by me */
    PMIX_INFO_LOAD(&cd->info[ninfo-1], "prte.notify.donotloop", NULL, PMIX_BOOL);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s NOTIFYING PMIX SERVER OF STATUS %s SOURCE %s RANGE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(code), source.nspace, PMIx_Data_range_string(range));

    ret = PMIx_Notify_event(code, &source, range, cd->info, cd->ninfo, _notify_release, cd);
    if (PMIX_SUCCESS != ret) {
        if (PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PRTE_RELEASE(cd);
    }

    if (PMIX_ERR_JOB_TERMINATED == code) {
        PRTE_PMIX_CONVERT_NSPACE(rc, &jobid, source.nspace);
        jdata = prte_get_job_data_object(jobid);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
    }
}

pmix_status_t pmix_server_notify_event(pmix_status_t code,
                                       const pmix_proc_t *source,
                                       pmix_data_range_t range,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_buffer_t *buf;
    int rc;
    prte_grpcomm_signature_t *sig;
    prte_byte_object_t *boptr, bo;
    pmix_byte_object_t pbo;
    pmix_data_buffer_t pbkt;
    pmix_status_t ret;
    size_t n;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s local process %s:%d generated event code %s range %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        source->nspace, source->rank,
                        PMIx_Error_string(code),
                        PMIx_Data_range_string(range));

    /* we can get events prior to completing prte_init as we have
     * to init PMIx early so that PRRTE components can use it */
    PRTE_ACQUIRE_THREAD(&prte_init_lock);
    if (!prte_initialized) {
        PRTE_RELEASE_THREAD(&prte_init_lock);
        goto done;
    }
    PRTE_RELEASE_THREAD(&prte_init_lock);

    /* check to see if this is one we sent down */
    for (n=0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, "prte.notify.donotloop")) {
            /* yep - do not process */
            goto done;
        }
    }

    /* if this is notification of procs being ready for debug, then
     * we treat this as a state change */
    if (PMIX_DEBUG_WAITING_FOR_NOTIFY == code) {

    }
    /* a local process has generated an event - we need to xcast it
     * to all the daemons so it can be passed down to their local
     * procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* pack the status code */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the source */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_proc_t*)source, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    if (0 < ninfo) {
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return ret;
        }
    }
    /* unload the pmix buffer */
    PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
    bo.bytes = (uint8_t*)pbo.bytes;
    bo.size = pbo.size;

    /* setup the broadcast */
    buf = PRTE_NEW(prte_buffer_t);
    /* we need to add a flag indicating this came from us as we are going to get it echoed
     * back to us by the broadcast */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(buf, &PRTE_PROC_MY_NAME->vpid, 1, PRTE_VPID))) {
        PRTE_ERROR_LOG(rc);
        free(bo.bytes);
        PRTE_RELEASE(buf);
        return PMIX_ERR_PACK_FAILURE;
    }
    /* add the payload */
    boptr = &bo;
    if (PRTE_SUCCESS != (rc = prte_dss.pack(buf, &boptr, 1, PRTE_BYTE_OBJECT))) {
        PRTE_ERROR_LOG(rc);
        free(bo.bytes);
        PRTE_RELEASE(buf);
        return PMIX_ERR_PACK_FAILURE;
    }
    free(bo.bytes);

    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    if (NULL == sig) {
        PRTE_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    sig->signature = (prte_process_name_t*)malloc(sizeof(prte_process_name_t));
    if (NULL == sig->signature) {
        PRTE_RELEASE(buf);
        PRTE_RELEASE(sig);
        return PMIX_ERR_NOMEM;
    }
    sig->signature[0].jobid = PRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRTE_VPID_WILDCARD;
    sig->sz = 1;
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_NOTIFICATION, buf))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        PRTE_RELEASE(sig);
        return PMIX_ERROR;
    }
    PRTE_RELEASE(buf);
    /* maintain accounting */
    PRTE_RELEASE(sig);

  done:
    /* we do not need to execute a callback as we did this atomically */
    return PMIX_OPERATION_SUCCEEDED;
}

static void _toolconn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *cd = (pmix_server_req_t*)cbdata;
    prte_job_t *jdata = NULL;
    int rc;
    size_t n;
    pmix_proc_t pname;
    prte_buffer_t *buf;
    prte_plm_cmd_flag_t command = PRTE_PLM_ALLOC_JOBID_CMD;
    pmix_status_t xrc;

    PRTE_ACQUIRE_OBJECT(cd);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION PROCESSING",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* check for directives */
    if (NULL != cd->info) {
        for (n=0; n < cd->ninfo; n++) {
            if (PMIX_CHECK_KEY(&cd->info[n], PMIX_EVENT_SILENT_TERMINATION)) {
                cd->flag = PMIX_INFO_TRUE(&cd->info[n]);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_VERSION_INFO)) {
                /* we ignore this for now */
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_USERID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->uid, uid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PRTE_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GRPID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->gid, gid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PRTE_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_NSPACE)) {
                 PRTE_PMIX_CONVERT_NSPACE(rc, &cd->target.jobid, cd->info[n].value.data.string);
                 if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                 }
             } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_RANK)) {
                PRTE_PMIX_CONVERT_RANK(cd->target.vpid, cd->info[n].value.data.rank);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_HOSTNAME)) {
                cd->operation = strdup(cd->info[n].value.data.string);
#ifdef PMIX_CMD_LINE
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CMD_LINE)) {
                cd->cmdline = strdup(cd->info[n].value.data.string);
#endif
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_LAUNCHER)) {
                cd->launcher = PMIX_INFO_TRUE(&cd->info[n]);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_PROC_PID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->pid, pid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PRTE_RELEASE(cd);
                    return;
                }
            }
        }
    }

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION FROM UID %d GID %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cd->uid, cd->gid);

    /* if we are not the HNP or master, and the tool doesn't
     * already have a name (i.e., we didn't spawn it), then
     * there is nothing we can currently do.
     * Eventually, when we switch to nspace instead of an
     * integer jobid, we'll just locally assign this value */
    if (PRTE_JOBID_INVALID == cd->target.jobid ||
        PRTE_VPID_INVALID == cd->target.vpid) {
       /* if we are the HNP, we can directly assign the jobid */
        if (PRTE_PROC_IS_MASTER) {
            jdata = PRTE_NEW(prte_job_t);
            rc = prte_plm_base_create_jobid(jdata);
            if (PRTE_SUCCESS != rc) {
                PRTE_RELEASE(jdata);
                if (NULL != cd->toolcbfunc) {
                    PRTE_PMIX_CONVERT_NAME(rc, &pname, &cd->target);
                    cd->toolcbfunc(PMIX_ERROR, &pname, cd->cbdata);
                }
                PRTE_RELEASE(cd);
                return;
            }
            cd->target.jobid = jdata->jobid;
            cd->target.vpid = 0;
            prte_pmix_server_tool_conn_complete(jdata, cd);
        } else {
            if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, cd, &cd->room_num))) {
                prte_show_help("help-prted.txt", "noroom", true, cd->operation, prte_pmix_server_globals.num_rooms);
                goto callback;
            }
            /* we need to send this to the HNP for a jobid */
            buf = PRTE_NEW(prte_buffer_t);
            prte_dss.pack(buf, &command, 1, PRTE_PLM_CMD);
            prte_dss.pack(buf, &cd->room_num, 1, PRTE_INT);
            /* send it to the HNP for processing - might be myself! */
            if (PRTE_SUCCESS != (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf,
                                                              PRTE_RML_TAG_PLM,
                                                              prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(rc);
                xrc = prte_pmix_convert_rc(rc);
                prte_hotel_checkout_and_return_occupant(&prte_pmix_server_globals.reqs, cd->room_num, (void**)&cd);
                PRTE_RELEASE(buf);
                if (NULL != cd->toolcbfunc) {
                    cd->toolcbfunc(xrc, NULL, cd->cbdata);
                }
                PRTE_RELEASE(cd);
            }
            return;
        }
    } else {
        /* we may have spawned this job, so check to see if we
         * already have a job object for it */
        jdata = prte_get_job_data_object(cd->target.jobid);
        if (NULL == jdata) {
            jdata = PRTE_NEW(prte_job_t);
            jdata->jobid = cd->target.jobid;
            PRTE_PMIX_CONVERT_JOBID(rc, jdata->nspace, jdata->jobid);
            prte_pmix_server_tool_conn_complete(jdata, cd);
        } else {
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);
        }
    }
    rc = PRTE_SUCCESS;

  callback:
    if (NULL != cd->toolcbfunc) {
        PRTE_PMIX_CONVERT_NAME(rc, &pname, &cd->target);
        xrc = prte_pmix_convert_rc(rc);
        cd->toolcbfunc(xrc, &pname, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

void prte_pmix_server_tool_conn_complete(prte_job_t *jdata,
                                         pmix_server_req_t *req)
{
    prte_app_context_t *app;
    prte_proc_t *proc;
    prte_node_t *node, *nptr;
    int i;

    /* flag that this job is a tool */
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);
    if (req->launcher) {
        /* flag that it is also a launcher */
        PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_LAUNCHER);
    }
    /* flag that it is not to be monitored */
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_DO_NOT_MONITOR);
    /* store it away */
    prte_set_job_data_object(jdata->jobid, jdata);

    /* must create a map for it (even though it has no
     * info in it) so that the job info will be picked
     * up in subsequent pidmaps or other daemons won't
     * know how to route
     */
    jdata->map = PRTE_NEW(prte_job_map_t);

    /* setup an app_context for the singleton */
    app = PRTE_NEW(prte_app_context_t);
    if (NULL == req->cmdline) {
        app->app = strdup("tool");
        prte_argv_append_nosize(&app->argv, "tool");
    } else {
        app->argv = prte_argv_split(req->cmdline, ' ');
        app->app = strdup(app->argv[0]);
    }
    app->num_procs = 1;
    prte_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;

    /* setup a proc object for the singleton - since we
     * -must- be the HNP, and therefore we stored our
     * node on the global node pool, and since the singleton
     * -must- be on the same node as us, indicate that
     */
    proc = PRTE_NEW(prte_proc_t);
    proc->name.jobid = jdata->jobid;
    proc->name.vpid = req->target.vpid;
    proc->pid = req->pid;
    proc->parent = PRTE_PROC_MY_NAME->vpid;
    PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_ALIVE);
    PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_TOOL);
    proc->state = PRTE_PROC_STATE_RUNNING;
    /* set the trivial */
    proc->local_rank = 0;
    proc->node_rank = 0;
    proc->app_rank = 0;
    if (NULL == req->operation) {
        /* it is on my node */
        node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0);
        PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_LOCAL);
    } else {
        /* we need to locate it */
        node = NULL;
        for (i=0; i < prte_node_pool->size; i++) {
            if (NULL == (nptr = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            if (0 == strcmp(req->operation, nptr->name)) {
                node = nptr;
                break;
            }
        }
        if (NULL == node) {
            /* not in our allocation - which is still okay */
            node = PRTE_NEW(prte_node_t);
            node->name = strdup(req->operation);
            PRTE_FLAG_SET(node, PRTE_NODE_NON_USABLE);
            prte_pointer_array_add(prte_node_pool, node);
        }
    }
    if (NULL == node) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
    } else {
        proc->node = node;
        PRTE_RETAIN(node);  /* keep accounting straight */
        /* add the node to the job map */
        PRTE_RETAIN(node);
        prte_pointer_array_add(jdata->map->nodes, node);
        jdata->map->num_nodes++;
        /* and it obviously is on the node - note that
         * we do _not_ increment the #procs on the node
         * as the tool doesn't count against the slot
         * allocation */
        PRTE_RETAIN(proc);
        prte_pointer_array_add(node->procs, proc);
    }
    prte_pointer_array_add(jdata->procs, proc);
    jdata->num_procs = 1;
    /* if they indicated a preference for termination, set it */
    if (req->flag) {
        prte_set_attribute(&jdata->attributes, PRTE_JOB_SILENT_TERMINATION,
                           PRTE_ATTR_GLOBAL, NULL, PRTE_BOOL);
    }
}

void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                            pmix_tool_connection_cbfunc_t cbfunc,
                            void *cbdata)
{
    pmix_server_req_t *cd;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION REQUEST RECVD",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PRTE_NEW(pmix_server_req_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;

    prte_event_set(prte_event_base, &(cd->ev), -1,
                   PRTE_EV_WRITE, _toolconn, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

}

static void lgcbfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

void pmix_server_log_fn(const pmix_proc_t *client,
                        const pmix_info_t data[], size_t ndata,
                        const pmix_info_t directives[], size_t ndirs,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n, cnt;
    prte_buffer_t *buf;
    prte_byte_object_t bo, *boptr;
    int rc = PRTE_SUCCESS;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t pbo;
    pmix_status_t ret;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s logging info",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* if we are the one that passed it down, then we don't pass it back */
    for (n=0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], "prte.log.noloop")) {
            if (PMIX_INFO_TRUE(&directives[n])) {
                rc = PMIX_SUCCESS;
                goto done;
            }
        }
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    cnt = 0;

    for (n=0; n < ndata; n++) {
        if (0 == strncmp(data[n].key, PRTE_PMIX_SHOW_HELP, PMIX_MAX_KEYLEN)) {
            /* pull out the blob */
            if (PMIX_BYTE_OBJECT != data[n].value.type) {
                continue;
            }
            buf = PRTE_NEW(prte_buffer_t);
            prte_dss.load(buf, data[n].value.data.bo.bytes, data[n].value.data.bo.size);
            if (PRTE_SUCCESS != (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf,
                                                              PRTE_RML_TAG_SHOW_HELP,
                                                              prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(rc);
                buf->base_ptr = NULL;
                PRTE_RELEASE(buf);
            }
        } else {
            /* ship this to our HNP/MASTER for processing, even if that is us */
            ret = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, (pmix_info_t*)&data[n], 1, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            }
            ++cnt;
        }
    }
    if (0 < cnt) {
        buf = PRTE_NEW(prte_buffer_t);
        prte_dss.pack(buf, &cnt, 1, PRTE_SIZE);
        PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;
        boptr = &bo;
        prte_dss.pack(buf, &boptr, 1, PRTE_BYTE_OBJECT);
        free(bo.bytes);
        rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf,
                                     PRTE_RML_TAG_LOGGING,
                                     prte_rml_send_callback, NULL);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PRTE_RELEASE(buf);
        }
    }

  done:
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    PRTE_PMIX_THREADSHIFT(PRTE_NAME_WILDCARD, NULL, rc,
                          NULL, NULL, 0, lgcbfn,
                          cbfunc, cbdata);
}

pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor,
                                      const pmix_proc_t targets[], size_t ntargets,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int rc, j;
    int32_t signum;
    size_t m, n;
    prte_proc_t *proc;
    prte_jobid_t jobid;
    prte_pointer_array_t parray, *ptrarray;
    prte_process_name_t name;
    prte_buffer_t *cmd;
    prte_daemon_cmd_flag_t cmmnd;
    prte_grpcomm_signature_t *sig;
    pmix_proc_t *proct;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m=0; m < ndirs; m++) {
        if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_KILL, PMIX_MAX_KEYLEN)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                PRTE_CONSTRUCT(&parray, prte_pointer_array_t);
                for (n=0; n < ntargets; n++) {
                    PRTE_PMIX_CONVERT_PROCT(rc, &name, (pmix_proc_t*)&targets[n]);
                    if (PRTE_SUCCESS != rc) {
                        PRTE_ERROR_LOG(rc);
                        return PMIX_ERR_BAD_PARAM;
                    }
                    if (PRTE_VPID_WILDCARD == name.vpid) {
                        /* create an object */
                        proc = PRTE_NEW(prte_proc_t);
                        proc->name.jobid = name.jobid;
                        proc->name.vpid = PRTE_VPID_WILDCARD;
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = prte_get_proc_object(&name))) {
                            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                            continue;
                        }
                        PRTE_RETAIN(proc);
                    }
                    prte_pointer_array_add(&parray, proc);
                }
                ptrarray = &parray;
            }
            if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(ptrarray))) {
                PRTE_ERROR_LOG(rc);
            }
            if (NULL != ptrarray) {
                /* cleanup the array */
                for (j=0; j < parray.size; j++) {
                    if (NULL != (proc = (prte_proc_t*)prte_pointer_array_get_item(&parray, j))) {
                        PRTE_RELEASE(proc);
                    }
                }
                PRTE_DESTRUCT(&parray);
            }
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_TERMINATE, PMIX_MAX_KEYLEN)) {
            if (NULL == targets) {
                /* terminate the daemons and all running jobs */
                cmd = PRTE_NEW(prte_buffer_t);
                /* pack the command */
                cmmnd = PRTE_DAEMON_HALT_VM_CMD;
                if (PRTE_SUCCESS != (rc = prte_dss.pack(cmd, &cmmnd, 1, PRTE_DAEMON_CMD))) {
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(cmd);
                    return rc;
                }
                /* goes to all daemons */
                sig = PRTE_NEW(prte_grpcomm_signature_t);
                sig->signature = (prte_process_name_t*)malloc(sizeof(prte_process_name_t));
                sig->signature[0].jobid = PRTE_PROC_MY_NAME->jobid;
                sig->signature[0].vpid = PRTE_VPID_WILDCARD;
                if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                    PRTE_ERROR_LOG(rc);
                }
                PRTE_RELEASE(cmd);
                PRTE_RELEASE(sig);
            }
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_SIGNAL, PMIX_MAX_KEYLEN)) {
                cmd = PRTE_NEW(prte_buffer_t);
                cmmnd = PRTE_DAEMON_SIGNAL_LOCAL_PROCS;
                /* pack the command */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(cmd, &cmmnd, 1, PRTE_DAEMON_CMD))) {
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(cmd);
                    return rc;
                }
                /* pack the target jobid */
                if (NULL == targets) {
                    jobid = PRTE_JOBID_WILDCARD;
                } else {
                    proct = (pmix_proc_t*)&targets[0];
                    PRTE_PMIX_CONVERT_NSPACE(rc, &jobid, proct->nspace);
                    if (PRTE_SUCCESS != rc) {
                        PRTE_RELEASE(cmd);
                        return PMIX_ERR_BAD_PARAM;
                    }
                }
                if (PRTE_SUCCESS != (rc = prte_dss.pack(cmd, &jobid, 1, PRTE_JOBID))) {
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(cmd);
                    return rc;
                }
                /* pack the signal */
                PMIX_VALUE_GET_NUMBER(rc, &directives[m].value, signum, int32_t);
                if (PMIX_SUCCESS != rc) {
                    PRTE_RELEASE(cmd);
                    return rc;
                }
                if (PRTE_SUCCESS != (rc = prte_dss.pack(cmd, &signum, 1, PRTE_INT32))) {
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(cmd);
                    return rc;
                }
                /* goes to all daemons */
                sig = PRTE_NEW(prte_grpcomm_signature_t);
                sig->signature = (prte_process_name_t*)malloc(sizeof(prte_process_name_t));
                sig->signature[0].jobid = PRTE_PROC_MY_NAME->jobid;
                sig->signature[0].vpid = PRTE_VPID_WILDCARD;
                if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                    PRTE_ERROR_LOG(rc);
                }
                PRTE_RELEASE(cmd);
                PRTE_RELEASE(sig);
        }
    }

    return PMIX_OPERATION_SUCCEEDED;
}

#if PMIX_NUMERIC_VERSION >= 0x00040000
static void relcb(void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd=(prte_pmix_mdx_caddy_t*)cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRTE_RELEASE(cd);
}
static void group_release(int status, prte_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t*)cbdata;
    int32_t cnt;
    int rc=PRTE_SUCCESS;
    pmix_status_t ret;
    size_t cid, n;
    pmix_byte_object_t bo;
    int32_t byused;

    PRTE_ACQUIRE_OBJECT(cd);

    if (PRTE_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    if (1 == cd->mode) {
        /* a context id was requested, get it */
        cnt = 1;
        rc = prte_dss.unpack(buf, &cid, &cnt, PRTE_SIZE);
        /* error if they didn't return it */
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto complete;
        }
        cd->ninfo++;
    }
    /* if anything is left in the buffer, then it is
     * modex data that needs to be stored */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    byused = buf->bytes_used - (buf->unpack_ptr - buf->base_ptr);
    if (0 < byused) {
        bo.bytes = buf->unpack_ptr;
        bo.size = byused;
    }
    if (NULL != bo.bytes && 0 < bo.size) {
        cd->ninfo++;
    }

    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        n = 0;
        if (1 == cd->mode) {
            PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
            ++n;
        }
        if (NULL != bo.bytes && 0 < bo.size) {
            PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_ENDPT_DATA, &bo, PMIX_BYTE_OBJECT);
        }
    }

  complete:
    ret = prte_pmix_convert_rc(rc);
    /* return to the local procs in the collective */
    if (NULL != cd->infocbfunc) {
        cd->infocbfunc(ret, cd->info, cd->ninfo, cd->cbdata, relcb, cd);
    } else {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PRTE_RELEASE(cd);
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *gpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i, mode = 0;
    pmix_server_pset_t *pset;
    bool fence = false;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    prte_buffer_t bf;
    pmix_byte_object_t *bo = NULL;
#endif

    /* they are required to pass us an id */
    if (NULL == gpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check the directives */
    for (i=0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            if (PMIX_INFO_TRUE(&directives[i])) {
                mode = 1;
            }
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_EMBED_BARRIER)) {
            fence = PMIX_INFO_TRUE(&directives[i]);
#if PMIX_NUMERIC_VERSION >= 0x00040000
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ENDPT_DATA)) {
            bo = (pmix_byte_object_t*)&directives[i].value.data.bo;
#endif
        }
    }

    if (PMIX_GROUP_CONSTRUCT == op) {
        /* add it to our list of known process sets */
        pset = PRTE_NEW(pmix_server_pset_t);
        pset->name = strdup(gpid);
        pset->num_members = nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, procs, nprocs * sizeof(pmix_proc_t));
        prte_list_append(&prte_pmix_server_globals.psets, &pset->super);
    } else if (PMIX_GROUP_DESTRUCT == op) {
        /* find this process set on our list of groups */
        PRTE_LIST_FOREACH(pset, &prte_pmix_server_globals.psets, pmix_server_pset_t) {
            if (0 == strcmp(pset->name, gpid)) {
                prte_list_remove_item(&prte_pmix_server_globals.psets, &pset->super);
                PRTE_RELEASE(pset);
                break;
            }
        }
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned, then we are done */
    if (!fence && 0 == mode) {
        return PMIX_OPERATION_SUCCEEDED;
    }

    cd = PRTE_NEW(prte_pmix_mdx_caddy_t);
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->mode = mode;

   /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = PRTE_NEW(prte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (prte_process_name_t*)malloc(cd->sig->sz * sizeof(prte_process_name_t));
        memset(cd->sig->signature, 0, cd->sig->sz * sizeof(prte_process_name_t));
        for (i=0; i < nprocs; i++) {
            PRTE_PMIX_CONVERT_PROCT(rc, &cd->sig->signature[i], (pmix_proc_t*)&procs[i]);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(cd);
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }
    cd->buf = PRTE_NEW(prte_buffer_t);
#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* if they provided us with a data blob, send it along */
    if (NULL != bo) {
        /* We don't own the byte_object and so we have to
         * copy it here */
        PRTE_CONSTRUCT(&bf, prte_buffer_t);
        prte_dss.load(&bf, bo->bytes, bo->size);
        prte_dss.copy_payload(cd->buf, &bf);
        /* don't destruct bf! */
    }
#endif
    /* pass it to the global collective algorithm */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd->sig, cd->buf, mode,
                                                     group_release, cd))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}
#endif

pmix_status_t pmix_server_iof_pull_fn(const pmix_proc_t procs[], size_t nprocs,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_iof_channel_t channels,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    return PMIX_ERR_NOT_SUPPORTED;
}

static void pmix_server_stdin_push(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;
    prte_process_name_t dst;
    pmix_byte_object_t *bo = (pmix_byte_object_t*)cd->server_object;
    size_t n;
    int rc;


    for (n=0; n < cd->nprocs; n++) {
        PRTE_PMIX_CONVERT_PROCT(rc, &dst, &cd->procs[n]);

        PRTE_OUTPUT_VERBOSE((1, prte_debug_output,
                              "%s pmix_server_stdin_push to dest %s: size %zu",
                              PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                              PRTE_NAME_PRINT(&dst),
                              bo->size));
        prte_iof.push_stdin(&dst, (uint8_t*)bo->bytes, bo->size);
    }

#if PMIX_NUMERIC_VERSION >= 0x00040000
    if (NULL == bo->bytes || 0 == bo->size) {
        cd->cbfunc(PMIX_ERR_IOF_COMPLETE, cd->cbdata);
    }
    else {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
#else
    cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
#endif

    PMIX_BYTE_OBJECT_FREE(bo, 1);
    PMIX_PROC_FREE(cd->procs, cd->nprocs);
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_stdin_fn(const pmix_proc_t *source,
                                   const pmix_proc_t targets[], size_t ntargets,
                                   const pmix_info_t directives[], size_t ndirs,
                                   const pmix_byte_object_t *bo,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_byte_object_t *bo_cpy = NULL;
    pmix_proc_t *targets_cpy = NULL;
    size_t n;

    // We need to copy the object and the targets since we are shifting them
    // so they would go out of scope after we return from this function.
    PMIX_BYTE_OBJECT_CREATE(bo_cpy, 1);
    bo_cpy->bytes = pmix_malloc(bo->size * sizeof(char));
    if (NULL == bo_cpy->bytes) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
    }
    memcpy(bo_cpy->bytes, bo->bytes, bo->size);
    bo_cpy->size = bo->size;

    PMIX_PROC_CREATE(targets_cpy, ntargets);
    for( n = 0; n < ntargets; ++n ) {
        PMIX_PROC_LOAD(&targets_cpy[n], targets[n].nspace, targets[n].rank);
    }

    // Note: We are ignoring the directives / ndirs at the moment
    PRTE_IO_OP(targets_cpy, ntargets, bo_cpy, pmix_server_stdin_push, cbfunc, cbdata);

    // Do not send PMIX_OPERATION_SUCCEEDED since the op hasn't completed yet.
    // We will send it back when we are done by calling the cbfunc.
    return PMIX_SUCCESS;
}
