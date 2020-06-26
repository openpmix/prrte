/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "src/include/constants.h"


#include <regex.h>

#include <time.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/class/prte_hash_table.h"
#include "src/threads/threads.h"
#include "src/util/proc_info.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/pmix/pmix-internal.h"

int prte_convert_jobid_to_nspace(pmix_nspace_t nspace, prte_jobid_t jobid)
{
    prte_job_t *jdata;

    /* zero out the nspace */
    PMIX_LOAD_NSPACE(nspace, NULL);

    /* if the jobid is WILDCARD, make the nspace so */
    if (PRTE_JOBID_WILDCARD == jobid) {
        PMIX_LOAD_NSPACE(nspace, "PRTE_JOBID_WILDCARD");
        return PRTE_SUCCESS;
    }
    if (PRTE_JOBID_INVALID == jobid) {
        PMIX_LOAD_NSPACE(nspace, "PRTE_JOBID_INVALID");
        return PRTE_SUCCESS;
    }

    jdata = prte_get_job_data_object(jobid);
    if (NULL != jdata) {
        PMIX_LOAD_NSPACE(nspace, jdata->nspace);
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_NOT_FOUND;
}


void prte_convert_daemon_nspace(prte_jobid_t *jobid, pmix_nspace_t nspace)
{
    prte_job_t *jdata;
    uint32_t hash32;
    uint16_t jobfam;

    jdata = PRTE_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, nspace);  // ensure we do this first so create_jobid can use the nspace
    PRTE_HASH_STR(nspace, hash32);

    /* now compress to 16-bits */
    jobfam = (uint16_t)(((0x0000ffff & (0xffff0000 & hash32) >> 16)) ^ (0x0000ffff & hash32));
    jdata->jobid = (0xffff0000 & ((uint32_t)jobfam << 16));
    *jobid = jdata->jobid;
    prte_hash_table_set_value_uint32(prte_job_data, jdata->jobid, jdata);
}

int prte_convert_nspace_to_jobid(prte_jobid_t *jobid, pmix_nspace_t nspace)
{
    uint32_t key;
    prte_job_t *jdata;
    uint32_t hash32, localjob = 0;
    uint16_t jobfam;
    char *p = NULL;

    /* set a default */
    *jobid = PRTE_JOBID_INVALID;

    /* if the nspace is empty, there is nothing more to do */
    if (0 == strlen(nspace)) {
        return PRTE_SUCCESS;
    }
    if (NULL != strstr(nspace, "JOBID_WILDCARD")) {
        *jobid = PRTE_JOBID_WILDCARD;
        return PRTE_SUCCESS;
    }
    if (NULL != strstr(nspace, "JOBID_INVALID")) {
        *jobid = PRTE_JOBID_INVALID;
        return PRTE_SUCCESS;
    }

    PRTE_HASH_TABLE_FOREACH(key, uint32, jdata, prte_job_data) {
        if (NULL != jdata && PMIX_CHECK_NSPACE(nspace, jdata->nspace)) {
            *jobid = jdata->jobid;
            return PRTE_SUCCESS;
        }
    }

    /* if we get here, we don't know this nspace */
    jdata = PRTE_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, nspace);  // ensure we do this first so create_jobid can use the nspace
    /* now find the "." at the end that indicates the child job */
    if (NULL != (p = strrchr(nspace, '.'))) {
        *p = '\0';
    }
    PRTE_HASH_STR(nspace, hash32);
    if (NULL != p) {
        *p = '.';
        ++p;
        localjob = strtoul(p, NULL, 10);
    }

    /* now compress to 16-bits */
    jobfam = (uint16_t)(((0x0000ffff & (0xffff0000 & hash32) >> 16)) ^ (0x0000ffff & hash32));
    jdata->jobid = (0xffff0000 & ((uint32_t)jobfam << 16)) | (0x0000ffff & localjob);
    *jobid = jdata->jobid;
    prte_hash_table_set_value_uint32(prte_job_data, jdata->jobid, jdata);

    return PRTE_SUCCESS;
}


