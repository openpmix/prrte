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
 * Copyright (c) 2021-2023 Nanook Consulting  All rights reserved.
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
#include "src/tools/psched/psched.h"

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
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_SERVER_SCHEDULER)) {
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
                        cd->launcher ? "LAUNCHER" : (cd->scheduler ? "SCHEDULER" : "TOOL"),
                        cd->uid, cd->gid, cd->target.nspace);

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
            PMIX_LOAD_PROCID(&prte_pmix_server_globals.scheduler,
                             cd->target.nspace, cd->target.rank);
            /* we cannot immediately set the scheduler to be our
             * PMIx server as the PMIx library hasn't finished
             * recording it */
        }
    }

    /* if we are not the HNP or master, and the tool doesn't
     * already have a self-assigned name, then
     * we need to ask the master for one */
    if (PMIX_NSPACE_INVALID(cd->target.nspace) || PMIX_RANK_INVALID == cd->target.rank) {
        /* if we are the HNP, we can directly assign the jobid */
        if (PRTE_PROC_IS_MASTER) {
            /* the new nspace is our base nspace with an "@N" extension */
            pmix_asprintf(&tmp, "%s@%u", prte_plm_globals.base_nspace, prte_plm_globals.next_jobid);
            PMIX_LOAD_PROCID(&cd->target, tmp, 0);
            free(tmp);
            prte_plm_globals.next_jobid++;
        } else {
            cd->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, cd);
            /* we need to send this to the HNP for a jobid */
            PMIX_DATA_BUFFER_CREATE(buf);
            rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            rc = PMIx_Data_pack(NULL, buf, &cd->local_index, 1, PMIX_INT);
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
    prte_event_set_priority(&(cd->ev), PRTE_MSG_PRI);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
}

static void lgcbfn(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

void psched_log_fn(const pmix_proc_t *client,
                   const pmix_info_t data[], size_t ndata,
                   const pmix_info_t directives[], size_t ndirs,
                   pmix_op_cbfunc_t cbfunc, void *cbdata)
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

static void relcb(void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}
static void group_release(int status, pmix_data_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;
    int32_t cnt;
    int rc = PRTE_SUCCESS;
    pmix_status_t ret;
    bool assignedID = false;
    bool procsadded = false;
    size_t cid;
    pmix_proc_t *procs, *members;
    size_t n, num_members;
    pmix_data_array_t darray;
    pmix_info_t info;
    pmix_data_buffer_t dbuf;
    pmix_byte_object_t bo;
    int32_t byused;
    pmix_server_pset_t *pset;

    PMIX_ACQUIRE_OBJECT(cd);

   pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s group request complete",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (PRTE_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    /* if this was a destruct operation, then there is nothing
     * further we need do */
    if (PMIX_GROUP_DESTRUCT == cd->op) {
        /* find this group ID on our list of groups */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, cd->grpid)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
        rc = status;
        goto complete;
    }

    /* check for any directives */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    PMIX_DATA_BUFFER_LOAD(&dbuf, bo.bytes, bo.size);

    cd->ninfo = 2;
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &dbuf, &info, &cnt, PMIX_INFO);
    while (PMIX_SUCCESS == rc) {
        if (PMIX_CHECK_KEY(&info, PMIX_GROUP_CONTEXT_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info.value, cid, size_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                cd->ninfo = 0;
                PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
                goto complete;
            }
            assignedID = true;
            cd->ninfo++;
        } else if (PMIX_CHECK_KEY(&info, PMIX_GROUP_ADD_MEMBERS)) {
            members = (pmix_proc_t*)info.value.data.darray->array;
            num_members = info.value.data.darray->size;
            PMIX_PROC_CREATE(procs, cd->nprocs + num_members);
            for (n=0; n < cd->nprocs; n++) {
                PMIX_XFER_PROCID(&procs[n], &cd->procs[n]);
            }
            for (n=0; n < num_members; n++) {
                PMIX_XFER_PROCID(&procs[n+cd->nprocs], &members[n]);
            }
            PMIX_PROC_FREE(cd->procs, cd->nprocs);
            cd->procs = procs;
            cd->nprocs += num_members;
            procsadded = true;
        }
        /* cleanup */
        PMIX_INFO_DESTRUCT(&info);
        /* get the next object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &dbuf, &info, &cnt, PMIX_INFO);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
    /* the unpacking loop will have ended when the unpack either
     * went past the end of the buffer */
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    rc = PMIX_SUCCESS;

    if (PMIX_GROUP_CONSTRUCT == cd->op) {
       /* add it to our list of known groups */
        pset = PMIX_NEW(pmix_server_pset_t);
        pset->name = strdup(cd->grpid);
        pset->num_members = cd->nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
        pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);
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

    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    n = 0;
    // pass back the final group membership
    darray.type = PMIX_PROC;
    darray.array = cd->procs;
    darray.size = cd->nprocs;
    PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
    PMIX_PROC_FREE(cd->procs, cd->nprocs);
    ++n;
    if (assignedID) {
        PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
        ++n;
    }
    if (NULL != bo.bytes && 0 < bo.size) {
        PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_ENDPT_DATA, &bo, PMIX_BYTE_OBJECT);
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
        PMIX_RELEASE(cd);
    }
}

