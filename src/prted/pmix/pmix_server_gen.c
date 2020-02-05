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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
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

static void pmix_server_stdin_push(int sd, short args, void *cbdata);

static void _client_conn(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_proc_t *p, *ptr;
    int i;

    PRRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prrte_proc_t */
        p = (prrte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prrte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, i))) {
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
        PRRTE_FLAG_SET(p, PRRTE_PROC_FLAG_REG);
        PRRTE_ACTIVATE_PROC_STATE(&p->name, PRRTE_PROC_STATE_REGISTERED);
    }
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

pmix_status_t pmix_server_client_connected_fn(const pmix_proc_t *proc, void* server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_process_name_t name;
    int rc;

    PRRTE_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (PRRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRRTE_PMIX_THREADSHIFT(&name, server_object, PRRTE_SUCCESS, NULL,
                          NULL, 0, _client_conn, cbfunc, cbdata);
    return PRRTE_SUCCESS;
}

static void _client_finalized(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_proc_t *p, *ptr;
    int i;

    PRRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prrte_proc_t */
        p = (prrte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prrte_get_job_data_object(cd->proc.jobid))) {
            /* this tool was not started by us and we have
             * no job record for it - this shouldn't happen,
             * so let's error log it */
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            /* ensure they don't hang */
            goto release;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, i))) {
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
        PRRTE_FLAG_SET(p, PRRTE_PROC_FLAG_IOF_COMPLETE);
        PRRTE_FLAG_SET(p, PRRTE_PROC_FLAG_WAITPID);
        PRRTE_ACTIVATE_PROC_STATE(&cd->proc, PRRTE_PROC_STATE_TERMINATED);
    }
    if (NULL != p) {
        PRRTE_FLAG_SET(p, PRRTE_PROC_FLAG_HAS_DEREG);
    }

  release:
    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void* server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_process_name_t name;
    int rc;

    PRRTE_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (PRRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRRTE_PMIX_THREADSHIFT(&name, server_object, PRRTE_SUCCESS, NULL,
                          NULL, 0, _client_finalized, cbfunc, cbdata);
    return PRRTE_SUCCESS;

}

static void _client_abort(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_proc_t *p, *ptr;
    int i;

    PRRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        p = (prrte_proc_t*)cd->server_object;
    } else {
        /* find the named process */
        p = NULL;
        if (NULL == (jdata = prrte_get_job_data_object(cd->proc.jobid))) {
            return;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (ptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, i))) {
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
        PRRTE_ACTIVATE_PROC_STATE(&p->name, PRRTE_PROC_STATE_CALLED_ABORT);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRRTE_SUCCESS, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object,
                                   int status, const char msg[],
                                   pmix_proc_t procs[], size_t nprocs,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_process_name_t name;
    int rc;

    PRRTE_PMIX_CONVERT_PROCT(rc, &name, proc);
    if (PRRTE_SUCCESS != rc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRRTE_PMIX_THREADSHIFT(&name, server_object, status, msg,
                          procs, nprocs, _client_abort, cbfunc, cbdata);
    return PRRTE_SUCCESS;
}

static void _register_events(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* need to implement this */

    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRRTE_SUCCESS, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

/* hook for the local PMIX server to pass event registrations
 * up to us - we will assume the responsibility for providing
 * notifications for registered events */
pmix_status_t pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->info = (pmix_info_t*)info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prrte_event_set(prrte_event_base, &(cd->ev), -1,
                   PRRTE_EV_WRITE, _register_events, cd);
    prrte_event_set_priority(&(cd->ev), PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(cd);
    prrte_event_active(&(cd->ev), PRRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void _deregister_events(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* need to implement this */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRRTE_SUCCESS, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}
/* hook for the local PMIX server to pass event deregistrations
 * up to us */
pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes, size_t ncodes,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prrte_event_set(prrte_event_base, &(cd->ev), -1,
                   PRRTE_EV_WRITE, _deregister_events, cd);
    prrte_event_set_priority(&(cd->ev), PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(cd);
    prrte_event_active(&(cd->ev), PRRTE_EV_WRITE, 1);
    return PRRTE_SUCCESS;
}

static void _notify_release(int status, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRRTE_RELEASE(cd);
}

/* someone has sent us an event that we need to distribute
 * to our local clients */
void pmix_server_notify(int status, prrte_process_name_t* sender,
                        prrte_buffer_t *buffer,
                        prrte_rml_tag_t tg, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd;
    int cnt, rc;
    pmix_proc_t source, psender;
    prrte_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    size_t ninfo;
    prrte_jobid_t jobid;
    prrte_job_t *jdata;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s PRRTE Notification received from %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(sender));

    /* unpack the byte object payload */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    free(boptr);

    /* convert the sender */
    PRRTE_PMIX_CONVERT_NAME(&psender, sender);

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

    cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);

    /* unpack the #infos that were provided */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &cd->ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        PRRTE_RELEASE(cd);
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
            PRRTE_RELEASE(cd);
            return;
        }
    }
    cd->ninfo = ninfo;
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

    /* protect against infinite loops by marking that this notification was
     * passed down to the server by me */
    PMIX_INFO_LOAD(&cd->info[ninfo-1], "prrte.notify.donotloop", NULL, PMIX_BOOL);

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s NOTIFYING PMIX SERVER OF STATUS %s SOURCE %s RANGE %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PMIx_Error_string(code), source.nspace, PMIx_Data_range_string(range));

    ret = PMIx_Notify_event(code, &source, range, cd->info, cd->ninfo, _notify_release, cd);
    if (PMIX_SUCCESS != ret) {
        if (PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PRRTE_RELEASE(cd);
    }

    if (PMIX_ERR_JOB_TERMINATED == code) {
        PRRTE_PMIX_CONVERT_NSPACE(rc, &jobid, source.nspace);
        jdata = prrte_get_job_data_object(jobid);
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
    }
}

