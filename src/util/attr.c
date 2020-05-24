/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"
#include "constants.h"

#include "src/dss/dss.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/string_copy.h"

#include "src/mca/errmgr/errmgr.h"

#include "src/util/attr.h"

#define MAX_CONVERTERS 5
#define MAX_CONVERTER_PROJECT_LEN 10

typedef struct {
    int init;
    char project[MAX_CONVERTER_PROJECT_LEN];
    prte_attribute_key_t key_base;
    prte_attribute_key_t key_max;
    prte_attr2str_fn_t converter;
} prte_attr_converter_t;

/* all default to NULL */
static prte_attr_converter_t converters[MAX_CONVERTERS];

bool prte_get_attribute(prte_list_t *attributes,
                        prte_attribute_key_t key,
                        void **data, prte_data_type_t type)
{
    prte_attribute_t *kv;
    int rc;

    PRTE_LIST_FOREACH(kv, attributes, prte_attribute_t) {
        if (key == kv->key) {
            if (kv->type != type) {
                PRTE_ERROR_LOG(PRTE_ERR_TYPE_MISMATCH);
                return false;
            }
            if (NULL != data) {
                if (PRTE_SUCCESS != (rc = prte_attr_unload(kv, data, type))) {
                    PRTE_ERROR_LOG(rc);
                }
            }
            return true;
        }
    }
    /* not found */
    return false;
}

int prte_set_attribute(prte_list_t *attributes,
                       prte_attribute_key_t key, bool local,
                       void *data, prte_data_type_t type)
{
    prte_attribute_t *kv;
    int rc;

    PRTE_LIST_FOREACH(kv, attributes, prte_attribute_t) {
        if (key == kv->key) {
            if (kv->type != type) {
                return PRTE_ERR_TYPE_MISMATCH;
            }
            if (PRTE_SUCCESS != (rc = prte_attr_load(kv, data, type))) {
                PRTE_ERROR_LOG(rc);
            }
            return rc;
        }
    }
    /* not found - add it */
    kv = PRTE_NEW(prte_attribute_t);
    kv->key = key;
    kv->local = local;
    if (PRTE_SUCCESS != (rc = prte_attr_load(kv, data, type))) {
        PRTE_RELEASE(kv);
        return rc;
    }
    prte_list_append(attributes, &kv->super);
    return PRTE_SUCCESS;
}

prte_attribute_t* prte_fetch_attribute(prte_list_t *attributes,
                                       prte_attribute_t *prev,
                                       prte_attribute_key_t key)
{
    prte_attribute_t *kv, *end, *next;

    /* if prev is NULL, then find the first attr on the list
     * that matches the key */
    if (NULL == prev) {
        PRTE_LIST_FOREACH(kv, attributes, prte_attribute_t) {
            if (key == kv->key) {
                return kv;
            }
        }
        /* if we get, then the key isn't on the list */
        return NULL;
    }

    /* if we are at the end of the list, then nothing to do */
    end = (prte_attribute_t*)prte_list_get_end(attributes);
    if (prev == end || end == (prte_attribute_t*)prte_list_get_next(&prev->super) ||
        NULL == prte_list_get_next(&prev->super)) {
        return NULL;
    }

    /* starting with the next item on the list, search
     * for the next attr with the matching key */
    next = (prte_attribute_t*)prte_list_get_next(&prev->super);
    while (NULL != next) {
        if (next->key == key) {
            return next;
        }
        next = (prte_attribute_t*)prte_list_get_next(&next->super);
    }

    /* if we get here, then no matching key was found */
    return NULL;
}

int prte_add_attribute(prte_list_t *attributes,
                       prte_attribute_key_t key, bool local,
                       void *data, prte_data_type_t type)
{
    prte_attribute_t *kv;
    int rc;

    kv = PRTE_NEW(prte_attribute_t);
    kv->key = key;
    kv->local = local;
    if (PRTE_SUCCESS != (rc = prte_attr_load(kv, data, type))) {
        PRTE_RELEASE(kv);
        return rc;
    }
    prte_list_append(attributes, &kv->super);
    return PRTE_SUCCESS;
}

