/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "types.h"
#include "constants.h"

#include "src/dss/dss.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"

#include "src/mca/errmgr/errmgr.h"

#include "src/util/attr.h"

#define MAX_CONVERTERS 5
#define MAX_CONVERTER_PROJECT_LEN 10

typedef struct {
    int init;
    char project[MAX_CONVERTER_PROJECT_LEN];
    prrte_attribute_key_t key_base;
    prrte_attribute_key_t key_max;
    prrte_attr2str_fn_t converter;
} prrte_attr_converter_t;

/* all default to NULL */
static prrte_attr_converter_t converters[MAX_CONVERTERS];

bool prrte_get_attribute(prrte_list_t *attributes,
                        prrte_attribute_key_t key,
                        void **data, prrte_data_type_t type)
{
    prrte_attribute_t *kv;
    int rc;

    PRRTE_LIST_FOREACH(kv, attributes, prrte_attribute_t) {
        if (key == kv->key) {
            if (kv->type != type) {
                PRRTE_ERROR_LOG(PRRTE_ERR_TYPE_MISMATCH);
                return false;
            }
            if (NULL != data) {
                if (PRRTE_SUCCESS != (rc = prrte_attr_unload(kv, data, type))) {
                    PRRTE_ERROR_LOG(rc);
                }
            }
            return true;
        }
    }
    /* not found */
    return false;
}

int prrte_set_attribute(prrte_list_t *attributes,
                       prrte_attribute_key_t key, bool local,
                       void *data, prrte_data_type_t type)
{
    prrte_attribute_t *kv;
    int rc;

    PRRTE_LIST_FOREACH(kv, attributes, prrte_attribute_t) {
        if (key == kv->key) {
            if (kv->type != type) {
                return PRRTE_ERR_TYPE_MISMATCH;
            }
            if (PRRTE_SUCCESS != (rc = prrte_attr_load(kv, data, type))) {
                PRRTE_ERROR_LOG(rc);
            }
            return rc;
        }
    }
    /* not found - add it */
    kv = PRRTE_NEW(prrte_attribute_t);
    kv->key = key;
    kv->local = local;
    if (PRRTE_SUCCESS != (rc = prrte_attr_load(kv, data, type))) {
        PRRTE_RELEASE(kv);
        return rc;
    }
    prrte_list_append(attributes, &kv->super);
    return PRRTE_SUCCESS;
}

prrte_attribute_t* prrte_fetch_attribute(prrte_list_t *attributes,
                                       prrte_attribute_t *prev,
                                       prrte_attribute_key_t key)
{
    prrte_attribute_t *kv, *end, *next;

    /* if prev is NULL, then find the first attr on the list
     * that matches the key */
    if (NULL == prev) {
        PRRTE_LIST_FOREACH(kv, attributes, prrte_attribute_t) {
            if (key == kv->key) {
                return kv;
            }
        }
        /* if we get, then the key isn't on the list */
        return NULL;
    }

    /* if we are at the end of the list, then nothing to do */
    end = (prrte_attribute_t*)prrte_list_get_end(attributes);
    if (prev == end || end == (prrte_attribute_t*)prrte_list_get_next(&prev->super) ||
        NULL == prrte_list_get_next(&prev->super)) {
        return NULL;
    }

    /* starting with the next item on the list, search
     * for the next attr with the matching key */
    next = (prrte_attribute_t*)prrte_list_get_next(&prev->super);
    while (NULL != next) {
        if (next->key == key) {
            return next;
        }
        next = (prrte_attribute_t*)prrte_list_get_next(&next->super);
    }

    /* if we get here, then no matching key was found */
    return NULL;
}

int prrte_add_attribute(prrte_list_t *attributes,
                       prrte_attribute_key_t key, bool local,
                       void *data, prrte_data_type_t type)
{
    prrte_attribute_t *kv;
    int rc;

    kv = PRRTE_NEW(prrte_attribute_t);
    kv->key = key;
    kv->local = local;
    if (PRRTE_SUCCESS != (rc = prrte_attr_load(kv, data, type))) {
        PRRTE_RELEASE(kv);
        return rc;
    }
    prrte_list_append(attributes, &kv->super);
    return PRRTE_SUCCESS;
}

