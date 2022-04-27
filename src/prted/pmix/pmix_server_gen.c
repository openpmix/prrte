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
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void pmix_server_stdin_push(int sd, short args, void *cbdata);

static void _client_conn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t *) cd->server_object;
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_REG);
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_REGISTERED);
    }

    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_client_connected_fn(const pmix_proc_t *proc, void *server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(proc, server_object, PRTE_SUCCESS,
                          NULL, NULL, 0, _client_conn,
                          cbfunc, cbdata);
    return PRTE_SUCCESS;
}

static void _client_finalized(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t *) cd->server_object;
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_HAS_DEREG);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void *server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(proc, server_object, PRTE_SUCCESS,
                          NULL, NULL, 0, _client_finalized,
                          cbfunc, cbdata);
    return PRTE_SUCCESS;
}

static void _client_abort(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        p = (prte_proc_t *) cd->server_object;
        p->exit_code = cd->status;
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_CALLED_ABORT);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object, int status,
                                   const char msg[], pmix_proc_t procs[], size_t nprocs,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_PMIX_THREADSHIFT(proc, server_object, status, msg, procs, nprocs, _client_abort, cbfunc,
                          cbdata);
    return PRTE_SUCCESS;
}

static void _register_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

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
    cd->info = (pmix_info_t *) info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _register_events, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void _deregister_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

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
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _deregister_events, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void _notify_release(int status, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    PRTE_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRTE_RELEASE(cd);
}

/* someone has sent us an event that we need to distribute
 * to our local clients */
void pmix_server_notify(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                        prte_rml_tag_t tg, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    int cnt, rc;
    pmix_proc_t source;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    size_t ninfo;
    pmix_rank_t vpid;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s PRTE Notification received from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the daemon who broadcast the event */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &vpid, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* if I am the one who sent it, then discard it */
    if (vpid == PRTE_PROC_MY_NAME->rank) {
        return;
    }

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &code, &cnt, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* unpack the source */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &source, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* unpack the range */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &range, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);

    /* unpack the #infos that were provided */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &cd->ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
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
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, cd->info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(cd->info, cd->ninfo);
            PRTE_RELEASE(cd);
            return;
        }
    }
    cd->ninfo = ninfo;

    /* protect against infinite loops by marking that this notification was
     * passed down to the server by me */
    PMIX_INFO_LOAD(&cd->info[ninfo - 1], "prte.notify.donotloop", NULL, PMIX_BOOL);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s NOTIFYING PMIX SERVER OF STATUS %s SOURCE %s RANGE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(code), source.nspace,
                        PMIx_Data_range_string(range));

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
}

pmix_status_t pmix_server_notify_event(pmix_status_t code, const pmix_proc_t *source,
                                       pmix_data_range_t range, pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
    prte_grpcomm_signature_t *sig;
    pmix_data_buffer_t pbkt;
    pmix_status_t ret;
    size_t n;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s local process %s generated event code %s range %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(source),
                        PMIx_Error_string(code), PMIx_Data_range_string(range));

    /* we can get events prior to completing prte_init as we have
     * to init PMIx early so that PRRTE components can use it */
    PRTE_ACQUIRE_THREAD(&prte_init_lock);
    if (!prte_initialized) {
        PRTE_RELEASE_THREAD(&prte_init_lock);
        goto done;
    }
    PRTE_RELEASE_THREAD(&prte_init_lock);

    /* check to see if this is one we sent down */
    for (n = 0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, "prte.notify.donotloop")) {
            /* yep - do not process */
            goto done;
        }
    }

    /* if this is notification of procs being ready for debug, then
     * we treat this as a state change */
    if (PMIX_READY_FOR_DEBUG == code) {
        PRTE_ACTIVATE_PROC_STATE((pmix_proc_t*)source, PRTE_PROC_STATE_READY_FOR_DEBUG);
        goto done;
    }

    /* a local process has generated an event - we need to xcast it
     * to all the daemons so it can be passed down to their local
     * procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* we need to add a flag indicating this came from us as we are going to get it echoed
     * back to us by the broadcast */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &pbkt, &PRTE_PROC_MY_NAME->rank, 1, PMIX_PROC_RANK))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return rc;
    }

    /* pack the status code */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the source */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, (pmix_proc_t *) source, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    if (0 < ninfo) {
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return ret;
        }
    }

    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    if (NULL == sig) {
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return PMIX_ERR_NOMEM;
    }
    sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    if (NULL == sig->signature) {
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        PRTE_RELEASE(sig);
        return PMIX_ERR_NOMEM;
    }
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    sig->sz = 1;
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_NOTIFICATION, &pbkt))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        PRTE_RELEASE(sig);
        return PMIX_ERROR;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
    /* maintain accounting */
    PRTE_RELEASE(sig);