pmix_status_t pmix_server_notify_event(pmix_status_t code,
                                       const pmix_proc_t *source,
                                       pmix_data_range_t range,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_buffer_t *buf;
    int rc;
    prrte_grpcomm_signature_t *sig;
    prrte_byte_object_t *boptr, bo;
    pmix_byte_object_t pbo;
    pmix_data_buffer_t pbkt;
    pmix_proc_t psender;
    pmix_status_t ret;
    size_t n;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s local process %s:%d generated event code %d range %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        source->nspace, source->rank, code, PMIx_Data_range_string(range));

    /* check to see if this is one we sent down */
    for (n=0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, "prrte.notify.donotloop")) {
            /* yep - do not process */
            goto done;
        }
    }

    /* a local process has generated an event - we need to xcast it
     * to all the daemons so it can be passed down to their local
     * procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    /* convert the sender */
    PRRTE_PMIX_CONVERT_NAME(&psender, PRRTE_PROC_MY_NAME);

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
    buf = PRRTE_NEW(prrte_buffer_t);
    boptr = &bo;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &boptr, 1, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        free(bo.bytes);
        PRRTE_RELEASE(buf);
        return PMIX_ERR_PACK_FAILURE;
    }
    free(bo.bytes);

    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    if (NULL == sig) {
        PRRTE_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    if (NULL == sig->signature) {
        PRRTE_RELEASE(buf);
        PRRTE_RELEASE(sig);
        return PMIX_ERR_NOMEM;
    }
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    sig->sz = 1;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_NOTIFICATION, buf))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        PRRTE_RELEASE(sig);
        return PMIX_ERROR;
    }
    PRRTE_RELEASE(buf);
    /* maintain accounting */
    PRRTE_RELEASE(sig);

  done:
    /* we do not need to execute a callback as we did this atomically */
    return PMIX_OPERATION_SUCCEEDED;
}