int prrte_prepend_attribute(prrte_list_t *attributes,
                           prrte_attribute_key_t key, bool local,
                           void *data, prrte_data_type_t type)
{
    prrte_attribute_t *kv;
    int rc;

    kv = PRRTE_NEW(prrte_attribute_t);
    kv->key = key;
    kv->local = local;
    if (PRRTE_SUCCESS != (rc = prrte_attr_load(kv, data, type))) {
        PRRTE_RELEASE(kv);
        return rc;
    }
    prrte_list_prepend(attributes, &kv->super);
    return PRRTE_SUCCESS;
}

void prrte_remove_attribute(prrte_list_t *attributes, prrte_attribute_key_t key)
{
    prrte_attribute_t *kv;

    PRRTE_LIST_FOREACH(kv, attributes, prrte_attribute_t) {
        if (key == kv->key) {
            prrte_list_remove_item(attributes, &kv->super);
            PRRTE_RELEASE(kv);
            return;
        }
    }
}

int prrte_attr_register(const char *project,
                       prrte_attribute_key_t key_base,
                       prrte_attribute_key_t key_max,
                       prrte_attr2str_fn_t converter)
{
    int i;

    for (i = 0 ; i < MAX_CONVERTERS ; ++i) {
        if (0 == converters[i].init) {
            converters[i].init = 1;
            prrte_string_copy(converters[i].project, project,
                             MAX_CONVERTER_PROJECT_LEN);
            converters[i].project[MAX_CONVERTER_PROJECT_LEN-1] = '\0';
            converters[i].key_base = key_base;
            converters[i].key_max = key_max;
            converters[i].converter = converter;
            return PRRTE_SUCCESS;
        }
    }

    return PRRTE_ERR_OUT_OF_RESOURCE;
}