done:
    /* we do not need to execute a callback as we did this atomically */
    return PMIX_OPERATION_SUCCEEDED;
}

void pmix_server_jobid_return(int status, pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                              void *cbdata)
{
    pmix_server_req_t *req;
    int rc, room;
    int32_t ret, cnt;
    pmix_nspace_t jobid;
    pmix_proc_t proc;

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the jobid */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &jobid, &cnt, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack our tracking room number */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &room, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* we are hosed */
        return;
    }

    /* retrieve the request */
    req = (pmix_server_req_t*)prte_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, room);
    prte_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, room, NULL);
    if (NULL == req) {
        /* we are hosed */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        prte_output(0, "UNABLE TO RETRIEVE SPWN_REQ FOR JOB %s [room=%d]", jobid, room);
        return;
    }

    PMIX_LOAD_PROCID(&proc, jobid, 0);
    /* the tool is not a client of ours, but we can provide at least some information */
    rc = prte_pmix_server_register_tool(jobid);
    if (PRTE_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        // we can live without it
    }

    req->toolcbfunc(ret, &proc, req->cbdata);

    /* cleanup */
    PRTE_RELEASE(req);
}

static void _toolconn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *cd = (pmix_server_req_t *) cbdata;
    int rc;
    char *tmp;
    size_t n;
    pmix_data_buffer_t *buf;
    prte_plm_cmd_flag_t command = PRTE_PLM_ALLOC_JOBID_CMD;
    pmix_status_t xrc;

    PRTE_ACQUIRE_OBJECT(cd);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s PROCESSING TOOL CONNECTION",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* check for directives */
    if (NULL != cd->info) {
        for (n = 0; n < cd->ninfo; n++) {
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
                PMIX_LOAD_NSPACE(cd->target.nspace, cd->info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_RANK)) {
                cd->target.rank = cd->info[n].value.data.rank;
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_HOSTNAME)) {
                cd->operation = strdup(cd->info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CMD_LINE)) {
                cd->cmdline = strdup(cd->info[n].value.data.string);
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
                        "%s TOOL CONNECTION FROM UID %d GID %d NSPACE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        cd->uid, cd->gid, cd->target.nspace);

    /* if we are not the HNP or master, and the tool doesn't
     * already have a self-assigned name, then
     * we need to ask the master for one */
    if (PMIX_NSPACE_INVALID(cd->target.nspace) || PMIX_RANK_INVALID == cd->target.rank) {
        /* if we are the HNP, we can directly assign the jobid */
        if (PRTE_PROC_IS_MASTER) {
            /* the new nspace is our base nspace with an "@N" extension */
            prte_asprintf(&tmp, "%s@%u", prte_plm_globals.base_nspace, prte_plm_globals.next_jobid);
            PMIX_LOAD_PROCID(&cd->target, tmp, 0);
            free(tmp);
            prte_plm_globals.next_jobid++;
        } else {
            cd->room_num = prte_pointer_array_add(&prte_pmix_server_globals.local_reqs, cd);
            /* we need to send this to the HNP for a jobid */
            PMIX_DATA_BUFFER_CREATE(buf);
            rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            rc = PMIx_Data_pack(NULL, buf, &cd->room_num, 1, PMIX_INT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            /* send it to the HNP for processing - might be myself! */
            rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_PLM,
                                         prte_rml_send_callback, NULL);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                xrc = prte_pmix_convert_rc(rc);
                prte_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, cd->room_num, NULL);
                PMIX_DATA_BUFFER_RELEASE(buf);
                if (NULL != cd->toolcbfunc) {
                    cd->toolcbfunc(xrc, NULL, cd->cbdata);
                }
                PRTE_RELEASE(cd);
            }
            return;
        }
    }

    /* the tool is not a client of ours, but we can provide at least some information */
    rc = prte_pmix_server_register_tool(cd->target.nspace);
    if (PMIX_SUCCESS != rc) {
        rc = prte_pmix_convert_rc(rc);
    }
    if (NULL != cd->toolcbfunc) {
        cd->toolcbfunc(rc, &cd->target, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo, pmix_tool_connection_cbfunc_t cbfunc,
                            void *cbdata)
{
    pmix_server_req_t *cd;

    prte_output_verbose(2, prte_pmix_server_globals.output, "%s TOOL CONNECTION REQUEST RECVD",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PRTE_NEW(pmix_server_req_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _toolconn, cd);
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
}

static void lgcbfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

void pmix_server_log_fn(const pmix_proc_t *client, const pmix_info_t data[], size_t ndata,
                        const pmix_info_t directives[], size_t ndirs, pmix_op_cbfunc_t cbfunc,
                        void *cbdata)
{
    size_t n, cnt, dcnt;
    pmix_data_buffer_t *buf;
    int rc = PRTE_SUCCESS;
    pmix_data_buffer_t pbuf, dbuf;
    pmix_byte_object_t pbo, dbo;
    pmix_status_t ret;
    prte_output_verbose(2, prte_pmix_server_globals.output, "%s logging info",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    /* if we are the one that passed it down, then we don't pass it back */
    dcnt = 0;
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], "prte.log.noloop")) {
            if (PMIX_INFO_TRUE(&directives[n])) {
                rc = PMIX_SUCCESS;
                goto done;
            }
        }
        else {
            ret = PMIx_Data_pack(NULL, &dbuf, (pmix_info_t *) &directives[n], 1, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            }
            dcnt++;
        }
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    cnt = 0;

    for (n = 0; n < ndata; n++) {
        /* ship this to our HNP/MASTER for processing, even if that is us */
        ret = PMIx_Data_pack(NULL, &pbuf, (pmix_info_t *) &data[n], 1, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
        }
        ++cnt;
    }
    if (0 < cnt) {
        PMIX_DATA_BUFFER_CREATE(buf);
        rc = PMIx_Data_pack(NULL, buf, &cnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &dcnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_unload(&pbuf, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

        rc = PMIx_Data_unload(&dbuf, &dbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &dbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&dbo);

        rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_LOGGING,
                                     prte_rml_send_callback, NULL);

        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
        }
    }

done:
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    PRTE_PMIX_THREADSHIFT(PRTE_NAME_WILDCARD, NULL, rc, NULL, NULL, 0, lgcbfn, cbfunc, cbdata);
}

pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor, const pmix_proc_t targets[],
                                      size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int rc, j;
    int32_t signum;
    size_t m, n;
    prte_proc_t *proc;
    pmix_nspace_t jobid;
    prte_pointer_array_t parray, *ptrarray;
    pmix_data_buffer_t *cmd;
    prte_daemon_cmd_flag_t cmmnd;
    prte_grpcomm_signature_t *sig;
    pmix_proc_t *proct;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m = 0; m < ndirs; m++) {
        if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_KILL, PMIX_MAX_KEYLEN)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                PRTE_CONSTRUCT(&parray, prte_pointer_array_t);
                for (n = 0; n < ntargets; n++) {
                    if (PMIX_RANK_WILDCARD == targets[n].rank) {
                        /* create an object */
                        proc = PRTE_NEW(prte_proc_t);
                        PMIX_LOAD_PROCID(&proc->name, targets[n].nspace, PMIX_RANK_WILDCARD);
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = prte_get_proc_object(&targets[n]))) {
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
                for (j = 0; j < parray.size; j++) {
                    if (NULL != (proc = (prte_proc_t *) prte_pointer_array_get_item(&parray, j))) {
                        PRTE_RELEASE(proc);
                    }
                }
                PRTE_DESTRUCT(&parray);
            }
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_TERMINATE, PMIX_MAX_KEYLEN)) {
            if (NULL == targets) {
                /* terminate the daemons and all running jobs */
                PMIX_DATA_BUFFER_CREATE(cmd);
                /* pack the command */
                cmmnd = PRTE_DAEMON_HALT_VM_CMD;
                rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(cmd);
                    return rc;
                }
                /* goes to all daemons */
                sig = PRTE_NEW(prte_grpcomm_signature_t);
                sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
                sig->sz = 1;
                PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
                if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                    PRTE_ERROR_LOG(rc);
                }
                PMIX_DATA_BUFFER_RELEASE(cmd);
                PRTE_RELEASE(sig);
            }
        } else if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_SIGNAL, PMIX_MAX_KEYLEN)) {
            PMIX_DATA_BUFFER_CREATE(cmd);
            cmmnd = PRTE_DAEMON_SIGNAL_LOCAL_PROCS;
            /* pack the command */
            rc = PMIx_Data_pack(NULL, cmd, &cmmnd, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* pack the target jobid */
            if (NULL == targets) {
                PMIX_LOAD_NSPACE(&jobid, NULL);
            } else {
                proct = (pmix_proc_t *) &targets[0];
                PMIX_LOAD_NSPACE(&jobid, proct->nspace);
            }
            rc = PMIx_Data_pack(NULL, cmd, &jobid, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* pack the signal */
            PMIX_VALUE_GET_NUMBER(rc, &directives[m].value, signum, int32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            rc = PMIx_Data_pack(NULL, cmd, &signum, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(cmd);
                return rc;
            }
            /* goes to all daemons */
            sig = PRTE_NEW(prte_grpcomm_signature_t);
            sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            sig->sz = 1;
            PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                PRTE_ERROR_LOG(rc);
            }
            PMIX_DATA_BUFFER_RELEASE(cmd);
            PRTE_RELEASE(sig);
        }
    }

    return PMIX_OPERATION_SUCCEEDED;
}