pmix_status_t prte_pmix_convert_rc(int rc)
{
    switch (rc) {

#if PMIX_NUMERIC_VERSION >= 0x00040000
    case PRTE_ERR_HEARTBEAT_ALERT:
    case PRTE_ERR_FILE_ALERT:
    case PRTE_ERR_HEARTBEAT_LOST:
    case PRTE_ERR_SENSOR_LIMIT_EXCEEDED:
        return PMIX_ERR_JOB_SENSOR_BOUND_EXCEEDED;

    case PRTE_ERR_NO_EXE_SPECIFIED:
    case PRTE_ERR_NO_APP_SPECIFIED:
        return PMIX_ERR_JOB_NO_EXE_SPECIFIED;

    case PRTE_ERR_FAILED_TO_MAP:
        return PMIX_ERR_JOB_FAILED_TO_MAP;

    case PRTE_ERR_JOB_CANCELLED:
        return PMIX_ERR_JOB_CANCELLED;
#endif

    case PRTE_ERR_DEBUGGER_RELEASE:
        return PMIX_ERR_DEBUGGER_RELEASE;

    case PRTE_ERR_HANDLERS_COMPLETE:
        return PMIX_EVENT_ACTION_COMPLETE;

    case PRTE_ERR_PROC_ABORTED:
        return PMIX_ERR_PROC_ABORTED;

    case PRTE_ERR_PROC_REQUESTED_ABORT:
        return PMIX_ERR_PROC_REQUESTED_ABORT;

    case PRTE_ERR_PROC_ABORTING:
        return PMIX_ERR_PROC_ABORTING;

    case PRTE_ERR_NODE_DOWN:
        return PMIX_ERR_NODE_DOWN;

    case PRTE_ERR_NODE_OFFLINE:
        return PMIX_ERR_NODE_OFFLINE;

    case PRTE_ERR_JOB_TERMINATED:
        return PMIX_ERR_JOB_TERMINATED;

    case PRTE_ERR_PROC_RESTART:
        return PMIX_ERR_PROC_RESTART;

    case PRTE_ERR_PROC_CHECKPOINT:
        return PMIX_ERR_PROC_CHECKPOINT;

    case PRTE_ERR_PROC_MIGRATE:
        return PMIX_ERR_PROC_MIGRATE;

    case PRTE_ERR_EVENT_REGISTRATION:
        return PMIX_ERR_EVENT_REGISTRATION;

    case PRTE_ERR_NOT_IMPLEMENTED:
    case PRTE_ERR_NOT_SUPPORTED:
        return PMIX_ERR_NOT_SUPPORTED;

    case PRTE_ERR_NOT_FOUND:
        return PMIX_ERR_NOT_FOUND;

    case PRTE_ERR_PERM:
    case PRTE_ERR_UNREACH:
    case PRTE_ERR_SERVER_NOT_AVAIL:
        return PMIX_ERR_UNREACH;

    case PRTE_ERR_BAD_PARAM:
        return PMIX_ERR_BAD_PARAM;

    case PRTE_ERR_OUT_OF_RESOURCE:
        return PMIX_ERR_OUT_OF_RESOURCE;

    case PRTE_ERR_DATA_VALUE_NOT_FOUND:
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;

    case PRTE_ERR_TIMEOUT:
        return PMIX_ERR_TIMEOUT;

    case PRTE_ERR_WOULD_BLOCK:
        return PMIX_ERR_WOULD_BLOCK;

    case PRTE_EXISTS:
        return PMIX_EXISTS;

    case PRTE_ERR_PARTIAL_SUCCESS:
        return PMIX_QUERY_PARTIAL_SUCCESS;

    case PRTE_ERR_MODEL_DECLARED:
        return PMIX_MODEL_DECLARED;

    case PRTE_ERROR:
        return PMIX_ERROR;
    case PRTE_SUCCESS:
        return PMIX_SUCCESS;

    default:
        return PMIX_ERROR;
    }
}

