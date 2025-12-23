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
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void pmix_server_stdin_push(int sd, short args, void *cbdata);

static void _client_conn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p = NULL, *p2;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n;
    uid_t euid;
    gid_t egid;
    pid_t pid;
    bool singleton = false;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t *) cd->server_object;
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_REG);
        PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_REGISTERED);
    }

    // since this is a client of mine, look it up
    p2 = prte_get_proc_object(&cd->proc);
    if (NULL == p2) {
        // not one of our clients!
        rc = PMIX_ERR_NOT_SUPPORTED;
       goto complete;
    }
    if (NULL != p && p2 != p) {
        // bogus!
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto complete;
    }
    /* if p is NULL and we were launched by a singleton,
     * then this is our singleton connecting to us */
    if (NULL == p) {
        if (NULL != prte_pmix_server_globals.singleton) {
            // use the retrieved proc object
            p = p2;
            singleton = true;
        } else {
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto complete;
        }
    }

    // check if the uid, gid, and pid match
    for (n=0; n < cd->ninfo; n++) {
        if (PMIx_Check_key(cd->info[n].key, PMIX_USERID)) {
            rc = PMIx_Value_get_number(&cd->info[n].value, (void*)&euid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto complete;
            }
            if (prte_process_info.euid != euid) {
                rc = PMIX_ERR_NOT_SUPPORTED;
                PMIX_ERROR_LOG(rc);
                goto complete;
            }
            continue;
        }
        if (PMIx_Check_key(cd->info[n].key, PMIX_GRPID)) {
            rc = PMIx_Value_get_number(&cd->info[n].value, (void*)&egid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto complete;
            }
            if (prte_process_info.egid != egid) {
                rc = PMIX_ERR_NOT_SUPPORTED;
                PMIX_ERROR_LOG(rc);
                goto complete;
            }
            continue;
        }
        if (prte_pmix_server_globals.require_pid_match) {
            if (PMIx_Check_key(cd->info[n].key, PMIX_PROC_PID)) {
                rc = PMIx_Value_get_number(&cd->info[n].value, (void*)&pid, PMIX_PID);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto complete;
                }
                if (singleton) {
                    // we didn't know the pid initially, so update it here
                    p->pid = pid;
                } else {
                    if (p->pid != pid) {
                        rc = PMIX_ERR_NOT_SUPPORTED;
                        PMIX_ERROR_LOG(rc);
                        goto complete;
                    }
                }
                continue;
            }
        }
    }

complete:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_client_connected2_fn(const pmix_proc_t *proc,
                                               void *server_object,
                                               pmix_info_t *info, size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc,
                                               void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    PRTE_HIDE_UNUSED_PARAMS(info, ninfo);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s Client connected2 received for %s with %d info",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_NAME_PRINT(proc), (int)ninfo);

    /* need to thread-shift this request as we are going
     * to access our global data */

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    memcpy(&cd->proc, proc, sizeof(pmix_proc_t));
    cd->server_object = server_object;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _client_conn, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

static void _client_finalized(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_proc_t *p;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        /* we were passed back the prte_proc_t */
        p = (prte_proc_t *) cd->server_object;
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_HAS_DEREG);
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void *server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    PRTE_SERVER_PMIX_THREADSHIFT(proc, server_object, PRTE_SUCCESS,
                          NULL, NULL, 0, _client_finalized,
                          cbfunc, cbdata);
    return PRTE_SUCCESS;
}

/* the fields being passed include:
 *
 * cd->proc: the process that called "abort". Note that this isn't
 *           the process to be aborted!
 *
 * server_object: object PRRTE passed down to PMIx server when
 *            registering the process returned in cd->proc. Note
 *            that this will be non-NULL only if the requesting
 *            process is a local client - i.e., it will be NULL
 *            if abort was called by a tool.
 *
 * cd->procs: the processes that actually are to be aborted. This
 *            can include specific process IDs, wildcard ranks to
 *            indicate termination of an entire job, or any combination
 *            of the two. A NULL value indicates that the job of the
 *            requesting process (i.e., the process in cd->proc) is
 *            to be terminated.
 *
 * cd->nprocs: number of procs in the cd->procs array
 *
 * cd->status: the status we were given. This is to be returned upon
 *             termination of the procs.
 *
 * msg: string message to be output to user
 *
 * cbfunc/cbdata: callback for when abort is complete
 */