static void _toolconn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *cd = (pmix_server_req_t*)cbdata;
    prrte_job_t *jdata = NULL, *jptr;
    int rc, i;
    uint32_t u32;
    size_t n;
    pmix_proc_t pname;
    prrte_buffer_t *buf;
    prrte_plm_cmd_flag_t command = PRRTE_PLM_ALLOC_JOBID_CMD;
    pmix_status_t xrc;
    void *nptr;

    PRRTE_ACQUIRE_OBJECT(cd);

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s TOOL CONNECTION PROCESSING",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

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
                    PRRTE_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GRPID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->gid, gid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PRRTE_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_NSPACE)) {
                 PRRTE_PMIX_CONVERT_NSPACE(rc, &cd->target.jobid, cd->info[n].value.data.string);
                 if (PRRTE_SUCCESS != rc) {
                    PRRTE_ERROR_LOG(rc);
                 }
             } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_RANK)) {
                PRRTE_PMIX_CONVERT_RANK(cd->target.vpid, cd->info[n].value.data.rank);
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
                    PRRTE_RELEASE(cd);
                    return;
                }
            }
        }
    }

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s TOOL CONNECTION FROM UID %d GID %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), cd->uid, cd->gid);

    /* if we are not the HNP or master, and the tool doesn't
     * already have a name (i.e., we didn't spawn it), then
     * there is nothing we can currently do.
     * Eventually, when we switch to nspace instead of an
     * integer jobid, we'll just locally assign this value */
    if (PRRTE_JOBID_INVALID == cd->target.jobid ||
        PRRTE_VPID_INVALID == cd->target.vpid) {
       /* if we are the HNP, we can directly assign the jobid */
        if (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_MASTER) {
            jdata = PRRTE_NEW(prrte_job_t);
            rc = prrte_plm_base_create_jobid(jdata);
            if (PRRTE_SUCCESS != rc) {
                PRRTE_RELEASE(jdata);
                if (NULL != cd->toolcbfunc) {
                    PRRTE_PMIX_CONVERT_NAME(&pname, &cd->target);
                    cd->toolcbfunc(PMIX_ERROR, &pname, cd->cbdata);
                }
                PRRTE_RELEASE(cd);
                return;
            }
            cd->target.jobid = jdata->jobid;
            cd->target.vpid = 0;
            prrte_pmix_server_tool_conn_complete(jdata, cd);
        } else {
            if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, cd, &cd->room_num))) {
                prrte_show_help("help-orted.txt", "noroom", true, cd->operation, prrte_pmix_server_globals.num_rooms);
                goto callback;
            }
            /* we need to send this to the HNP for a jobid */
            buf = PRRTE_NEW(prrte_buffer_t);
            prrte_dss.pack(buf, &command, 1, PRRTE_PLM_CMD);
            prrte_dss.pack(buf, &cd->room_num, 1, PRRTE_INT);
            /* send it to the HNP for processing - might be myself! */
            if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                                              PRRTE_RML_TAG_PLM,
                                                              prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
                xrc = prrte_pmix_convert_rc(rc);
                prrte_hotel_checkout_and_return_occupant(&prrte_pmix_server_globals.reqs, cd->room_num, (void**)&cd);
                PRRTE_RELEASE(buf);
                if (NULL != cd->toolcbfunc) {
                    cd->toolcbfunc(xrc, NULL, cd->cbdata);
                }
                PRRTE_RELEASE(cd);
            }
            return;
        }
    } else {
        /* we may have spawned this job, so check to see if we
         * already have a job object for it */
        jdata = NULL;
        i = prrte_hash_table_get_first_key_uint32(prrte_job_data, &u32, (void **)&jptr, &nptr);
        while (PRRTE_SUCCESS == i) {
            if (cd->target.jobid == jptr->jobid) {
                jdata = jptr;
                /* flag that this job is a tool */
                PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_TOOL);
                break;
            }
            i = prrte_hash_table_get_next_key_uint32(prrte_job_data, &u32, (void **)&jptr, nptr, &nptr);
        }
        if (NULL == jdata) {
            jdata = PRRTE_NEW(prrte_job_t);
            jdata->jobid = cd->target.jobid;
            prrte_pmix_server_tool_conn_complete(jdata, cd);
        }
    }
    rc = PRRTE_SUCCESS;

  callback:
    if (NULL != cd->toolcbfunc) {
        PRRTE_PMIX_CONVERT_NAME(&pname, &cd->target);
        xrc = prrte_pmix_convert_rc(rc);
        cd->toolcbfunc(xrc, &pname, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

void prrte_pmix_server_tool_conn_complete(prrte_job_t *jdata,
                                         pmix_server_req_t *req)
{
    prrte_app_context_t *app;
    prrte_proc_t *proc;
    prrte_node_t *node, *nptr;
    int i;

    /* flag that this job is a tool */
    PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_TOOL);
    if (req->launcher) {
        /* flag that it is also a launcher */
        PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_LAUNCHER);
    }
    /* store it away */
    prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, jdata);

    /* must create a map for it (even though it has no
     * info in it) so that the job info will be picked
     * up in subsequent pidmaps or other daemons won't
     * know how to route
     */
    jdata->map = PRRTE_NEW(prrte_job_map_t);

    /* setup an app_context for the singleton */
    app = PRRTE_NEW(prrte_app_context_t);
    if (NULL == req->cmdline) {
        app->app = strdup("tool");
        prrte_argv_append_nosize(&app->argv, "tool");
    } else {
        app->argv = prrte_argv_split(req->cmdline, ' ');
        app->app = strdup(app->argv[0]);
    }
    app->num_procs = 1;
    prrte_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;

    /* setup a proc object for the singleton - since we
     * -must- be the HNP, and therefore we stored our
     * node on the global node pool, and since the singleton
     * -must- be on the same node as us, indicate that
     */
    proc = PRRTE_NEW(prrte_proc_t);
    proc->name.jobid = jdata->jobid;
    proc->name.vpid = req->target.vpid;
    proc->pid = req->pid;
    proc->parent = PRRTE_PROC_MY_NAME->vpid;
    PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_ALIVE);
    PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_TOOL);
    proc->state = PRRTE_PROC_STATE_RUNNING;
    /* set the trivial */
    proc->local_rank = 0;
    proc->node_rank = 0;
    proc->app_rank = 0;
    proc->app_idx = 0;
    if (NULL == req->operation) {
        /* it is on my node */
        node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);
        PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_LOCAL);
    } else {
        /* we need to locate it */
        node = NULL;
        for (i=0; i < prrte_node_pool->size; i++) {
            if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                continue;
            }
            if (0 == strcmp(req->operation, nptr->name)) {
                node = nptr;
                break;
            }
        }
        if (NULL == node) {
            /* not in our allocation - which is still okay */
            node = PRRTE_NEW(prrte_node_t);
            node->name = strdup(req->operation);
            PRRTE_FLAG_SET(node, PRRTE_NODE_NON_USABLE);
            prrte_pointer_array_add(prrte_node_pool, node);
        }
    }
    proc->node = node;
    PRRTE_RETAIN(node);  /* keep accounting straight */
    prrte_pointer_array_add(jdata->procs, proc);
    jdata->num_procs = 1;
    /* add the node to the job map */
    PRRTE_RETAIN(node);
    prrte_pointer_array_add(jdata->map->nodes, node);
    jdata->map->num_nodes++;
    /* and it obviously is on the node - note that
     * we do _not_ increment the #procs on the node
     * as the tool doesn't count against the slot
     * allocation */
    PRRTE_RETAIN(proc);
    prrte_pointer_array_add(node->procs, proc);
    /* if they indicated a preference for termination, set it */
    if (req->flag) {
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_SILENT_TERMINATION,
                           PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);
    }
}