int prte_pmix_convert_status(pmix_status_t status)
{
    switch (status) {
    case PMIX_ERR_DEBUGGER_RELEASE:
        return PRTE_ERR_DEBUGGER_RELEASE;

    case PMIX_EVENT_ACTION_COMPLETE:
        return PRTE_ERR_HANDLERS_COMPLETE;

    case PMIX_ERR_PROC_ABORTED:
        return PRTE_ERR_PROC_ABORTED;

    case PMIX_ERR_PROC_REQUESTED_ABORT:
        return PRTE_ERR_PROC_REQUESTED_ABORT;

    case PMIX_ERR_PROC_ABORTING:
        return PRTE_ERR_PROC_ABORTING;

    case PMIX_ERR_NODE_DOWN:
        return PRTE_ERR_NODE_DOWN;

    case PMIX_ERR_NODE_OFFLINE:
        return PRTE_ERR_NODE_OFFLINE;

    case PMIX_ERR_JOB_TERMINATED:
        return PRTE_ERR_JOB_TERMINATED;

    case PMIX_ERR_PROC_RESTART:
        return PRTE_ERR_PROC_RESTART;

    case PMIX_ERR_PROC_CHECKPOINT:
        return PRTE_ERR_PROC_CHECKPOINT;

    case PMIX_ERR_PROC_MIGRATE:
        return PRTE_ERR_PROC_MIGRATE;

    case PMIX_ERR_EVENT_REGISTRATION:
        return PRTE_ERR_EVENT_REGISTRATION;

    case PMIX_ERR_NOT_SUPPORTED:
        return PRTE_ERR_NOT_SUPPORTED;

    case PMIX_ERR_NOT_FOUND:
        return PRTE_ERR_NOT_FOUND;

    case PMIX_ERR_OUT_OF_RESOURCE:
        return PRTE_ERR_OUT_OF_RESOURCE;

    case PMIX_ERR_INIT:
        return PRTE_ERROR;

    case PMIX_ERR_BAD_PARAM:
        return PRTE_ERR_BAD_PARAM;

    case PMIX_ERR_UNREACH:
    case PMIX_ERR_NO_PERMISSIONS:
        return PRTE_ERR_UNREACH;

    case PMIX_ERR_TIMEOUT:
        return PRTE_ERR_TIMEOUT;

    case PMIX_ERR_WOULD_BLOCK:
        return PRTE_ERR_WOULD_BLOCK;

    case PMIX_ERR_LOST_CONNECTION_TO_SERVER:
    case PMIX_ERR_LOST_PEER_CONNECTION:
    case PMIX_ERR_LOST_CONNECTION_TO_CLIENT:
        return PRTE_ERR_COMM_FAILURE;

    case PMIX_EXISTS:
        return PRTE_EXISTS;

    case PMIX_QUERY_PARTIAL_SUCCESS:
        return PRTE_ERR_PARTIAL_SUCCESS;

    case PMIX_MONITOR_HEARTBEAT_ALERT:
        return PRTE_ERR_HEARTBEAT_ALERT;

    case PMIX_MONITOR_FILE_ALERT:
        return PRTE_ERR_FILE_ALERT;

    case PMIX_MODEL_DECLARED:
        return PRTE_ERR_MODEL_DECLARED;

    case PMIX_ERROR:
        return PRTE_ERROR;
    case PMIX_SUCCESS:
        return PRTE_SUCCESS;
    default:
        return status;
    }
}

pmix_proc_state_t prte_pmix_convert_state(int state)
{
    switch(state) {
        case 0:
            return PMIX_PROC_STATE_UNDEF;
        case 1:
            return PMIX_PROC_STATE_LAUNCH_UNDERWAY;
        case 2:
            return PMIX_PROC_STATE_RESTART;
        case 3:
            return PMIX_PROC_STATE_TERMINATE;
        case 4:
            return PMIX_PROC_STATE_RUNNING;
        case 5:
            return PMIX_PROC_STATE_CONNECTED;
        case 51:
            return PMIX_PROC_STATE_KILLED_BY_CMD;
        case 52:
            return PMIX_PROC_STATE_ABORTED;
        case 53:
            return PMIX_PROC_STATE_FAILED_TO_START;
        case 54:
            return PMIX_PROC_STATE_ABORTED_BY_SIG;
        case 55:
            return PMIX_PROC_STATE_TERM_WO_SYNC;
        case 56:
            return PMIX_PROC_STATE_COMM_FAILED;
        case 58:
            return PMIX_PROC_STATE_CALLED_ABORT;
        case 59:
            return PMIX_PROC_STATE_MIGRATING;
        case 61:
            return PMIX_PROC_STATE_CANNOT_RESTART;
        case 62:
            return PMIX_PROC_STATE_TERM_NON_ZERO;
        case 63:
            return PMIX_PROC_STATE_FAILED_TO_LAUNCH;
        default:
            return PMIX_PROC_STATE_UNDEF;
    }
}

int prte_pmix_convert_pstate(pmix_proc_state_t state)
{
    switch(state) {
        case PMIX_PROC_STATE_UNDEF:
            return 0;
        case PMIX_PROC_STATE_PREPPED:
        case PMIX_PROC_STATE_LAUNCH_UNDERWAY:
            return 1;
        case PMIX_PROC_STATE_RESTART:
            return 2;
        case PMIX_PROC_STATE_TERMINATE:
            return 3;
        case PMIX_PROC_STATE_RUNNING:
            return 4;
        case PMIX_PROC_STATE_CONNECTED:
            return 5;
        case PMIX_PROC_STATE_UNTERMINATED:
            return 15;
        case PMIX_PROC_STATE_TERMINATED:
            return 20;
        case PMIX_PROC_STATE_KILLED_BY_CMD:
            return 51;
        case PMIX_PROC_STATE_ABORTED:
            return 52;
        case PMIX_PROC_STATE_FAILED_TO_START:
            return 53;
        case PMIX_PROC_STATE_ABORTED_BY_SIG:
            return 54;
        case PMIX_PROC_STATE_TERM_WO_SYNC:
            return 55;
        case PMIX_PROC_STATE_COMM_FAILED:
            return 56;
        case PMIX_PROC_STATE_CALLED_ABORT:
            return 58;
        case PMIX_PROC_STATE_MIGRATING:
            return 60;
        case PMIX_PROC_STATE_CANNOT_RESTART:
            return 61;
        case PMIX_PROC_STATE_TERM_NON_ZERO:
            return 62;
        case PMIX_PROC_STATE_FAILED_TO_LAUNCH:
            return 63;
        default:
            return 0;  // undef
    }
}