const char *prrte_attr_key_to_str(prrte_attribute_key_t key)
{
    int i;

    if (PRRTE_ATTR_KEY_BASE < key &&
        key < PRRTE_ATTR_KEY_MAX) {
        /* belongs to PRRTE, so we handle it */
        switch(key) {
        case PRRTE_APP_HOSTFILE:
            return "APP-HOSTFILE";
        case PRRTE_APP_ADD_HOSTFILE:
            return "APP-ADD-HOSTFILE";
        case PRRTE_APP_DASH_HOST:
            return "APP-DASH-HOST";
        case PRRTE_APP_ADD_HOST:
            return "APP-ADD-HOST";
        case PRRTE_APP_USER_CWD:
            return "APP-USER-CWD";
        case PRRTE_APP_SSNDIR_CWD:
            return "APP-USE-SESSION-DIR-AS-CWD";
        case PRRTE_APP_PRELOAD_BIN:
            return "APP-PRELOAD-BIN";
        case PRRTE_APP_PRELOAD_FILES:
            return "APP-PRELOAD-FILES";
        case PRRTE_APP_SSTORE_LOAD:
            return "APP-SSTORE-LOAD";
        case PRRTE_APP_RECOV_DEF:
            return "APP-RECOVERY-DEFINED";
        case PRRTE_APP_MAX_RESTARTS:
            return "APP-MAX-RESTARTS";
        case PRRTE_APP_MIN_NODES:
            return "APP-MIN-NODES";
        case PRRTE_APP_MANDATORY:
            return "APP-NODES-MANDATORY";
        case PRRTE_APP_MAX_PPN:
            return "APP-MAX-PPN";
        case PRRTE_APP_PREFIX_DIR:
            return "APP-PREFIX-DIR";
        case PRRTE_APP_NO_CACHEDIR:
            return "PRRTE_APP_NO_CACHEDIR";
        case PRRTE_APP_SET_ENVAR:
            return "PRRTE_APP_SET_ENVAR";
        case PRRTE_APP_UNSET_ENVAR:
            return "PRRTE_APP_UNSET_ENVAR";
        case PRRTE_APP_PREPEND_ENVAR:
            return "PRRTE_APP_PREPEND_ENVAR";
        case PRRTE_APP_APPEND_ENVAR:
            return "PRRTE_APP_APPEND_ENVAR";
        case PRRTE_APP_ADD_ENVAR:
            return "PRRTE_APP_ADD_ENVAR";
        case PRRTE_APP_DEBUGGER_DAEMON:
            return "PRRTE_APP_DEBUGGER_DAEMON";
        case PRRTE_APP_PSET_NAME:
            return "PRRTE_APP_PSET_NAME";

        case PRRTE_NODE_USERNAME:
            return "NODE-USERNAME";
        case PRRTE_NODE_PORT:
            return "NODE-PORT";
        case PRRTE_NODE_LAUNCH_ID:
            return "NODE-LAUNCHID";
        case PRRTE_NODE_HOSTID:
            return "NODE-HOSTID";
        case PRRTE_NODE_ALIAS:
            return "NODE-ALIAS";
        case PRRTE_NODE_SERIAL_NUMBER:
            return "NODE-SERIAL-NUM";

        case PRRTE_JOB_LAUNCH_MSG_SENT:
            return "JOB-LAUNCH-MSG-SENT";
        case PRRTE_JOB_LAUNCH_MSG_RECVD:
            return "JOB-LAUNCH-MSG-RECVD";
        case PRRTE_JOB_MAX_LAUNCH_MSG_RECVD:
            return "JOB-MAX-LAUNCH-MSG-RECVD";
        case PRRTE_JOB_CKPT_STATE:
            return "JOB-CKPT-STATE";
        case PRRTE_JOB_SNAPSHOT_REF:
            return "JOB-SNAPSHOT-REF";
        case PRRTE_JOB_SNAPSHOT_LOC:
            return "JOB-SNAPSHOT-LOC";
        case PRRTE_JOB_SNAPC_INIT_BAR:
            return "JOB-SNAPC-INIT-BARRIER-ID";
        case PRRTE_JOB_SNAPC_FINI_BAR:
            return "JOB-SNAPC-FINI-BARRIER-ID";
        case PRRTE_JOB_NUM_NONZERO_EXIT:
            return "JOB-NUM-NONZERO-EXIT";
        case PRRTE_JOB_FAILURE_TIMER_EVENT:
            return "JOB-FAILURE-TIMER-EVENT";
        case PRRTE_JOB_ABORTED_PROC:
            return "JOB-ABORTED-PROC";
        case PRRTE_JOB_MAPPER:
            return "JOB-MAPPER";
        case PRRTE_JOB_REDUCER:
            return "JOB-REDUCER";
        case PRRTE_JOB_COMBINER:
            return "JOB-COMBINER";
        case PRRTE_JOB_INDEX_ARGV:
            return "JOB-INDEX-ARGV";
        case PRRTE_JOB_NO_VM:
            return "JOB-NO-VM";
        case PRRTE_JOB_SPIN_FOR_DEBUG:
            return "JOB-SPIN-FOR-DEBUG";
        case PRRTE_JOB_CONTINUOUS_OP:
            return "JOB-CONTINUOUS-OP";
        case PRRTE_JOB_RECOVER_DEFINED:
            return "JOB-RECOVERY-DEFINED";
        case PRRTE_JOB_NON_PRRTE_JOB:
            return "JOB-NON-PRRTE-JOB";
        case PRRTE_JOB_STDOUT_TARGET:
            return "JOB-STDOUT-TARGET";
        case PRRTE_JOB_POWER:
            return "JOB-POWER";
        case PRRTE_JOB_MAX_FREQ:
            return "JOB-MAX_FREQ";
        case PRRTE_JOB_MIN_FREQ:
            return "JOB-MIN_FREQ";
        case PRRTE_JOB_GOVERNOR:
            return "JOB-FREQ-GOVERNOR";
        case PRRTE_JOB_FAIL_NOTIFIED:
            return "JOB-FAIL-NOTIFIED";
        case PRRTE_JOB_TERM_NOTIFIED:
            return "JOB-TERM-NOTIFIED";
        case PRRTE_JOB_PEER_MODX_ID:
            return "JOB-PEER-MODX-ID";
        case PRRTE_JOB_INIT_BAR_ID:
            return "JOB-INIT-BAR-ID";
        case PRRTE_JOB_FINI_BAR_ID:
            return "JOB-FINI-BAR-ID";
        case PRRTE_JOB_FWDIO_TO_TOOL:
            return "JOB-FWD-IO-TO-TOOL";
        case PRRTE_JOB_PHYSICAL_CPUIDS:
            return "JOB-PHYSICAL-CPUIDS";
        case PRRTE_JOB_LAUNCHED_DAEMONS:
            return "JOB-LAUNCHED-DAEMONS";
        case PRRTE_JOB_REPORT_BINDINGS:
            return "JOB-REPORT-BINDINGS";
        case PRRTE_JOB_CPU_LIST:
            return "JOB-CPU-LIST";
        case PRRTE_JOB_NOTIFICATIONS:
            return "JOB-NOTIFICATIONS";
        case PRRTE_JOB_ROOM_NUM:
            return "JOB-ROOM-NUM";
        case PRRTE_JOB_LAUNCH_PROXY:
            return "JOB-LAUNCH-PROXY";
        case PRRTE_JOB_NSPACE_REGISTERED:
            return "JOB-NSPACE-REGISTERED";
        case PRRTE_JOB_FIXED_DVM:
            return "PRRTE-JOB-FIXED-DVM";
        case PRRTE_JOB_DVM_JOB:
            return "PRRTE-JOB-DVM-JOB";
        case PRRTE_JOB_CANCELLED:
            return "PRRTE-JOB-CANCELLED";
        case PRRTE_JOB_OUTPUT_TO_FILE:
            return "PRRTE-JOB-OUTPUT-TO-FILE";
        case PRRTE_JOB_MERGE_STDERR_STDOUT:
            return "PRRTE-JOB-MERGE-STDERR-STDOUT";
        case PRRTE_JOB_TAG_OUTPUT:
            return "PRRTE-JOB-TAG-OUTPUT";
        case PRRTE_JOB_TIMESTAMP_OUTPUT:
            return "PRRTE-JOB-TIMESTAMP-OUTPUT";
        case PRRTE_JOB_MULTI_DAEMON_SIM:
            return "PRRTE_JOB_MULTI_DAEMON_SIM";
        case PRRTE_JOB_NOTIFY_COMPLETION:
            return "PRRTE_JOB_NOTIFY_COMPLETION";
        case PRRTE_JOB_TRANSPORT_KEY:
            return "PRRTE_JOB_TRANSPORT_KEY";
        case PRRTE_JOB_INFO_CACHE:
            return "PRRTE_JOB_INFO_CACHE";
        case PRRTE_JOB_FULLY_DESCRIBED:
            return "PRRTE_JOB_FULLY_DESCRIBED";
        case PRRTE_JOB_SILENT_TERMINATION:
            return "PRRTE_JOB_SILENT_TERMINATION";
        case PRRTE_JOB_SET_ENVAR:
            return "PRRTE_JOB_SET_ENVAR";
        case PRRTE_JOB_UNSET_ENVAR:
            return "PRRTE_JOB_UNSET_ENVAR";
        case PRRTE_JOB_PREPEND_ENVAR:
            return "PRRTE_JOB_PREPEND_ENVAR";
        case PRRTE_JOB_APPEND_ENVAR:
            return "PRRTE_JOB_APPEND_ENVAR";
        case PRRTE_JOB_ADD_ENVAR:
            return "PRRTE_APP_ADD_ENVAR";
        case PRRTE_JOB_APP_SETUP_DATA:
            return "PRRTE_JOB_APP_SETUP_DATA";
        case PRRTE_JOB_OUTPUT_TO_DIRECTORY:
            return "PRRTE_JOB_OUTPUT_TO_DIRECTORY";
        case PRRTE_JOB_STOP_ON_EXEC:
            return "JOB_STOP_ON_EXEC";
        case PRRTE_JOB_SPAWN_NOTIFIED:
            return "JOB_SPAWN_NOTIFIED";

        case PRRTE_PROC_NOBARRIER:
            return "PROC-NOBARRIER";
        case PRRTE_PROC_CPU_BITMAP:
            return "PROC-CPU-BITMAP";
        case PRRTE_PROC_HWLOC_LOCALE:
            return "PROC-HWLOC-LOCALE";
        case PRRTE_PROC_HWLOC_BOUND:
            return "PROC-HWLOC-BOUND";
        case PRRTE_PROC_PRIOR_NODE:
            return "PROC-PRIOR-NODE";
        case PRRTE_PROC_NRESTARTS:
            return "PROC-NUM-RESTARTS";
        case PRRTE_PROC_RESTART_TIME:
            return "PROC-RESTART-TIME";
        case PRRTE_PROC_FAST_FAILS:
            return "PROC-FAST-FAILS";
        case PRRTE_PROC_CKPT_STATE:
            return "PROC-CKPT-STATE";
        case PRRTE_PROC_SNAPSHOT_REF:
            return "PROC-SNAPHOT-REF";
        case PRRTE_PROC_SNAPSHOT_LOC:
            return "PROC-SNAPSHOT-LOC";
        case PRRTE_PROC_NODENAME:
            return "PROC-NODENAME";
        case PRRTE_PROC_CGROUP:
            return "PROC-CGROUP";
        case PRRTE_PROC_NBEATS:
            return "PROC-NBEATS";

        case PRRTE_RML_TRANSPORT_TYPE:
            return "RML-TRANSPORT-TYPE";
        case PRRTE_RML_PROTOCOL_TYPE:
            return "RML-PROTOCOL-TYPE";
        case PRRTE_RML_CONDUIT_ID:
            return "RML-CONDUIT-ID";
        case PRRTE_RML_INCLUDE_COMP_ATTRIB:
            return "RML-INCLUDE";
        case PRRTE_RML_EXCLUDE_COMP_ATTRIB:
            return "RML-EXCLUDE";
        case PRRTE_RML_TRANSPORT_ATTRIB:
            return "RML-TRANSPORT";
        case PRRTE_RML_QUALIFIER_ATTRIB:
            return "RML-QUALIFIER";
        case PRRTE_RML_PROVIDER_ATTRIB:
            return "RML-DESIRED-PROVIDERS";
        case PRRTE_RML_PROTOCOL_ATTRIB:
            return "RML-DESIRED-PROTOCOLS";
        case PRRTE_RML_ROUTED_ATTRIB:
            return "RML-DESIRED-ROUTED-MODULES";
        default:
            return "UNKNOWN-KEY";
        }
    }

    /* see if one of the converters can handle it */
    for (i = 0 ; i < MAX_CONVERTERS ; ++i) {
        if (0 != converters[i].init) {
            if (converters[i].key_base < key &&
                key < converters[i].key_max) {
                return converters[i].converter(key);
            }
        }
    }

    /* get here if nobody know what to do */
    return "UNKNOWN-KEY";
}


