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
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
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
#include <fcntl.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_dirpath.h"
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
#include "src/tools/psched/psched.h"

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;

    lock->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

/* add any info that the tool couldn't self-assign */
static int register_tool(pmix_nspace_t nspace)
{
    pmix_status_t ret;
    prte_pmix_lock_t lock;
    int rc;
    prte_pmix_tool_t *tl;

    /* record this tool */
    tl = PMIX_NEW(prte_pmix_tool_t);
    PMIX_LOAD_PROCID(&tl->name, nspace, 0);
    pmix_list_append(&psched_globals.tools, &tl->super);

    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(nspace, 1, NULL, 0,
                                      opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prte_pmix_convert_status(ret);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        return rc;
    }
    PRTE_PMIX_WAIT_THREAD(&lock);
    rc = lock.status;
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    return rc;
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
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->uid, uid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PMIX_RELEASE(cd);
                    return;
                }
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GRPID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->gid, gid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PMIX_RELEASE(cd);
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
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_SERVER_SYS_CONTROLLER)) {
                cd->scheduler = PMIX_INFO_TRUE(&cd->info[n]);
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_PROC_PID)) {
                PMIX_VALUE_GET_NUMBER(xrc, &cd->info[n].value, cd->pid, pid_t);
                if (PMIX_SUCCESS != xrc) {
                    if (NULL != cd->toolcbfunc) {
                        cd->toolcbfunc(xrc, NULL, cd->cbdata);
                    }
                    PMIX_RELEASE(cd);
                    return;
                }
            }
        }
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s %s CONNECTION FROM UID %d GID %d NSPACE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        cd->launcher ? "LAUNCHER" : (cd->scheduler ? "SYSTEM CONTROLLER" : "TOOL"),
                        cd->uid, cd->gid, cd->target.nspace);

    if (cd->scheduler) {
        /* mark that the system controller has attached to us */
        psched_globals.controller_connected = true;
        PMIX_LOAD_PROCID(&psched_globals.syscontroller,
                         cd->target.nspace, cd->target.rank);
        /* we cannot immediately set the system controller to be our
         * PMIx server as the PMIx library hasn't finished
         * recording it */
    }

    /* if the tool doesn't already have a self-assigned name, then
     * there isn't much we can do about it */
    xrc = PMIX_SUCCESS;
    if (PMIX_NSPACE_INVALID(cd->target.nspace) || PMIX_RANK_INVALID == cd->target.rank) {
        xrc = PMIX_ERR_BAD_PARAM;
    } else {
        /* the tool is not a client of ours, but we can provide at least some information */
        rc = register_tool(cd->target.nspace);
        if (PRTE_SUCCESS != rc) {
            xrc = prte_pmix_convert_rc(rc);
        }
    }
    if (NULL != cd->toolcbfunc) {
        cd->toolcbfunc(xrc, &cd->target, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

void psched_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc,
                              void *cbdata)
{
    pmix_server_req_t *cd;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s TOOL CONNECTION REQUEST RECVD",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_server_req_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->toolcbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->target.rank = 0; // set default for tool

    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _toolconn, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
}

pmix_status_t psched_job_ctrl_fn(const pmix_proc_t *requestor,
                                 const pmix_proc_t targets[], size_t ntargets,
                                 const pmix_info_t directives[], size_t ndirs,
                                 pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int rc, j;
    int32_t signum;
    size_t m, n;
    prte_proc_t *proc;
    pmix_nspace_t jobid;
    pmix_pointer_array_t parray, *ptrarray;
    pmix_data_buffer_t *cmd;
    prte_daemon_cmd_flag_t cmmnd;
    prte_grpcomm_signature_t *sig;
    pmix_proc_t *proct;
    PRTE_HIDE_UNUSED_PARAMS(cbfunc, cbdata);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s job control request from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor->nspace, requestor->rank);

    for (m = 0; m < ndirs; m++) {
        if (0 == strncmp(directives[m].key, PMIX_JOB_CTRL_KILL, PMIX_MAX_KEYLEN)) {
            /* convert the list of targets to a pointer array */
            if (NULL == targets) {
                ptrarray = NULL;
            } else {
                PMIX_CONSTRUCT(&parray, pmix_pointer_array_t);
                for (n = 0; n < ntargets; n++) {
                    if (PMIX_RANK_WILDCARD == targets[n].rank) {
                        /* create an object */
                        proc = PMIX_NEW(prte_proc_t);
                        PMIX_LOAD_PROCID(&proc->name, targets[n].nspace, PMIX_RANK_WILDCARD);
                    } else {
                        /* get the proc object for this proc */
                        if (NULL == (proc = prte_get_proc_object(&targets[n]))) {
                            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                            continue;
                        }
                        PMIX_RETAIN(proc);
                    }
                    pmix_pointer_array_add(&parray, proc);
                }
                ptrarray = &parray;
            }
            if (PRTE_SUCCESS != (rc = prte_plm.terminate_procs(ptrarray))) {
                PRTE_ERROR_LOG(rc);
            }
            if (NULL != ptrarray) {
                /* cleanup the array */
                for (j = 0; j < parray.size; j++) {
                    if (NULL != (proc = (prte_proc_t *) pmix_pointer_array_get_item(&parray, j))) {
                        PMIX_RELEASE(proc);
                    }
                }
                PMIX_DESTRUCT(&parray);
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
                sig = PMIX_NEW(prte_grpcomm_signature_t);
                sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
                sig->sz = 1;
                PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
                if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                    PRTE_ERROR_LOG(rc);
                }
                PMIX_DATA_BUFFER_RELEASE(cmd);
                PMIX_RELEASE(sig);
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
                PMIX_LOAD_NSPACE(jobid, NULL);
            } else {
                proct = (pmix_proc_t *) &targets[0];
                PMIX_LOAD_NSPACE(jobid, proct->nspace);
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
            sig = PMIX_NEW(prte_grpcomm_signature_t);
            sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            sig->sz = 1;
            PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
            if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_DAEMON, cmd))) {
                PRTE_ERROR_LOG(rc);
            }
            PMIX_DATA_BUFFER_RELEASE(cmd);
            PMIX_RELEASE(sig);
        }
    }

    return PMIX_OPERATION_SUCCEEDED;
}