pmix_status_t prte_pmix_convert_job_state_to_error(int state)
{
    switch(state) {

#if PMIX_NUMERIC_VERSION >= 0x00040000
        case PRTE_JOB_STATE_ALLOC_FAILED:
            return PMIX_ERR_JOB_ALLOC_FAILED;

        case PRTE_JOB_STATE_MAP_FAILED:
            return PMIX_ERR_JOB_FAILED_TO_MAP;

        case PRTE_JOB_STATE_NEVER_LAUNCHED:
        case PRTE_JOB_STATE_FAILED_TO_LAUNCH:
        case PRTE_JOB_STATE_FAILED_TO_START:
        case PRTE_JOB_STATE_CANNOT_LAUNCH:
            return PMIX_ERR_JOB_FAILED_TO_LAUNCH;

        case PRTE_JOB_STATE_KILLED_BY_CMD:
            return PMIX_ERR_JOB_CANCELLED;

        case PRTE_JOB_STATE_ABORTED:
        case PRTE_JOB_STATE_CALLED_ABORT:
        case PRTE_JOB_STATE_SILENT_ABORT:
            return PMIX_ERR_JOB_ABORTED;

        case PRTE_JOB_STATE_ABORTED_BY_SIG:
            return PMIX_ERR_JOB_ABORTED_BY_SIG;

        case PRTE_JOB_STATE_ABORTED_WO_SYNC:
            return PMIX_ERR_JOB_TERM_WO_SYNC;
#endif

        case PRTE_JOB_STATE_TERMINATED:
            return PMIX_ERR_JOB_TERMINATED;

        default:
            return PMIX_ERROR;
    }
}

pmix_status_t prte_pmix_convert_proc_state_to_error(int state)
{
    switch(state) {
        default:
            return PMIX_ERROR;
    }
}