void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                            pmix_tool_connection_cbfunc_t cbfunc,
                            void *cbdata)
{
    pmix_server_req_t *cd;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s TOOL CONNECTION REQUEST RECVD",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PRRTE_NEW(pmix_server_req_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;

    prrte_event_set(prrte_event_base, &(cd->ev), -1,
                   PRRTE_EV_WRITE, _toolconn, cd);
    prrte_event_set_priority(&(cd->ev), PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(cd);
    prrte_event_active(&(cd->ev), PRRTE_EV_WRITE, 1);

}

static void lgcbfn(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

void pmix_server_log_fn(const pmix_proc_t *client,
                        const pmix_info_t data[], size_t ndata,
                        const pmix_info_t directives[], size_t ndirs,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n, cnt;
    prrte_buffer_t *buf;
    prrte_byte_object_t bo, *boptr;
    int rc = PRRTE_SUCCESS;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t pbo;
    pmix_proc_t psender;
    pmix_status_t ret;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s logging info",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if we are the one that passed it down, then we don't pass it back */
    for (n=0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], "prrte.log.noloop")) {
            if (PMIX_INFO_TRUE(&directives[n])) {
                rc = PMIX_SUCCESS;
                goto done;
            }
        }
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    PRRTE_PMIX_CONVERT_NAME(&psender, PRRTE_PROC_MY_NAME);
    cnt = 0;

    for (n=0; n < ndata; n++) {
        if (0 == strncmp(data[n].key, PRRTE_PMIX_SHOW_HELP, PMIX_MAX_KEYLEN)) {
            /* pull out the blob */
            if (PMIX_BYTE_OBJECT != data[n].value.type) {
                continue;
            }
            buf = PRRTE_NEW(prrte_buffer_t);
            prrte_dss.load(buf, data[n].value.data.bo.bytes, data[n].value.data.bo.size);
            if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                                              PRRTE_RML_TAG_SHOW_HELP,
                                                              prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
                buf->base_ptr = NULL;
                PRRTE_RELEASE(buf);
            }
        } else {
            /* ship this to our HNP/MASTER for processing, even if that is us */
            ret = PMIx_Data_pack(&psender, &pbuf, (pmix_info_t*)&data[n], 1, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            }
            ++cnt;
        }
    }
    if (0 < cnt) {
        buf = PRRTE_NEW(prrte_buffer_t);
        prrte_dss.pack(buf, &cnt, 1, PRRTE_SIZE);
        PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;
        boptr = &bo;
        prrte_dss.pack(buf, &boptr, 1, PRRTE_BYTE_OBJECT);
        free(bo.bytes);
        rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                     PRRTE_RML_TAG_LOGGING,
                                     prrte_rml_send_callback, NULL);
        if (PRRTE_SUCCESS != rc) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(buf);
        }
    }

  done:
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    PRRTE_PMIX_THREADSHIFT(PRRTE_NAME_WILDCARD, NULL, rc,
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
    prrte_proc_t *proc;
    prrte_pointer_array_t parray, *ptrarray;
    prrte_process_name_t name;
    prrte_buffer_t *cmd;
    prrte_daemon_cmd_flag_t cmmnd = PRRTE_DAEMON_HALT_VM_CMD;
    prrte_grpcomm_signature_t *sig;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m=0; m < ndirs; m++) {
        if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_KILL, PMIX_MAX_KEYLEN)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                PRRTE_CONSTRUCT(&parray, prrte_pointer_array_t);
                for (n=0; n < ntargets; n++) {
                    PRRTE_PMIX_CONVERT_PROCT(rc, &name, &targets[n]);
                    if (PRRTE_SUCCESS != rc) {
                        PRRTE_ERROR_LOG(rc);
                        return PMIX_ERR_BAD_PARAM;
                    }
                    if (PRRTE_VPID_WILDCARD == name.vpid) {
                        /* create an object */
                        proc = PRRTE_NEW(prrte_proc_t);
                        proc->name.jobid = name.jobid;
                        proc->name.vpid = PRRTE_VPID_WILDCARD;
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = prrte_get_proc_object(&name))) {
                            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                            continue;
                        }
                        PRRTE_RETAIN(proc);
                    }
                    prrte_pointer_array_add(&parray, proc);
                }
                ptrarray = &parray;
            }
            if (PRRTE_SUCCESS != (rc = prrte_plm.terminate_procs(ptrarray))) {
                PRRTE_ERROR_LOG(rc);
            }
            if (NULL != ptrarray) {
                /* cleanup the array */
                for (j=0; j < parray.size; j++) {
                    if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(&parray, j))) {
                        PRRTE_RELEASE(proc);
                    }
                }
                PRRTE_DESTRUCT(&parray);
            }
            continue;
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_TERMINATE, PMIX_MAX_KEYLEN)) {
            if (NULL == targets) {
                /* terminate the daemons and all running jobs */
                cmd = PRRTE_NEW(prrte_buffer_t);
                /* pack the command */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &cmmnd, 1, PRRTE_DAEMON_CMD))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(cmd);
                    return rc;
                }
                /* goes to all daemons */
                sig = PRRTE_NEW(prrte_grpcomm_signature_t);
                sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
                sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
                sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
                if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, cmd))) {
                    PRRTE_ERROR_LOG(rc);
                }
                PRRTE_RELEASE(cmd);
                PRRTE_RELEASE(sig);
            }
        }
    }

    return PMIX_OPERATION_SUCCEEDED;
}