int prrte_attr_load(prrte_attribute_t *kv,
                   void *data, prrte_data_type_t type)
{
    prrte_byte_object_t *boptr;
    struct timeval *tv;
    prrte_envar_t *envar;

    kv->type = type;
    if (NULL == data) {
        /* if the type is BOOL, then the user wanted to
         * use the presence of the attribute to indicate
         * "true" - so let's mark it that way just in
         * case a subsequent test looks for the value */
        if (PRRTE_BOOL == type) {
            kv->data.flag = true;
        } else {
            /* otherwise, check to see if this type has storage
             * that is already allocated, and free it if so */
            if (PRRTE_STRING == type && NULL != kv->data.string) {
                free(kv->data.string);
            } else if (PRRTE_BYTE_OBJECT == type && NULL != kv->data.bo.bytes) {
                free(kv->data.bo.bytes);
            }
            /* just set the fields to zero */
            memset(&kv->data, 0, sizeof(kv->data));
        }
        return PRRTE_SUCCESS;
    }

    switch (type) {
    case PRRTE_BOOL:
        kv->data.flag = *(bool*)(data);
        break;
    case PRRTE_BYTE:
        kv->data.byte = *(uint8_t*)(data);
        break;
    case PRRTE_STRING:
        if (NULL != kv->data.string) {
            free(kv->data.string);
        }
        if (NULL != data) {
            kv->data.string = strdup( (const char *) data);
        } else {
            kv->data.string = NULL;
        }
        break;
    case PRRTE_SIZE:
        kv->data.size = *(size_t*)(data);
        break;
    case PRRTE_PID:
        kv->data.pid = *(pid_t*)(data);
        break;

    case PRRTE_INT:
        kv->data.integer = *(int*)(data);
        break;
    case PRRTE_INT8:
        kv->data.int8 = *(int8_t*)(data);
        break;
    case PRRTE_INT16:
        kv->data.int16 = *(int16_t*)(data);
        break;
    case PRRTE_INT32:
        kv->data.int32 = *(int32_t*)(data);
        break;
    case PRRTE_INT64:
        kv->data.int64 = *(int64_t*)(data);
        break;

    case PRRTE_UINT:
        kv->data.uint = *(unsigned int*)(data);
        break;
    case PRRTE_UINT8:
        kv->data.uint8 = *(uint8_t*)(data);
        break;
    case PRRTE_UINT16:
        kv->data.uint16 = *(uint16_t*)(data);
        break;
    case PRRTE_UINT32:
        kv->data.uint32 = *(uint32_t*)data;
        break;
    case PRRTE_UINT64:
        kv->data.uint64 = *(uint64_t*)(data);
        break;

    case PRRTE_BYTE_OBJECT:
        if (NULL != kv->data.bo.bytes) {
            free(kv->data.bo.bytes);
        }
        boptr = (prrte_byte_object_t*)data;
        if (NULL != boptr && NULL != boptr->bytes && 0 < boptr->size) {
            kv->data.bo.bytes = (uint8_t *) malloc(boptr->size);
            memcpy(kv->data.bo.bytes, boptr->bytes, boptr->size);
            kv->data.bo.size = boptr->size;
        } else {
            kv->data.bo.bytes = NULL;
            kv->data.bo.size = 0;
        }
        break;

    case PRRTE_FLOAT:
        kv->data.fval = *(float*)(data);
        break;

    case PRRTE_TIMEVAL:
        tv = (struct timeval*)data;
        kv->data.tv.tv_sec = tv->tv_sec;
        kv->data.tv.tv_usec = tv->tv_usec;
        break;

    case PRRTE_PTR:
        kv->data.ptr = data;
        break;

    case PRRTE_VPID:
        kv->data.vpid = *(prrte_vpid_t *)data;
        break;

    case PRRTE_JOBID:
        kv->data.jobid = *(prrte_jobid_t *)data;
        break;

    case PRRTE_NAME:
        kv->data.name = *(prrte_process_name_t *)data;
        break;

    case PRRTE_ENVAR:
        PRRTE_CONSTRUCT(&kv->data.envar, prrte_envar_t);
        envar = (prrte_envar_t*)data;
        if (NULL != envar->envar) {
            kv->data.envar.envar = strdup(envar->envar);
        }
        if (NULL != envar->value) {
            kv->data.envar.value = strdup(envar->value);
        }
        kv->data.envar.separator = envar->separator;
        break;

    default:
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_SUPPORTED);
        return PRRTE_ERR_NOT_SUPPORTED;
    }
    return PRRTE_SUCCESS;
}