void prte_pmix_value_load(pmix_value_t *v,
                           prte_value_t *kv)
{
    prte_list_t *list;
    prte_value_t *val;
    pmix_info_t *info;
    size_t n;
    int rc;

    switch(kv->type) {
        case PRTE_UNDEF:
            v->type = PMIX_UNDEF;
            break;
        case PRTE_BOOL:
            v->type = PMIX_BOOL;
            memcpy(&(v->data.flag), &kv->data.flag, 1);
            break;
        case PRTE_BYTE:
            v->type = PMIX_BYTE;
            memcpy(&(v->data.byte), &kv->data.byte, 1);
            break;
        case PRTE_STRING:
            v->type = PMIX_STRING;
            if (NULL != kv->data.string) {
                v->data.string = strdup(kv->data.string);
            } else {
                v->data.string = NULL;
            }
            break;
        case PRTE_SIZE:
            v->type = PMIX_SIZE;
            memcpy(&(v->data.size), &kv->data.size, sizeof(size_t));
            break;
        case PRTE_PID:
            v->type = PMIX_PID;
            memcpy(&(v->data.pid), &kv->data.pid, sizeof(pid_t));
            break;
        case PRTE_INT:
            v->type = PMIX_INT;
            memcpy(&(v->data.integer), &kv->data.integer, sizeof(int));
            break;
        case PRTE_INT8:
            v->type = PMIX_INT8;
            memcpy(&(v->data.int8), &kv->data.int8, 1);
            break;
        case PRTE_INT16:
            v->type = PMIX_INT16;
            memcpy(&(v->data.int16), &kv->data.int16, 2);
            break;
        case PRTE_INT32:
            v->type = PMIX_INT32;
            memcpy(&(v->data.int32), &kv->data.int32, 4);
            break;
        case PRTE_INT64:
            v->type = PMIX_INT64;
            memcpy(&(v->data.int64), &kv->data.int64, 8);
            break;
        case PRTE_UINT:
            v->type = PMIX_UINT;
            memcpy(&(v->data.uint), &kv->data.uint, sizeof(int));
            break;
        case PRTE_UINT8:
            v->type = PMIX_UINT8;
            memcpy(&(v->data.uint8), &kv->data.uint8, 1);
            break;
        case PRTE_UINT16:
            v->type = PMIX_UINT16;
            memcpy(&(v->data.uint16), &kv->data.uint16, 2);
            break;
        case PRTE_UINT32:
            v->type = PMIX_UINT32;
            memcpy(&(v->data.uint32), &kv->data.uint32, 4);
            break;
        case PRTE_UINT64:
            v->type = PMIX_UINT64;
            memcpy(&(v->data.uint64), &kv->data.uint64, 8);
            break;
        case PRTE_FLOAT:
            v->type = PMIX_FLOAT;
            memcpy(&(v->data.fval), &kv->data.fval, sizeof(float));
            break;
        case PRTE_DOUBLE:
            v->type = PMIX_DOUBLE;
            memcpy(&(v->data.dval), &kv->data.dval, sizeof(double));
            break;
        case PRTE_TIMEVAL:
            v->type = PMIX_TIMEVAL;
            memcpy(&(v->data.tv), &kv->data.tv, sizeof(struct timeval));
            break;
        case PRTE_TIME:
            v->type = PMIX_TIME;
            memcpy(&(v->data.time), &kv->data.time, sizeof(time_t));
            break;
        case PRTE_STATUS:
            v->type = PMIX_STATUS;
            v->data.status = prte_pmix_convert_rc(kv->data.status);
            break;
        case PRTE_JOBID:
            v->type = PMIX_PROC;
            /* have to stringify the jobid */
            PMIX_PROC_CREATE(v->data.proc, 1);
            PRTE_PMIX_CONVERT_JOBID(rc, v->data.proc->nspace, kv->data.name.jobid);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
            }
            /* leave the rank as invalid */
            break;
        case PRTE_VPID:
            v->type = PMIX_PROC_RANK;
            PRTE_PMIX_CONVERT_VPID(v->data.rank, kv->data.name.vpid);
            break;
        case PRTE_NAME:
            v->type = PMIX_PROC;
            /* have to stringify the jobid */
            PMIX_PROC_CREATE(v->data.proc, 1);
            PRTE_PMIX_CONVERT_JOBID(rc, v->data.proc->nspace, kv->data.name.jobid);
            PRTE_PMIX_CONVERT_VPID(v->data.proc->rank, kv->data.name.vpid);
            break;
        case PRTE_BYTE_OBJECT:
            v->type = PMIX_BYTE_OBJECT;
            if (NULL != kv->data.bo.bytes) {
                v->data.bo.bytes = (char*)malloc(kv->data.bo.size);
                memcpy(v->data.bo.bytes, kv->data.bo.bytes, kv->data.bo.size);
                v->data.bo.size = (size_t)kv->data.bo.size;
            } else {
                v->data.bo.bytes = NULL;
                v->data.bo.size = 0;
            }
            break;
        case PRTE_PERSIST:
            v->type = PMIX_PERSIST;
            v->data.persist = (pmix_persistence_t)kv->data.uint8;
            break;
        case PRTE_SCOPE:
            v->type = PMIX_SCOPE;
            v->data.scope = (pmix_scope_t)kv->data.uint8;
            break;
        case PRTE_DATA_RANGE:
            v->type = PMIX_DATA_RANGE;
            v->data.range = (pmix_data_range_t)kv->data.uint8;
            break;
        case PRTE_PROC_STATE:
            v->type = PMIX_PROC_STATE;
            /* the PRTE layer doesn't have any concept of proc state,
             * so the PRTE layer is responsible for converting it */
            memcpy(&v->data.state, &kv->data.uint8, sizeof(uint8_t));
            break;
        case PRTE_PTR:
            v->type = PMIX_POINTER;
            v->data.ptr = kv->data.ptr;
            break;
         case PRTE_LIST:
            list = (prte_list_t*)kv->data.ptr;
            v->type = PMIX_DATA_ARRAY;
            v->data.darray = (pmix_data_array_t*)malloc(sizeof(pmix_data_array_t));
            v->data.darray->type = PMIX_INFO;
            v->data.darray->size = (NULL == list)?0:prte_list_get_size(list);
            if (0 < v->data.darray->size) {
                PMIX_INFO_CREATE(info, v->data.darray->size);
                v->data.darray->array = info;
                n=0;
                PRTE_LIST_FOREACH(val, list, prte_value_t) {
                    if (NULL != val->key) {
                        (void)strncpy(info[n].key, val->key, PMIX_MAX_KEYLEN);
                    }
                    prte_pmix_value_load(&info[n].value, val);
                    ++n;
                }
            } else {
                v->data.darray->array = NULL;
            }
            break;
        case PRTE_PROC_INFO:
            v->type = PMIX_PROC_INFO;
            PMIX_PROC_INFO_CREATE(v->data.pinfo, 1);
            PRTE_PMIX_CONVERT_JOBID(rc, v->data.pinfo->proc.nspace, kv->data.pinfo.name.jobid);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
            }
            PRTE_PMIX_CONVERT_VPID(v->data.pinfo->proc.rank, kv->data.pinfo.name.vpid);
            if (NULL != kv->data.pinfo.hostname) {
                v->data.pinfo->hostname = strdup(kv->data.pinfo.hostname);
            }
            if (NULL != kv->data.pinfo.executable_name) {
                v->data.pinfo->executable_name = strdup(kv->data.pinfo.executable_name);
            }
            v->data.pinfo->pid = kv->data.pinfo.pid;
            v->data.pinfo->exit_code = kv->data.pinfo.exit_code;
            v->data.pinfo->state = prte_pmix_convert_state(kv->data.pinfo.state);
            break;
        case PRTE_ENVAR:
            v->type = PMIX_ENVAR;
            PMIX_ENVAR_CONSTRUCT(&v->data.envar);
            if (NULL != kv->data.envar.envar) {
                v->data.envar.envar = strdup(kv->data.envar.envar);
            }
            if (NULL != kv->data.envar.value) {
                v->data.envar.value = strdup(kv->data.envar.value);
            }
            v->data.envar.separator = kv->data.envar.separator;
            break;
        default:
            /* silence warnings */
            break;
    }
}