int prte_prepend_attribute(prte_list_t *attributes,
                           prte_attribute_key_t key, bool local,
                           void *data, prte_data_type_t type)
{
    prte_attribute_t *kv;
    int rc;

    kv = PRTE_NEW(prte_attribute_t);
    kv->key = key;
    kv->local = local;
    if (PRTE_SUCCESS != (rc = prte_attr_load(kv, data, type))) {
        PRTE_RELEASE(kv);
        return rc;
    }
    prte_list_prepend(attributes, &kv->super);
    return PRTE_SUCCESS;
}

void prte_remove_attribute(prte_list_t *attributes, prte_attribute_key_t key)
{
    prte_attribute_t *kv;

    PRTE_LIST_FOREACH(kv, attributes, prte_attribute_t) {
        if (key == kv->key) {
            prte_list_remove_item(attributes, &kv->super);
            PRTE_RELEASE(kv);
            return;
        }
    }
}

int prte_attr_register(const char *project,
                       prte_attribute_key_t key_base,
                       prte_attribute_key_t key_max,
                       prte_attr2str_fn_t converter)
{
    int i;

    for (i = 0 ; i < MAX_CONVERTERS ; ++i) {
        if (0 == converters[i].init) {
            converters[i].init = 1;
            prte_string_copy(converters[i].project, project,
                             MAX_CONVERTER_PROJECT_LEN);
            converters[i].project[MAX_CONVERTER_PROJECT_LEN-1] = '\0';
            converters[i].key_base = key_base;
            converters[i].key_max = key_max;
            converters[i].converter = converter;
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_OUT_OF_RESOURCE;
}

char *prte_attr_print_list(prte_list_t *attributes)
{
    char *out1, *out2, *cache = NULL;
    prte_attribute_t *attr;

    PRTE_LIST_FOREACH(attr, attributes, prte_attribute_t) {
        prte_dss.print(&out1, NULL, attr, PRTE_ATTRIBUTE);
        if (NULL == cache) {
            cache = out1;
        } else {
            prte_asprintf(&out2, "%s\n%s", cache, out1);
            free(cache);
            free(out1);
            cache = out2;
        }
    }
    return cache;
}

const char *prte_attr_key_to_str(prte_attribute_key_t key)
{
    int i;

    if (PRTE_ATTR_KEY_BASE < key &&
        key < PRTE_ATTR_KEY_MAX) {
        /* belongs to PRTE, so we handle it */
        switch(key) {
        case PRTE_APP_HOSTFILE:
            return "APP-HOSTFILE";
        case PRTE_APP_ADD_HOSTFILE:
            return "APP-ADD-HOSTFILE";
        case PRTE_APP_DASH_HOST:
            return "APP-DASH-HOST";
        case PRTE_APP_ADD_HOST:
            return "APP-ADD-HOST";
        case PRTE_APP_USER_CWD:
            return "APP-USER-CWD";
        case PRTE_APP_SSNDIR_CWD:
            return "APP-USE-SESSION-DIR-AS-CWD";
        case PRTE_APP_PRELOAD_BIN:
            return "APP-PRELOAD-BIN";
        case PRTE_APP_PRELOAD_FILES:
            return "APP-PRELOAD-FILES";
        case PRTE_APP_SSTORE_LOAD:
            return "APP-SSTORE-LOAD";
        case PRTE_APP_RECOV_DEF:
            return "APP-RECOVERY-DEFINED";
        case PRTE_APP_MAX_RESTARTS:
            return "APP-MAX-RESTARTS";
        case PRTE_APP_MIN_NODES:
            return "APP-MIN-NODES";
        case PRTE_APP_MANDATORY:
            return "APP-NODES-MANDATORY";
        case PRTE_APP_MAX_PPN:
            return "APP-MAX-PPN";
        case PRTE_APP_PREFIX_DIR:
            return "APP-PREFIX-DIR";
        case PRTE_APP_NO_CACHEDIR:
            return "PRTE_APP_NO_CACHEDIR";
        case PRTE_APP_SET_ENVAR:
            return "PRTE_APP_SET_ENVAR";
        case PRTE_APP_UNSET_ENVAR:
            return "PRTE_APP_UNSET_ENVAR";
        case PRTE_APP_PREPEND_ENVAR:
            return "PRTE_APP_PREPEND_ENVAR";
        case PRTE_APP_APPEND_ENVAR:
            return "PRTE_APP_APPEND_ENVAR";
        case PRTE_APP_ADD_ENVAR:
            return "PRTE_APP_ADD_ENVAR";
        case PRTE_APP_DEBUGGER_DAEMON:
            return "PRTE_APP_DEBUGGER_DAEMON";
        case PRTE_APP_PSET_NAME:
            return "PRTE_APP_PSET_NAME";

        case PRTE_NODE_USERNAME:
            return "NODE-USERNAME";
        case PRTE_NODE_PORT:
            return "NODE-PORT";
        case PRTE_NODE_LAUNCH_ID:
            return "NODE-LAUNCHID";
        case PRTE_NODE_HOSTID:
            return "NODE-HOSTID";
        case PRTE_NODE_ALIAS:
            return "NODE-ALIAS";
        case PRTE_NODE_SERIAL_NUMBER:
            return "NODE-SERIAL-NUM";

        case PRTE_JOB_LAUNCH_MSG_SENT:
            return "JOB-LAUNCH-MSG-SENT";
        case PRTE_JOB_LAUNCH_MSG_RECVD:
            return "JOB-LAUNCH-MSG-RECVD";
        case PRTE_JOB_MAX_LAUNCH_MSG_RECVD:
            return "JOB-MAX-LAUNCH-MSG-RECVD";
        case PRTE_JOB_CKPT_STATE:
            return "JOB-CKPT-STATE";
        case PRTE_JOB_SNAPSHOT_REF:
            return "JOB-SNAPSHOT-REF";
        case PRTE_JOB_SNAPSHOT_LOC:
            return "JOB-SNAPSHOT-LOC";
        case PRTE_JOB_SNAPC_INIT_BAR:
            return "JOB-SNAPC-INIT-BARRIER-ID";
        case PRTE_JOB_SNAPC_FINI_BAR:
            return "JOB-SNAPC-FINI-BARRIER-ID";
        case PRTE_JOB_NUM_NONZERO_EXIT:
            return "JOB-NUM-NONZERO-EXIT";
        case PRTE_JOB_FAILURE_TIMER_EVENT:
            return "JOB-FAILURE-TIMER-EVENT";
        case PRTE_JOB_ABORTED_PROC:
            return "JOB-ABORTED-PROC";
        case PRTE_JOB_MAPPER:
            return "JOB-MAPPER";
        case PRTE_JOB_REDUCER:
            return "JOB-REDUCER";
        case PRTE_JOB_COMBINER:
            return "JOB-COMBINER";
        case PRTE_JOB_INDEX_ARGV:
            return "JOB-INDEX-ARGV";
        case PRTE_JOB_NO_VM:
            return "JOB-NO-VM";
        case PRTE_JOB_SPIN_FOR_DEBUG:
            return "JOB-SPIN-FOR-DEBUG";
        case PRTE_JOB_CONTINUOUS_OP:
            return "JOB-CONTINUOUS-OP";
        case PRTE_JOB_RECOVER_DEFINED:
            return "JOB-RECOVERY-DEFINED";
        case PRTE_JOB_NON_PRTE_JOB:
            return "JOB-NON-PRTE-JOB";
        case PRTE_JOB_STDOUT_TARGET:
            return "JOB-STDOUT-TARGET";
        case PRTE_JOB_POWER:
            return "JOB-POWER";
        case PRTE_JOB_MAX_FREQ:
            return "JOB-MAX_FREQ";
        case PRTE_JOB_MIN_FREQ:
            return "JOB-MIN_FREQ";
        case PRTE_JOB_GOVERNOR:
            return "JOB-FREQ-GOVERNOR";
        case PRTE_JOB_FAIL_NOTIFIED:
            return "JOB-FAIL-NOTIFIED";
        case PRTE_JOB_TERM_NOTIFIED:
            return "JOB-TERM-NOTIFIED";
        case PRTE_JOB_PEER_MODX_ID:
            return "JOB-PEER-MODX-ID";
        case PRTE_JOB_INIT_BAR_ID:
            return "JOB-INIT-BAR-ID";
        case PRTE_JOB_FINI_BAR_ID:
            return "JOB-FINI-BAR-ID";
        case PRTE_JOB_FWDIO_TO_TOOL:
            return "JOB-FWD-IO-TO-TOOL";
        case PRTE_JOB_LAUNCHED_DAEMONS:
            return "JOB-LAUNCHED-DAEMONS";
        case PRTE_JOB_REPORT_BINDINGS:
            return "JOB-REPORT-BINDINGS";
        case PRTE_JOB_CPUSET:
            return "JOB-CPUSET";
        case PRTE_JOB_NOTIFICATIONS:
            return "JOB-NOTIFICATIONS";
        case PRTE_JOB_ROOM_NUM:
            return "JOB-ROOM-NUM";
        case PRTE_JOB_LAUNCH_PROXY:
            return "JOB-LAUNCH-PROXY";
        case PRTE_JOB_NSPACE_REGISTERED:
            return "JOB-NSPACE-REGISTERED";
        case PRTE_JOB_FIXED_DVM:
            return "PRTE-JOB-FIXED-DVM";
        case PRTE_JOB_DVM_JOB:
            return "PRTE-JOB-DVM-JOB";
        case PRTE_JOB_CANCELLED:
            return "PRTE-JOB-CANCELLED";
        case PRTE_JOB_OUTPUT_TO_FILE:
            return "PRTE-JOB-OUTPUT-TO-FILE";
        case PRTE_JOB_MERGE_STDERR_STDOUT:
            return "PRTE-JOB-MERGE-STDERR-STDOUT";
        case PRTE_JOB_TAG_OUTPUT:
            return "PRTE-JOB-TAG-OUTPUT";
        case PRTE_JOB_TIMESTAMP_OUTPUT:
            return "PRTE-JOB-TIMESTAMP-OUTPUT";
        case PRTE_JOB_MULTI_DAEMON_SIM:
            return "PRTE_JOB_MULTI_DAEMON_SIM";
        case PRTE_JOB_NOTIFY_COMPLETION:
            return "PRTE_JOB_NOTIFY_COMPLETION";
        case PRTE_JOB_TRANSPORT_KEY:
            return "PRTE_JOB_TRANSPORT_KEY";
        case PRTE_JOB_INFO_CACHE:
            return "PRTE_JOB_INFO_CACHE";
        case PRTE_JOB_FULLY_DESCRIBED:
            return "PRTE_JOB_FULLY_DESCRIBED";
        case PRTE_JOB_SILENT_TERMINATION:
            return "PRTE_JOB_SILENT_TERMINATION";
        case PRTE_JOB_SET_ENVAR:
            return "PRTE_JOB_SET_ENVAR";
        case PRTE_JOB_UNSET_ENVAR:
            return "PRTE_JOB_UNSET_ENVAR";
        case PRTE_JOB_PREPEND_ENVAR:
            return "PRTE_JOB_PREPEND_ENVAR";
        case PRTE_JOB_APPEND_ENVAR:
            return "PRTE_JOB_APPEND_ENVAR";
        case PRTE_JOB_ADD_ENVAR:
            return "PRTE_APP_ADD_ENVAR";
        case PRTE_JOB_APP_SETUP_DATA:
            return "PRTE_JOB_APP_SETUP_DATA";
        case PRTE_JOB_OUTPUT_TO_DIRECTORY:
            return "PRTE_JOB_OUTPUT_TO_DIRECTORY";
        case PRTE_JOB_STOP_ON_EXEC:
            return "JOB_STOP_ON_EXEC";
        case PRTE_JOB_SPAWN_NOTIFIED:
            return "JOB_SPAWN_NOTIFIED";
        case PRTE_JOB_DISPLAY_MAP:
            return "DISPLAY_JOB_MAP";
        case PRTE_JOB_DISPLAY_DEVEL_MAP:
            return "DISPLAY_DEVEL_JOB_MAP";
        case PRTE_JOB_DISPLAY_TOPO:
            return "DISPLAY_TOPOLOGY";
        case PRTE_JOB_DISPLAY_DIFF:
            return "DISPLAY_DIFFABLE";
        case PRTE_JOB_DISPLAY_ALLOC:
            return "DISPLAY_ALLOCATION";
        case PRTE_JOB_DO_NOT_LAUNCH:
            return "DO_NOT_LAUNCH";
        case PRTE_JOB_XML_OUTPUT:
            return "XML_OUTPUT";
        case PRTE_JOB_TIMEOUT:
            return "JOB_TIMEOUT";
        case PRTE_JOB_STACKTRACES:
            return "JOB_STACKTRACES";
        case PRTE_JOB_REPORT_STATE:
            return "JOB_REPORT_STATE";
        case PRTE_JOB_TIMEOUT_EVENT:
            return "JOB_TIMEOUT_EVENT";
        case PRTE_JOB_TRACE_TIMEOUT_EVENT:
            return "JOB_TRACE_TIMEOUT_EVENT";
        case PRTE_JOB_INHERIT:
            return "JOB_INHERIT";
        case PRTE_JOB_PES_PER_PROC:
            return "JOB_PES_PER_PROC";
        case PRTE_JOB_DIST_DEVICE:
            return "JOB_DIST_DEVICE";
        case PRTE_JOB_HWT_CPUS:
            return "JOB_HWT_CPUS";
        case PRTE_JOB_CORE_CPUS:
            return "JOB_CORE_CPUS";
        case PRTE_JOB_PPR:
            return "JOB_PPR";
        case PRTE_JOB_NOINHERIT:
            return "JOB_NOINHERIT";

        case PRTE_PROC_NOBARRIER:
            return "PROC-NOBARRIER";
        case PRTE_PROC_CPU_BITMAP:
            return "PROC-CPU-BITMAP";
        case PRTE_PROC_HWLOC_LOCALE:
            return "PROC-HWLOC-LOCALE";
        case PRTE_PROC_HWLOC_BOUND:
            return "PROC-HWLOC-BOUND";
        case PRTE_PROC_PRIOR_NODE:
            return "PROC-PRIOR-NODE";
        case PRTE_PROC_NRESTARTS:
            return "PROC-NUM-RESTARTS";
        case PRTE_PROC_RESTART_TIME:
            return "PROC-RESTART-TIME";
        case PRTE_PROC_FAST_FAILS:
            return "PROC-FAST-FAILS";
        case PRTE_PROC_CKPT_STATE:
            return "PROC-CKPT-STATE";
        case PRTE_PROC_SNAPSHOT_REF:
            return "PROC-SNAPHOT-REF";
        case PRTE_PROC_SNAPSHOT_LOC:
            return "PROC-SNAPSHOT-LOC";
        case PRTE_PROC_NODENAME:
            return "PROC-NODENAME";
        case PRTE_PROC_CGROUP:
            return "PROC-CGROUP";
        case PRTE_PROC_NBEATS:
            return "PROC-NBEATS";

        case PRTE_RML_TRANSPORT_TYPE:
            return "RML-TRANSPORT-TYPE";
        case PRTE_RML_PROTOCOL_TYPE:
            return "RML-PROTOCOL-TYPE";
        case PRTE_RML_CONDUIT_ID:
            return "RML-CONDUIT-ID";
        case PRTE_RML_INCLUDE_COMP_ATTRIB:
            return "RML-INCLUDE";
        case PRTE_RML_EXCLUDE_COMP_ATTRIB:
            return "RML-EXCLUDE";
        case PRTE_RML_TRANSPORT_ATTRIB:
            return "RML-TRANSPORT";
        case PRTE_RML_QUALIFIER_ATTRIB:
            return "RML-QUALIFIER";
        case PRTE_RML_PROVIDER_ATTRIB:
            return "RML-DESIRED-PROVIDERS";
        case PRTE_RML_PROTOCOL_ATTRIB:
            return "RML-DESIRED-PROTOCOLS";
        case PRTE_RML_ROUTED_ATTRIB:
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


int prte_attr_load(prte_attribute_t *kv,
                   void *data, prte_data_type_t type)
{
    prte_byte_object_t *boptr;
    struct timeval *tv;
    prte_envar_t *envar;

    kv->type = type;
    if (NULL == data) {
        /* if the type is BOOL, then the user wanted to
         * use the presence of the attribute to indicate
         * "true" - so let's mark it that way just in
         * case a subsequent test looks for the value */
        if (PRTE_BOOL == type) {
            kv->data.flag = true;
        } else {
            /* otherwise, check to see if this type has storage
             * that is already allocated, and free it if so */
            if (PRTE_STRING == type && NULL != kv->data.string) {
                free(kv->data.string);
            } else if (PRTE_BYTE_OBJECT == type && NULL != kv->data.bo.bytes) {
                free(kv->data.bo.bytes);
            }
            /* just set the fields to zero */
            memset(&kv->data, 0, sizeof(kv->data));
        }
        return PRTE_SUCCESS;
    }

    switch (type) {
    case PRTE_BOOL:
        kv->data.flag = *(bool*)(data);
        break;
    case PRTE_BYTE:
        kv->data.byte = *(uint8_t*)(data);
        break;
    case PRTE_STRING:
        if (NULL != kv->data.string) {
            free(kv->data.string);
        }
        kv->data.string = strdup( (const char *) data);
        break;
    case PRTE_SIZE:
        kv->data.size = *(size_t*)(data);
        break;
    case PRTE_PID:
        kv->data.pid = *(pid_t*)(data);
        break;

    case PRTE_INT:
        kv->data.integer = *(int*)(data);
        break;
    case PRTE_INT8:
        kv->data.int8 = *(int8_t*)(data);
        break;
    case PRTE_INT16:
        kv->data.int16 = *(int16_t*)(data);
        break;
    case PRTE_INT32:
        kv->data.int32 = *(int32_t*)(data);
        break;
    case PRTE_INT64:
        kv->data.int64 = *(int64_t*)(data);
        break;

    case PRTE_UINT:
        kv->data.uint = *(unsigned int*)(data);
        break;
    case PRTE_UINT8:
        kv->data.uint8 = *(uint8_t*)(data);
        break;
    case PRTE_UINT16:
        kv->data.uint16 = *(uint16_t*)(data);
        break;
    case PRTE_UINT32:
        kv->data.uint32 = *(uint32_t*)data;
        break;
    case PRTE_UINT64:
        kv->data.uint64 = *(uint64_t*)(data);
        break;

    case PRTE_BYTE_OBJECT:
        if (NULL != kv->data.bo.bytes) {
            free(kv->data.bo.bytes);
        }
        boptr = (prte_byte_object_t*)data;
        if (NULL != boptr && NULL != boptr->bytes && 0 < boptr->size) {
            kv->data.bo.bytes = (uint8_t *) malloc(boptr->size);
            memcpy(kv->data.bo.bytes, boptr->bytes, boptr->size);
            kv->data.bo.size = boptr->size;
        } else {
            kv->data.bo.bytes = NULL;
            kv->data.bo.size = 0;
        }
        break;

    case PRTE_FLOAT:
        kv->data.fval = *(float*)(data);
        break;

    case PRTE_TIMEVAL:
        tv = (struct timeval*)data;
        kv->data.tv.tv_sec = tv->tv_sec;
        kv->data.tv.tv_usec = tv->tv_usec;
        break;

    case PRTE_PTR:
        kv->data.ptr = data;
        break;

    case PRTE_VPID:
        kv->data.vpid = *(prte_vpid_t *)data;
        break;

    case PRTE_JOBID:
        kv->data.jobid = *(prte_jobid_t *)data;
        break;

    case PRTE_NAME:
        kv->data.name = *(prte_process_name_t *)data;
        break;

    case PRTE_ENVAR:
        PRTE_CONSTRUCT(&kv->data.envar, prte_envar_t);
        envar = (prte_envar_t*)data;
        if (NULL != envar->envar) {
            kv->data.envar.envar = strdup(envar->envar);
        }
        if (NULL != envar->value) {
            kv->data.envar.value = strdup(envar->value);
        }
        kv->data.envar.separator = envar->separator;
        break;

    default:
        PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
        return PRTE_ERR_NOT_SUPPORTED;
    }
    return PRTE_SUCCESS;
}

int prte_attr_unload(prte_attribute_t *kv,
                     void **data, prte_data_type_t type)
{
    prte_byte_object_t *boptr;
    prte_envar_t *envar;

    if (type != kv->type) {
        return PRTE_ERR_TYPE_MISMATCH;
    }
    if (NULL == data  ||
        (PRTE_STRING != type && PRTE_BYTE_OBJECT != type &&
         PRTE_BUFFER != type && PRTE_PTR != type && NULL == *data)) {
        assert(0);
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    switch (type) {
    case PRTE_BOOL:
        memcpy(*data, &kv->data.flag, sizeof(bool));
        break;
    case PRTE_BYTE:
        memcpy(*data, &kv->data.byte, sizeof(uint8_t));
        break;
    case PRTE_STRING:
        if (NULL != kv->data.string) {
            *data = strdup(kv->data.string);
        } else {
            *data = NULL;
        }
        break;
    case PRTE_SIZE:
        memcpy(*data, &kv->data.size, sizeof(size_t));
        break;
    case PRTE_PID:
        memcpy(*data, &kv->data.pid, sizeof(pid_t));
        break;

    case PRTE_INT:
        memcpy(*data, &kv->data.integer, sizeof(int));
        break;
    case PRTE_INT8:
        memcpy(*data, &kv->data.int8, sizeof(int8_t));
        break;
    case PRTE_INT16:
        memcpy(*data, &kv->data.int16, sizeof(int16_t));
        break;
    case PRTE_INT32:
        memcpy(*data, &kv->data.int32, sizeof(int32_t));
        break;
    case PRTE_INT64:
        memcpy(*data, &kv->data.int64, sizeof(int64_t));
        break;

    case PRTE_UINT:
        memcpy(*data, &kv->data.uint, sizeof(unsigned int));
        break;
    case PRTE_UINT8:
        memcpy(*data, &kv->data.uint8, 1);
        break;
    case PRTE_UINT16:
        memcpy(*data, &kv->data.uint16, 2);
        break;
    case PRTE_UINT32:
        memcpy(*data, &kv->data.uint32, 4);
        break;
    case PRTE_UINT64:
        memcpy(*data, &kv->data.uint64, 8);
        break;

    case PRTE_BYTE_OBJECT:
        boptr = (prte_byte_object_t*)malloc(sizeof(prte_byte_object_t));
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

    case PRTE_BUFFER:
        *data = PRTE_NEW(prte_buffer_t);
        prte_dss.copy_payload(*data, &kv->data.buf);
        break;

    case PRTE_FLOAT:
        memcpy(*data, &kv->data.fval, sizeof(float));
        break;

    case PRTE_TIMEVAL:
        memcpy(*data, &kv->data.tv, sizeof(struct timeval));
        break;

    case PRTE_PTR:
        *data = kv->data.ptr;
        break;

    case PRTE_VPID:
        memcpy(*data, &kv->data.vpid, sizeof(prte_vpid_t));
        break;

    case PRTE_JOBID:
        memcpy(*data, &kv->data.jobid, sizeof(prte_jobid_t));
        break;

    case PRTE_NAME:
        memcpy(*data, &kv->data.name, sizeof(prte_process_name_t));
        break;

    case PRTE_ENVAR:
        envar = PRTE_NEW(prte_envar_t);
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
        PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
        return PRTE_ERR_NOT_SUPPORTED;
    }
    return PRTE_SUCCESS;
}