int prrte_attr_unload(prrte_attribute_t *kv,
                     void **data, prrte_data_type_t type)
{
    prrte_byte_object_t *boptr;
    prrte_envar_t *envar;

    if (type != kv->type) {
        return PRRTE_ERR_TYPE_MISMATCH;
    }
    if (NULL == data  ||
        (PRRTE_STRING != type && PRRTE_BYTE_OBJECT != type &&
         PRRTE_BUFFER != type && PRRTE_PTR != type && NULL == *data)) {
        assert(0);
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    switch (type) {
    case PRRTE_BOOL:
        memcpy(*data, &kv->data.flag, sizeof(bool));
        break;
    case PRRTE_BYTE:
        memcpy(*data, &kv->data.byte, sizeof(uint8_t));
        break;
    case PRRTE_STRING:
        if (NULL != kv->data.string) {
            *data = strdup(kv->data.string);
        } else {
            *data = NULL;
        }
        break;
    case PRRTE_SIZE:
        memcpy(*data, &kv->data.size, sizeof(size_t));
        break;
    case PRRTE_PID:
        memcpy(*data, &kv->data.pid, sizeof(pid_t));
        break;

    case PRRTE_INT:
        memcpy(*data, &kv->data.integer, sizeof(int));
        break;
    case PRRTE_INT8:
        memcpy(*data, &kv->data.int8, sizeof(int8_t));
        break;
    case PRRTE_INT16:
        memcpy(*data, &kv->data.int16, sizeof(int16_t));
        break;
    case PRRTE_INT32:
        memcpy(*data, &kv->data.int32, sizeof(int32_t));
        break;
    case PRRTE_INT64:
        memcpy(*data, &kv->data.int64, sizeof(int64_t));
        break;

    case PRRTE_UINT:
        memcpy(*data, &kv->data.uint, sizeof(unsigned int));
        break;
    case PRRTE_UINT8:
        memcpy(*data, &kv->data.uint8, 1);
        break;
    case PRRTE_UINT16:
        memcpy(*data, &kv->data.uint16, 2);
        break;
    case PRRTE_UINT32:
        memcpy(*data, &kv->data.uint32, 4);
        break;
    case PRRTE_UINT64:
        memcpy(*data, &kv->data.uint64, 8);
        break;

    case PRRTE_BYTE_OBJECT:
        boptr = (prrte_byte_object_t*)malloc(sizeof(prrte_byte_object_t));
        if (NULL != kv->data.bo.bytes && 0 < kv->data.bo.size) {
            boptr->bytes = (uint8_t *) malloc(kv->data.bo.size);
            memcpy(boptr->bytes, kv->data.bo.bytes, kv->data.bo.size);
            boptr->size = kv->data.bo.size;
        } else {
            boptr->bytes = NULL;
            boptr->size = 0;
        }
        *data = boptr;
        break;

    case PRRTE_BUFFER:
        *data = PRRTE_NEW(prrte_buffer_t);
        prrte_dss.copy_payload(*data, &kv->data.buf);
        break;

    case PRRTE_FLOAT:
        memcpy(*data, &kv->data.fval, sizeof(float));
        break;

    case PRRTE_TIMEVAL:
        memcpy(*data, &kv->data.tv, sizeof(struct timeval));
        break;

    case PRRTE_PTR:
        *data = kv->data.ptr;
        break;

    case PRRTE_VPID:
        memcpy(*data, &kv->data.vpid, sizeof(prrte_vpid_t));
        break;

    case PRRTE_JOBID:
        memcpy(*data, &kv->data.jobid, sizeof(prrte_jobid_t));
        break;

    case PRRTE_NAME:
        memcpy(*data, &kv->data.name, sizeof(prrte_process_name_t));
        break;

    case PRRTE_ENVAR:
        envar = PRRTE_NEW(prrte_envar_t);
        if (NULL != kv->data.envar.envar) {
            envar->envar = strdup(kv->data.envar.envar);
        }
        if (NULL != kv->data.envar.value) {
            envar->value = strdup(kv->data.envar.value);
        }
        envar->separator = kv->data.envar.separator;
        *data = envar;
        break;

    default:
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_SUPPORTED);
        return PRRTE_ERR_NOT_SUPPORTED;
    }
    return PRRTE_SUCCESS;
}