int prte_pmix_value_unload(prte_value_t *kv,
                           const pmix_value_t *v)
{
    int rc=PRTE_SUCCESS;
    prte_list_t *lt;
    prte_value_t *ival;
    size_t n;

    switch(v->type) {
    case PMIX_UNDEF:
        kv->type = PRTE_UNDEF;
        break;
    case PMIX_BOOL:
        kv->type = PRTE_BOOL;
        memcpy(&kv->data.flag, &(v->data.flag), 1);
        break;
    case PMIX_BYTE:
        kv->type = PRTE_BYTE;
        memcpy(&kv->data.byte, &(v->data.byte), 1);
        break;
    case PMIX_STRING:
        kv->type = PRTE_STRING;
        if (NULL != v->data.string) {
            kv->data.string = strdup(v->data.string);
        }
        break;
    case PMIX_SIZE:
        kv->type = PRTE_SIZE;
        memcpy(&kv->data.size, &(v->data.size), sizeof(size_t));
        break;
    case PMIX_PID:
        kv->type = PRTE_PID;
        memcpy(&kv->data.pid, &(v->data.pid), sizeof(pid_t));
        break;
    case PMIX_INT:
        kv->type = PRTE_INT;
        memcpy(&kv->data.integer, &(v->data.integer), sizeof(int));
        break;
    case PMIX_INT8:
        kv->type = PRTE_INT8;
        memcpy(&kv->data.int8, &(v->data.int8), 1);
        break;
    case PMIX_INT16:
        kv->type = PRTE_INT16;
        memcpy(&kv->data.int16, &(v->data.int16), 2);
        break;
    case PMIX_INT32:
        kv->type = PRTE_INT32;
        memcpy(&kv->data.int32, &(v->data.int32), 4);
        break;
    case PMIX_INT64:
        kv->type = PRTE_INT64;
        memcpy(&kv->data.int64, &(v->data.int64), 8);
        break;
    case PMIX_UINT:
        kv->type = PRTE_UINT;
        memcpy(&kv->data.uint, &(v->data.uint), sizeof(int));
        break;
    case PMIX_UINT8:
        kv->type = PRTE_UINT8;
        memcpy(&kv->data.uint8, &(v->data.uint8), 1);
        break;
    case PMIX_UINT16:
        kv->type = PRTE_UINT16;
        memcpy(&kv->data.uint16, &(v->data.uint16), 2);
        break;
    case PMIX_UINT32:
        kv->type = PRTE_UINT32;
        memcpy(&kv->data.uint32, &(v->data.uint32), 4);
        break;
    case PMIX_UINT64:
        kv->type = PRTE_UINT64;
        memcpy(&kv->data.uint64, &(v->data.uint64), 8);
        break;
    case PMIX_FLOAT:
        kv->type = PRTE_FLOAT;
        memcpy(&kv->data.fval, &(v->data.fval), sizeof(float));
        break;
    case PMIX_DOUBLE:
        kv->type = PRTE_DOUBLE;
        memcpy(&kv->data.dval, &(v->data.dval), sizeof(double));
        break;
    case PMIX_TIMEVAL:
        kv->type = PRTE_TIMEVAL;
        memcpy(&kv->data.tv, &(v->data.tv), sizeof(struct timeval));
        break;
    case PMIX_TIME:
        kv->type = PRTE_TIME;
        memcpy(&kv->data.time, &(v->data.time), sizeof(time_t));
        break;
    case PMIX_STATUS:
        kv->type = PRTE_STATUS;
        kv->data.status = prte_pmix_convert_status(v->data.status);
        break;
    case PMIX_PROC_RANK:
        kv->type = PRTE_VPID;
        PRTE_PMIX_CONVERT_RANK(kv->data.name.vpid, v->data.rank);
        break;
    case PMIX_PROC:
        kv->type = PRTE_NAME;
        PRTE_PMIX_CONVERT_NSPACE(rc, &kv->data.name.jobid, v->data.proc->nspace);
        PRTE_PMIX_CONVERT_RANK(kv->data.name.vpid, v->data.proc->rank);
        break;
    case PMIX_BYTE_OBJECT:
        kv->type = PRTE_BYTE_OBJECT;
        if (NULL != v->data.bo.bytes && 0 < v->data.bo.size) {
            kv->data.bo.bytes = (uint8_t*)malloc(v->data.bo.size);
            memcpy(kv->data.bo.bytes, v->data.bo.bytes, v->data.bo.size);
            kv->data.bo.size = (int)v->data.bo.size;
        } else {
            kv->data.bo.bytes = NULL;
            kv->data.bo.size = 0;
        }
        break;
    case PMIX_PERSIST:
        kv->type = PRTE_PERSIST;
        kv->data.uint8 = v->data.persist;
        break;
    case PMIX_SCOPE:
        kv->type = PRTE_SCOPE;
        kv->data.uint8 = v->data.scope;
        break;
    case PMIX_DATA_RANGE:
        kv->type = PRTE_DATA_RANGE;
        kv->data.uint8 = v->data.range;
        break;
    case PMIX_PROC_STATE:
        kv->type = PRTE_PROC_STATE;
        kv->data.integer = prte_pmix_convert_pstate(v->data.state);
        break;
    case PMIX_POINTER:
        kv->type = PRTE_PTR;
        kv->data.ptr = v->data.ptr;
        break;
    case PMIX_DATA_ARRAY:
        if (NULL == v->data.darray || NULL == v->data.darray->array) {
            kv->data.ptr = NULL;
            break;
        }
        lt = PRTE_NEW(prte_list_t);
        kv->type = PRTE_PTR;
        kv->data.ptr = (void*)lt;
        for (n=0; n < v->data.darray->size; n++) {
            ival = PRTE_NEW(prte_value_t);
            prte_list_append(lt, &ival->super);
            /* handle the various types */
            if (PMIX_INFO == v->data.darray->type) {
                pmix_info_t *iptr = (pmix_info_t*)v->data.darray->array;
                if (0 < strlen(iptr[n].key)) {
                    ival->key = strdup(iptr[n].key);
                }
                rc = prte_pmix_value_unload(ival, &iptr[n].value);
                if (PRTE_SUCCESS != rc) {
                    PRTE_LIST_RELEASE(lt);
                    kv->type = PRTE_UNDEF;
                    kv->data.ptr = NULL;
                    break;
                }
            }
        }
        break;
    case PMIX_PROC_INFO:
        kv->type = PRTE_PROC_INFO;
        if (NULL == v->data.pinfo) {
            rc = PRTE_ERR_BAD_PARAM;
            break;
        }
        PRTE_PMIX_CONVERT_NSPACE(rc, &kv->data.pinfo.name.jobid, v->data.pinfo->proc.nspace);
        PRTE_PMIX_CONVERT_RANK(kv->data.pinfo.name.vpid, v->data.pinfo->proc.rank);
        if (NULL != v->data.pinfo->hostname) {
            kv->data.pinfo.hostname = strdup(v->data.pinfo->hostname);
        }
        if (NULL != v->data.pinfo->executable_name) {
            kv->data.pinfo.executable_name = strdup(v->data.pinfo->executable_name);
        }
        kv->data.pinfo.pid = v->data.pinfo->pid;
        kv->data.pinfo.exit_code = v->data.pinfo->exit_code;
        kv->data.pinfo.state = prte_pmix_convert_pstate(v->data.pinfo->state);
        break;
    case PMIX_ENVAR:
        kv->type = PRTE_ENVAR;
        PRTE_CONSTRUCT(&kv->data.envar, prte_envar_t);
        if (NULL != v->data.envar.envar) {
            kv->data.envar.envar = strdup(v->data.envar.envar);
        }
        if (NULL != v->data.envar.value) {
            kv->data.envar.value = strdup(v->data.envar.value);
        }
        kv->data.envar.separator = v->data.envar.separator;
        break;
    default:
        /* silence warnings */
        rc = PRTE_ERROR;
        break;
    }
    return rc;
}