static void _client_abort(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    prte_job_t *jdata;
    pmix_proc_t pname;
    prte_proc_t *p;
    pmix_status_t rc;
    size_t n;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->server_object) {
        p = (prte_proc_t *) cd->server_object;
    } else {
        p = prte_get_proc_object(&cd->proc);
    }

    // if they didn't specify procs to abort, then we abort
    // the entire job of the requestor
    if (NULL == cd->procs) {
        if (NULL == p) {
            PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
            rc = PMIX_ERR_NOT_FOUND;
        } else {
            p->exit_code = cd->status;
            PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_CALLED_ABORT);
            rc = PMIX_SUCCESS;
        }
        goto release;
    }

    // otherwise, we need to abort the specified procs
    for (n=0; n < cd->nprocs; n++) {
        if (PMIX_RANK_WILDCARD == cd->procs[n].rank) {
            // just declare it for rank=0
            jdata = prte_get_job_data_object(cd->procs[n].nspace);
            if (NULL == jdata) {
                PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
                rc = PMIX_ERR_NOT_FOUND;
                goto release;
            }
            jdata->exit_code = cd->status;
            PMIx_Load_procid(&pname, cd->procs[n].nspace, 0);
            p = prte_get_proc_object(&pname);
            if (NULL == p) {
                PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
                rc = PMIX_ERR_NOT_FOUND;
                goto release;
            }
            p->exit_code = cd->status;
            PRTE_ACTIVATE_PROC_STATE(&pname, PRTE_PROC_STATE_CALLED_ABORT);
            rc = PMIX_SUCCESS;

        } else {
            p = prte_get_proc_object(&cd->procs[n]);
            if (NULL == p) {
                PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
                rc = PMIX_ERR_NOT_FOUND;
                goto release;
            }
            p->exit_code = cd->status;
            PRTE_ACTIVATE_PROC_STATE(&p->name, PRTE_PROC_STATE_CALLED_ABORT);
            rc = PMIX_SUCCESS;
        }
    }

release:
    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object, int status,
                                   const char msg[], pmix_proc_t procs[], size_t nprocs,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "Abort request %s received",
                        (NULL == msg) ? "NULL" : msg);

    PRTE_SERVER_PMIX_THREADSHIFT(proc, server_object, status,
                                 msg, procs, nprocs, _client_abort,
                                 cbfunc, cbdata);
    return PRTE_SUCCESS;
}

void pmix_server_tconn_return(int status, pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                              void *cbdata)
{
    prte_pmix_server_req_t *req;
    int rc, room;
    int32_t ret, cnt;
    pmix_nspace_t jobid;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
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
    req = (prte_pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, room);
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, room, NULL);

    if (NULL == req) {
        /* we are hosed */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        pmix_output(0, "UNABLE TO RETRIEVE SPWN_REQ FOR JOB %s [room=%d]", jobid, room);
        return;
    }

    if (PMIX_SUCCESS == ret) {
        /* unpack the jobid */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &jobid, &cnt, PMIX_PROC_NSPACE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        PMIX_LOAD_NSPACE(req->target.nspace, jobid);
        /* the tool is not a client of ours, but we can provide at least some information */
        rc = prte_pmix_server_register_tool(req);
        if (PRTE_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            // we can live without it
        }
    }

    req->toolcbfunc(ret, &req->target, req->cbdata);

    /* cleanup */
    PMIX_RELEASE(req);
}