pmix_status_t psched_group_fn(pmix_group_operation_t op, char *grpid,
                              const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i;
    bool assignID = false;
    pmix_server_pset_t *pset;
    bool fence = false;
    bool force_local = false;
    pmix_byte_object_t *bo = NULL;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s group request recvd",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* they are required to pass us an id */
    if (NULL == grpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check the directives */
    for (i = 0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            assignID = PMIX_INFO_TRUE(&directives[i]);
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_EMBED_BARRIER)) {
            fence = PMIX_INFO_TRUE(&directives[i]);
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ENDPT_DATA)) {
            bo = (pmix_byte_object_t *) &directives[i].value.data.bo;
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_TIMEOUT)) {
            tv.tv_sec = directives[i].value.data.uint32;
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&directives[i]);
        }
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned, or they insist on forcing local
     * completion of the operation, then we are done */
    if ((!fence && !assignID) || force_local) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s group request - purely local",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        if (PMIX_GROUP_CONSTRUCT == op) {
            /* add it to our list of known groups */
            pset = PMIX_NEW(pmix_server_pset_t);
            pset->name = strdup(grpid);
            pset->num_members = nprocs;
            PMIX_PROC_CREATE(pset->members, pset->num_members);
            memcpy(pset->members, procs, nprocs * sizeof(pmix_proc_t));
            pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);
        } else if (PMIX_GROUP_DESTRUCT == op) {
            /* find this group ID on our list of groups */
            PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
            {
                if (0 == strcmp(pset->name, grpid)) {
                    pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                    PMIX_RELEASE(pset);
                    break;
                }
            }
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    cd = PMIX_NEW(prte_pmix_mdx_caddy_t);
    cd->grpid = grpid;
    cd->op = op;
    /* have to copy the procs in case we add members */
    PMIX_PROC_CREATE(cd->procs, nprocs);
    memcpy(cd->procs, procs, nprocs * sizeof(pmix_proc_t));
    cd->nprocs = nprocs;
    cd->grpcbfunc = group_release;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* compute the signature of this collective */
    if (NULL != procs) {
        cd->sig = PMIX_NEW(prte_grpcomm_signature_t);
        cd->sig->sz = nprocs;
        cd->sig->signature = (pmix_proc_t *) malloc(cd->sig->sz * sizeof(pmix_proc_t));
        memcpy(cd->sig->signature, procs, cd->sig->sz * sizeof(pmix_proc_t));
    }
    /* setup the ctrls blob - this will include any "add_members" directive */
    rc = prte_pack_ctrl_options(&cd->ctrls, directives, ndirs);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
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
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd))) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}