static void cleanup_cbfunc(pmix_status_t status,
                           pmix_info_t *info, size_t ninfo,
                           void *cbdata,
                           pmix_release_cbfunc_t release_fn,
                           void *release_cbdata)
{
    prte_pmix_lock_t *lk = (prte_pmix_lock_t*)cbdata;

    PRTE_POST_OBJECT(lk);

    /* let the library release the data and cleanup from
     * the operation */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* release the block */
    lk->status = status;
    PRTE_PMIX_WAKEUP_THREAD(lk);
}

int prte_pmix_register_cleanup(char *path, bool directory, bool ignore, bool jobscope)
{
    prte_pmix_lock_t lk;
    pmix_info_t pinfo[3];
    size_t n, ninfo=0;
    pmix_status_t rc, ret;
    pmix_proc_t proc;

    PRTE_PMIX_CONSTRUCT_LOCK(&lk);

    if (ignore) {
        /* they want this path ignored */
        PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_CLEANUP_IGNORE, path, PMIX_STRING);
        ++ninfo;
    } else {
        if (directory) {
            PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_REGISTER_CLEANUP_DIR, path, PMIX_STRING);
            ++ninfo;
            /* recursively cleanup directories */
            PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_CLEANUP_RECURSIVE, NULL, PMIX_BOOL);
            ++ninfo;
        } else {
            /* order cleanup of the provided path */
            PMIX_INFO_LOAD(&pinfo[ninfo], PMIX_REGISTER_CLEANUP, path, PMIX_STRING);
            ++ninfo;
        }
    }

    /* if they want this applied to the job, then indicate so */
    if (jobscope) {
        rc = PMIx_Job_control_nb(NULL, 0, pinfo, ninfo, cleanup_cbfunc, (void*)&lk);
    } else {
        /* only applies to us */
        (void)snprintf(proc.nspace, PMIX_MAX_NSLEN, "%s",
                       PRTE_JOBID_PRINT(PRTE_PROC_MY_NAME->jobid));
        proc.rank = PRTE_PROC_MY_NAME->vpid;
        rc = PMIx_Job_control_nb(&proc, 1, pinfo, ninfo, cleanup_cbfunc, (void*)&lk);
    }
    if (PMIX_SUCCESS != rc) {
        ret = rc;
    } else {
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
        /* There is a bug in PMIx 3.0.0 up to 3.0.2 that causes the callback never
         * being called, so assumes the everything went well and avoid a deadlock. */
        cleanup_cbfunc(PMIX_SUCCESS, NULL, 0, (void *)&lk, NULL, NULL);
#endif
        PRTE_PMIX_WAIT_THREAD(&lk);
        ret = lk.status;
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lk);
    for (n=0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&pinfo[n]);
    }
    return ret;
}


/* CLASS INSTANTIATIONS */
static void dsicon(prte_ds_info_t *p)
{
    PMIX_PROC_CONSTRUCT(&p->source);
    p->info = NULL;
    p->persistence = PMIX_PERSIST_INVALID;
}
PRTE_EXPORT PRTE_CLASS_INSTANCE(prte_ds_info_t,
                                  prte_list_item_t,
                                  dsicon, NULL);

static void infoitmcon(prte_info_item_t *p)
{
    PMIX_INFO_CONSTRUCT(&p->info);
}
static void infoitdecon(prte_info_item_t *p)
{
    PMIX_INFO_DESTRUCT(&p->info);
}
PRTE_EXPORT PRTE_CLASS_INSTANCE(prte_info_item_t,
                                  prte_list_item_t,
                                  infoitmcon, infoitdecon);

static void arritmcon(prte_info_array_item_t *p)
{
    PRTE_CONSTRUCT(&p->infolist, prte_list_t);
}
static void arritdecon(prte_info_array_item_t *p)
{
    PRTE_LIST_DESTRUCT(&p->infolist);
}
PRTE_EXPORT PRTE_CLASS_INSTANCE(prte_info_array_item_t,
                                  prte_list_item_t,
                                  arritmcon, arritdecon);