static void _toolconn(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *cd = (prte_pmix_server_req_t *) cbdata;
    int rc;
    char *tmp;
    size_t n;
    pmix_data_buffer_t *buf;
    prte_plm_cmd_flag_t command = PRTE_PLM_TOOL_ATTACHED_CMD;
    pmix_status_t xrc = PMIX_SUCCESS, trc;
    bool primary = false;
    bool nspace_given = false;
    bool rank_given = false;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
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
                PMIX_VALUE_GET_NUMBER(trc, &cd->info[n].value, cd->uid, uid_t);
                if (PMIX_SUCCESS == xrc && PMIX_SUCCESS != trc) {
                    xrc = trc;
                }

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GRPID)) {
                PMIX_VALUE_GET_NUMBER(trc, &cd->info[n].value, cd->gid, gid_t);
                if (PMIX_SUCCESS == xrc && PMIX_SUCCESS != trc) {
                    xrc = trc;
                }

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_NSPACE)) {
                PMIX_LOAD_NSPACE(cd->target.nspace, cd->info[n].value.data.string);
                nspace_given = true;

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_RANK)) {
                cd->target.rank = cd->info[n].value.data.rank;
                rank_given = true;

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_HOSTNAME)) {
                cd->operation = strdup(cd->info[n].value.data.string);

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CMD_LINE)) {
                cd->cmdline = strdup(cd->info[n].value.data.string);

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_LAUNCHER)) {
                cd->launcher = PMIX_INFO_TRUE(&cd->info[n]);

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_SERVER_SCHEDULER)) {
                cd->scheduler = PMIX_INFO_TRUE(&cd->info[n]);

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_PRIMARY_SERVER)) {
                primary = PMIX_INFO_TRUE(&cd->info[n]);

            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_PROC_PID)) {
                PMIX_VALUE_GET_NUMBER(trc, &cd->info[n].value, cd->pid, pid_t);
                if (PMIX_SUCCESS == xrc && PMIX_SUCCESS != trc) {
                    xrc = trc;
                }
            }
        }
    }

    if (prte_pmix_server_globals.no_foreign_tools) {
        // the PMIx "uid" is the effective uid of the tool,
        // so compare it to our effective  uid
        if (cd->uid != prte_process_info.euid) {
            // this should be handled by the PMIx library,
            // but we back it up here just to be safe
            xrc = PMIX_ERR_NOT_SUPPORTED;
        }
    }

    if (PMIX_SUCCESS != xrc) {
        if (NULL != cd->toolcbfunc) {
            cd->toolcbfunc(xrc, &cd->target, cd->cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s %s CONNECTION FROM UID %d GID %d NSPACE %s PID %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        cd->launcher ? "LAUNCHER" : (cd->scheduler ? "SCHEDULER" : "TOOL"),
                        cd->uid, cd->gid, cd->target.nspace, cd->pid);

    /* if this is the scheduler and we are not the DVM master, then
     * this is not allowed */
    if (cd->scheduler) {
        if (!PRTE_PROC_IS_MASTER) {
            cd->toolcbfunc(PMIX_ERR_NOT_SUPPORTED, NULL, cd->cbdata);
            PMIX_RELEASE(cd);
            return;
        } else {
            /* mark that the scheduler has attached to us */
            prte_pmix_server_globals.scheduler_connected = true;
            // the scheduler always self-assigns its ID
            if (!nspace_given || !rank_given) {
                cd->toolcbfunc(PMIX_ERR_NOT_SUPPORTED, NULL, cd->cbdata);
                PMIX_RELEASE(cd);
                return;
            }
            PMIX_LOAD_PROCID(&prte_pmix_server_globals.scheduler,
                             cd->target.nspace, cd->target.rank);
            rc = PMIX_SUCCESS;

            if (!primary) {
                /* we cannot immediately set the scheduler to be our
                 * PMIx server as the PMIx library hasn't finished
                 * recording it */
                goto complete;
            }
            // it has been recorded in the library, so record it here
            prte_pmix_server_globals.scheduler_set_as_server = true;
            goto complete;
        }
    }

    /* if they gave us a namespace but not a rank, then assume rank=0 */
    if (!rank_given) {
        cd->target.rank = 0;
    }

    /* if we are the HNP, we can handle this ourselves */
    if (PRTE_PROC_IS_MASTER) {
        if (!nspace_given) {
            /* the new nspace is our base nspace with an "@N" extension */
            pmix_asprintf(&tmp, "%s@%u", prte_plm_globals.base_nspace, prte_plm_globals.next_jobid);
            PMIX_LOAD_NSPACE(cd->target.nspace, tmp);
            free(tmp);
            prte_plm_globals.next_jobid++;
        }
        // register this job - will add to our array of jobs
        rc = prte_pmix_server_register_tool(cd);
        if (PMIX_SUCCESS != rc) {
            rc = prte_pmix_convert_rc(rc);
        }
        goto complete;
    }

    /* not the DVM master, so we have to alert the HNP that
     * a tool has connected to a daemon so the DVM doesn't
     * shut down until the tool has disconnected */
    PMIX_DATA_BUFFER_CREATE(buf);
    rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    // record the request and pass the location along
    cd->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, cd);
    rc = PMIx_Data_pack(NULL, buf, &cd->local_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    // flag if we need a jobid assigned
    rc = PMIx_Data_pack(NULL, buf, &nspace_given, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    // if we do, then pass along the rank so they have it
    if (!nspace_given) {
        rc = PMIx_Data_pack(NULL, buf, &cd->target.rank, 1, PMIX_PROC_RANK);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    } else {
        // if not, then pass along the full procID
        rc = PMIx_Data_pack(NULL, buf, &cd->target, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    // pass along the cmd line
    rc = PMIx_Data_pack(NULL, buf, &cd->cmdline, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    // and the pid
    rc = PMIx_Data_pack(NULL, buf, &cd->pid, 1, PMIX_PID);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }

    /* send it to the HNP for processing - might be myself! */
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank,
                  buf, PRTE_RML_TAG_PLM);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        xrc = prte_pmix_convert_rc(rc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, cd->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        if (NULL != cd->toolcbfunc) {
            cd->toolcbfunc(xrc, NULL, cd->cbdata);
        }
        PMIX_RELEASE(cd);
    }
    return;


complete:
    if (NULL != cd->toolcbfunc) {
        cd->toolcbfunc(rc, &cd->target, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                            pmix_tool_connection_cbfunc_t cbfunc,
                            void *cbdata)
{
    prte_pmix_server_req_t *cd;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION REQUEST RECVD",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PMIX_NEW(prte_pmix_server_req_t);
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->target.rank = 0; // set default for tool
    cd->info = info;
    cd->ninfo = ninfo;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _toolconn, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
}

#if PRTE_PMIX_SERVER2_UPCALLS

pmix_status_t pmix_tool_connected2_fn(pmix_info_t *info, size_t ninfo,
                                      pmix_tool_connection_cbfunc_t cbfunc,
                                      void *cbdata)
{
    prte_pmix_server_req_t *cd;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION2 REQUEST RECVD",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PMIX_NEW(prte_pmix_server_req_t);
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->target.rank = 0; // set default for tool
    cd->info = info;
    cd->ninfo = ninfo;

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _toolconn, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}
#endif

static void lgcbfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
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

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s logging info",
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
        /* pack the source of this log request */
        rc = PMIx_Data_pack(NULL, buf, (void*)client, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* pack number of info provided */
        rc = PMIx_Data_pack(NULL, buf, &cnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* pack number of directives given */
        rc = PMIx_Data_pack(NULL, buf, &dcnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* bring over the packed info blob */
        rc = PMIx_Data_unload(&pbuf, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        /* pack the directives blob */
        rc = PMIx_Data_unload(&dbuf, &dbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &dbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&dbo);
        /* send the result to the HNP */
        PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf,
                      PRTE_RML_TAG_LOGGING);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
        }
    }

done:
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    PRTE_SERVER_PMIX_THREADSHIFT(PRTE_NAME_WILDCARD, NULL, rc, NULL, NULL, 0, lgcbfn, cbfunc, cbdata);
}

#if PRTE_PMIX_SERVER2_UPCALLS
pmix_status_t pmix_server_log2_fn(const pmix_proc_t *client, const pmix_info_t data[], size_t ndata,
                                  const pmix_info_t directives[], size_t ndirs, pmix_op_cbfunc_t cbfunc,
                                  void *cbdata)
{
    size_t n, cnt, dcnt;
    pmix_data_buffer_t *buf;
    int rc = PRTE_SUCCESS;
    pmix_data_buffer_t pbuf, dbuf;
    pmix_byte_object_t pbo, dbo;
    pmix_status_t ret;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s logging2 info",
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
                return ret;
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
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            return ret;
        }
        ++cnt;
    }
    if (0 < cnt) {
        PMIX_DATA_BUFFER_CREATE(buf);
        /* pack the source of this log request */
        rc = PMIx_Data_pack(NULL, buf, (void*)client, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            return rc;
        }
        /* pack number of info provided */
        rc = PMIx_Data_pack(NULL, buf, &cnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            return rc;
        }
        /* pack number of directives given */
        rc = PMIx_Data_pack(NULL, buf, &dcnt, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            return rc;
        }
        /* bring over the packed info blob */
        rc = PMIx_Data_unload(&pbuf, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            return rc;
        }
        rc = PMIx_Data_pack(NULL, buf, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        /* pack the directives blob */
        rc = PMIx_Data_unload(&dbuf, &dbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
        rc = PMIx_Data_pack(NULL, buf, &dbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&dbo);
        /* send the result to the HNP */
        PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf,
                      PRTE_RML_TAG_LOGGING);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    }

done:
    /* we cannot directly execute the callback here
     * as it would threadlock - so shift to somewhere
     * safe */
    PRTE_SERVER_PMIX_THREADSHIFT(PRTE_NAME_WILDCARD, NULL, rc, NULL, NULL, 0, lgcbfn, cbfunc, cbdata);
    return PMIX_SUCCESS;
}
#endif

pmix_status_t pmix_server_iof_pull_fn(const pmix_proc_t procs[], size_t nprocs,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_iof_channel_t channels, pmix_op_cbfunc_t cbfunc,
                                      void *cbdata)
{
    prte_iof_sink_t *sink;
    size_t i;
    bool stop = false;
    PRTE_HIDE_UNUSED_PARAMS(cbfunc, cbdata);

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
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    for (n = 0; n < cd->nprocs; n++) {
        PMIX_OUTPUT_VERBOSE((1, prte_pmix_server_globals.output,
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

    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_stdin_fn(const pmix_proc_t *source, const pmix_proc_t targets[],
                                   size_t ntargets, const pmix_info_t directives[], size_t ndirs,
                                   const pmix_byte_object_t *bo, pmix_op_cbfunc_t cbfunc,
                                   void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(source, directives, ndirs);

    // Note: We are ignoring the directives / ndirs at the moment
    PRTE_IO_OP(targets, ntargets, bo, pmix_server_stdin_push, cbfunc, cbdata);

    // Do not send PMIX_OPERATION_SUCCEEDED since the op hasn't completed yet.
    // We will send it back when we are done by calling the cbfunc.
    return PMIX_SUCCESS;
}