#if PMIX_NUMERIC_VERSION >= 0x00040000
static void relcb(void *cbdata)
{
    prrte_pmix_mdx_caddy_t *cd=(prrte_pmix_mdx_caddy_t*)cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRRTE_RELEASE(cd);
}
static void group_release(int status, prrte_buffer_t *buf, void *cbdata)
{
    prrte_pmix_mdx_caddy_t *cd = (prrte_pmix_mdx_caddy_t*)cbdata;
    int32_t cnt;
    int rc=PRRTE_SUCCESS;
    pmix_status_t ret;
    size_t cid, n;
    pmix_byte_object_t bo;
    int32_t byused;

    PRRTE_ACQUIRE_OBJECT(cd);

    if (PRRTE_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    if (1 == cd->mode) {
        /* a context id was requested, get it */
        cnt = 1;
        rc = prrte_dss.unpack(buf, &cid, &cnt, PRRTE_SIZE);
        /* error if they didn't return it */
        if (PRRTE_SUCCESS != rc) {
            PRRTE_ERROR_LOG(rc);
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
    ret = prrte_pmix_convert_rc(rc);
    /* return to the local procs in the collective */
    if (NULL != cd->infocbfunc) {
        cd->infocbfunc(ret, cd->info, cd->ninfo, cd->cbdata, relcb, cd);
    } else {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PRRTE_RELEASE(cd);
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *gpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i, mode = 0;
    pmix_server_pset_t *pset;
    bool fence = false;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    prrte_buffer_t bf;
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
        pset = PRRTE_NEW(pmix_server_pset_t);
        pset->name = strdup(gpid);
        pset->num_members = nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, procs, nprocs * sizeof(pmix_proc_t));
        prrte_list_append(&prrte_pmix_server_globals.psets, &pset->super);
    } else if (PMIX_GROUP_DESTRUCT == op) {
        /* find this process set on our list of groups */
        PRRTE_LIST_FOREACH(pset, &prrte_pmix_server_globals.psets, pmix_server_pset_t) {
            if (0 == strcmp(pset->name, gpid)) {
                prrte_list_remove_item(&prrte_pmix_server_globals.psets, &pset->super);
                PRRTE_RELEASE(pset);
                break;
            }
        }
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned, then we are done */
    if (!fence && 0 == mode) {
        return PMIX_OPERATION_SUCCEEDED;
    }

    cd = PRRTE_NEW(prrte_pmix_mdx_caddy_t);
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->mode = mode;

   /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = PRRTE_NEW(prrte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (prrte_process_name_t*)malloc(cd->sig->sz * sizeof(prrte_process_name_t));
        memset(cd->sig->signature, 0, cd->sig->sz * sizeof(prrte_process_name_t));
        for (i=0; i < nprocs; i++) {
            PRRTE_PMIX_CONVERT_PROCT(rc, &cd->sig->signature[i], &procs[i]);
            if (PRRTE_SUCCESS != rc) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(cd);
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }
    cd->buf = PRRTE_NEW(prrte_buffer_t);
#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* if they provided us with a data blob, send it along */
    if (NULL != bo) {
        /* We don't own the byte_object and so we have to
         * copy it here */
        PRRTE_CONSTRUCT(&bf, prrte_buffer_t);
        prrte_dss.load(&bf, bo->bytes, bo->size);
        prrte_dss.copy_payload(cd->buf, &bf);
        /* don't destruct bf! */
    }
#endif
    /* pass it to the global collective algorithm */
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.allgather(cd->sig, cd->buf, mode,
                                                     group_release, cd))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cd);
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
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    prrte_process_name_t dst;
    pmix_byte_object_t *bo = (pmix_byte_object_t*)cd->server_object;
    size_t n;
    int rc;


    for (n=0; n < cd->nprocs; n++) {
        PRRTE_PMIX_CONVERT_PROCT(rc, &dst, &cd->procs[n]);

        PRRTE_OUTPUT_VERBOSE((1, prrte_debug_output,
                              "%s pmix_server_stdin_push to dest %s: size %zu",
                              PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                              PRRTE_NAME_PRINT(&dst),
                              bo->size));
        prrte_iof.push_stdin(&dst, (uint8_t*)bo->bytes, bo->size);
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
    PRRTE_RELEASE(cd);
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
    memcpy(bo_cpy->bytes, bo->bytes, bo->size);
    bo_cpy->size = bo->size;

    PMIX_PROC_CREATE(targets_cpy, ntargets);
    for( n = 0; n < ntargets; ++n ) {
        PMIX_PROC_LOAD(&targets_cpy[n], targets[n].nspace, targets[n].rank);
    }

    // Note: We are ignoring the directives / ndirs at the moment
    PRRTE_IO_OP(targets_cpy, ntargets, bo_cpy, pmix_server_stdin_push, cbfunc, cbdata);

    // Do not send PMIX_OPERATION_SUCCEEDED since the op hasn't completed yet.
    // We will send it back when we are done by calling the cbfunc.
    return PMIX_SUCCESS;
}