static void relcb(void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PRTE_RELEASE(cd);
}
static void group_release(int status, pmix_data_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;
    int32_t cnt;
    int rc = PRTE_SUCCESS;
    pmix_status_t ret;
    uint32_t cid;
    size_t n;
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
        rc = PMIx_Data_unpack(NULL, buf, &cid, &cnt, PMIX_UINT32);
        /* error if they didn't return it */
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
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
            PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_UINT32);
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
    pmix_byte_object_t *bo = NULL;

    /* they are required to pass us an id */
    if (NULL == gpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check the directives */
    for (i = 0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            if (PMIX_INFO_TRUE(&directives[i])) {
                mode = 1;
            }
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_EMBED_BARRIER)) {
            fence = PMIX_INFO_TRUE(&directives[i]);
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ENDPT_DATA)) {
            bo = (pmix_byte_object_t *) &directives[i].value.data.bo;
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
        PRTE_LIST_FOREACH(pset, &prte_pmix_server_globals.psets, pmix_server_pset_t)
        {
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
        cd->sig->signature = (pmix_proc_t *) malloc(cd->sig->sz * sizeof(pmix_proc_t));
        memcpy(cd->sig->signature, procs, cd->sig->sz * sizeof(pmix_proc_t));
    }
    PMIX_DATA_BUFFER_CREATE(cd->buf);
    /* if they provided us with a data blob, send it along */
    if (NULL != bo) {
        /* We don't own the byte_object and so we have to
         * copy it here */
        rc = PMIx_Data_embed(cd->buf, bo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    /* pass it to the global collective algorithm */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd->sig, cd->buf,
                                                     mode, PMIX_SUCCESS,
                                                     group_release, cd))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_iof_pull_fn(const pmix_proc_t procs[], size_t nprocs,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_iof_channel_t channels, pmix_op_cbfunc_t cbfunc,
                                      void *cbdata)
{
    prte_iof_sink_t *sink;
    size_t i;
    bool stop = false;

    /* no really good way to do this - we have to search the directives to
     * see if we are being asked to stop the specified channels before
     * we can process them */
    for (i = 0; i < ndirs; i++) {
        if (PMIX_CHECK_KEY(&directives[i], PMIX_IOF_STOP)) {
            stop = PMIX_INFO_TRUE(&directives[i]);
            break;
        }
    }

    /* Set up I/O forwarding sinks and handlers for stdout and stderr for each proc
     * requesting I/O forwarding */
    for (i = 0; i < nprocs; i++) {
        if (channels & PMIX_FWD_STDOUT_CHANNEL) {
            if (stop) {
                /* ask the IOF to stop forwarding this channel */
            } else {
                PRTE_IOF_SINK_DEFINE(&sink, &procs[i], fileno(stdout), PRTE_IOF_STDOUT,
                                     prte_iof_base_write_handler);
                PRTE_IOF_SINK_ACTIVATE(sink->wev);
            }
        }
        if (channels & PMIX_FWD_STDERR_CHANNEL) {
            if (stop) {
                /* ask the IOF to stop forwarding this channel */
            } else {
                PRTE_IOF_SINK_DEFINE(&sink, &procs[i], fileno(stderr), PRTE_IOF_STDERR,
                                     prte_iof_base_write_handler);
                PRTE_IOF_SINK_ACTIVATE(sink->wev);
            }
        }
    }
    return PMIX_OPERATION_SUCCEEDED;
}

static void pmix_server_stdin_push(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    pmix_byte_object_t *bo = (pmix_byte_object_t *) cd->server_object;
    size_t n;

    for (n = 0; n < cd->nprocs; n++) {
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_server_globals.output,
                             "%s pmix_server_stdin_push to dest %s: size %zu",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(&cd->procs[n]),
                             bo->size));
        prte_iof.push_stdin(&cd->procs[n], (uint8_t *) bo->bytes, bo->size);
    }

    if (NULL == bo->bytes || 0 == bo->size) {
        cd->cbfunc(PMIX_ERR_IOF_COMPLETE, cd->cbdata);
    } else {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }

    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_stdin_fn(const pmix_proc_t *source, const pmix_proc_t targets[],
                                   size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                   const pmix_byte_object_t *bo, pmix_op_cbfunc_t cbfunc,
                                   void *cbdata)
{
    // Note: We are ignoring the directives / ndirs at the moment
    PRTE_IO_OP(targets, ntargets, bo, pmix_server_stdin_push, cbfunc, cbdata);

    // Do not send PMIX_OPERATION_SUCCEEDED since the op hasn't completed yet.
    // We will send it back when we are done by calling the cbfunc.
    return PMIX_SUCCESS;
}
