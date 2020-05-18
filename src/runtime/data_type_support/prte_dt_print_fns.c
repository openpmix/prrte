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
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include <sys/types.h>

#include "src/util/argv.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/rmaps/base/base.h"
#include "src/dss/dss.h"
#include "src/util/name_fns.h"
#include "src/util/error_strings.h"
#include "src/runtime/prte_globals.h"

#include "src/runtime/data_type_support/prte_dt_support.h"

static void prte_dt_quick_print(char **output, char *type_name, char *prefix, void *src, prte_data_type_t real_type)
{
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    uint8_t *ui8;
    uint16_t *ui16;
    uint32_t *ui32;
    uint64_t *ui64;

    /* set default result */
    *output = NULL;

    /* check for NULL ptr */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: %s\tData size: 8-bit\tValue: NULL pointer",
                 (NULL == prefix) ? "" : prefix, type_name);
        return;
    }

    switch(real_type) {
        case PRTE_INT8:
            i8 = (int8_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 8-bit\tValue: %d",
                     (NULL == prefix) ? "" : prefix, type_name, (int) *i8);
            break;

        case PRTE_UINT8:
            ui8 = (uint8_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 8-bit\tValue: %u",
                     (NULL == prefix) ? "" : prefix, type_name, (unsigned int)*ui8);
            break;

        case PRTE_INT16:
            i16 = (int16_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 16-bit\tValue: %d",
                     (NULL == prefix) ? "" : prefix, type_name, (int) *i16);
            break;

        case PRTE_UINT16:
            ui16 = (uint16_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 16-bit\tValue: %u",
                     (NULL == prefix) ? "" : prefix, type_name, (unsigned int) *ui16);
            break;

        case PRTE_INT32:
            i32 = (int32_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 32-bit\tValue: %ld",
                     (NULL == prefix) ? "" : prefix, type_name, (long) *i32);
            break;

        case PRTE_UINT32:
            ui32 = (uint32_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 32-bit\tValue: %lu",
                     (NULL == prefix) ? "" : prefix, type_name, (unsigned long) *ui32);
            break;

        case PRTE_INT64:
            i64 = (int64_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 64-bit\tValue: %ld",
                     (NULL == prefix) ? "" : prefix, type_name, (long) *i64);
            break;

        case PRTE_UINT64:
            ui64 = (uint64_t*)src;
            prte_asprintf(output, "%sData type: %s\tData size: 64-bit\tValue: %lu",
                     (NULL == prefix) ? "" : prefix, type_name, (unsigned long) *ui64);
            break;

        default:
            return;
    }

    return;
}

/*
 * STANDARD PRINT FUNCTION - WORKS FOR EVERYTHING NON-STRUCTURED
 */
int prte_dt_std_print(char **output, char *prefix, void *src, prte_data_type_t type)
{
    /* set default result */
    *output = NULL;

    switch(type) {
        case PRTE_STD_CNTR:
            prte_dt_quick_print(output, "PRTE_STD_CNTR", prefix, src, PRTE_STD_CNTR_T);
            break;

        case PRTE_PROC_STATE:
            prte_dt_quick_print(output, "PRTE_PROC_STATE", prefix, src, PRTE_PROC_STATE_T);
            break;

        case PRTE_JOB_STATE:
            prte_dt_quick_print(output, "PRTE_JOB_STATE", prefix, src, PRTE_JOB_STATE_T);
            break;

        case PRTE_NODE_STATE:
            prte_dt_quick_print(output, "PRTE_NODE_STATE", prefix, src, PRTE_NODE_STATE_T);
            break;

        case PRTE_EXIT_CODE:
            prte_dt_quick_print(output, "PRTE_EXIT_CODE", prefix, src, PRTE_EXIT_CODE_T);
            break;

        case PRTE_RML_TAG:
            prte_dt_quick_print(output, "PRTE_RML_TAG", prefix, src, PRTE_RML_TAG_T);
            break;

        case PRTE_DAEMON_CMD:
            prte_dt_quick_print(output, "PRTE_DAEMON_CMD", prefix, src, PRTE_DAEMON_CMD_T);
            break;

        case PRTE_IOF_TAG:
            prte_dt_quick_print(output, "PRTE_IOF_TAG", prefix, src, PRTE_IOF_TAG_T);
            break;

        default:
            PRTE_ERROR_LOG(PRTE_ERR_UNKNOWN_DATA_TYPE);
            return PRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    return PRTE_SUCCESS;
}

/*
 * JOB
 */
int prte_dt_print_job(char **output, char *prefix, prte_job_t *src, prte_data_type_t type)
{
    char *tmp, *tmp2, *tmp3, *pfx2, *pfx;
    int32_t i;
    int rc;
    prte_app_context_t *app;
    prte_proc_t *proc;

    /* set default result */
    *output = NULL;

    /* protect against NULL prefix */
    if (NULL == prefix) {
        prte_asprintf(&pfx2, " ");
    } else {
        prte_asprintf(&pfx2, "%s", prefix);
    }

    tmp2 = prte_argv_join(src->personality, ',');
    prte_asprintf(&tmp, "\n%sData for job: %s\tPersonality: %s\tRecovery: %s(%s)\n%s\tNum apps: %ld\tStdin target: %s\tState: %s\tAbort: %s", pfx2,
             PRTE_JOBID_PRINT(src->jobid), tmp2,
             (PRTE_FLAG_TEST(src, PRTE_JOB_FLAG_RECOVERABLE)) ? "ENABLED" : "DISABLED",
             (prte_get_attribute(&src->attributes, PRTE_JOB_RECOVER_DEFINED, NULL, PRTE_BOOL)) ? "DEFINED" : "DEFAULT",
             pfx2,
             (long)src->num_apps, PRTE_VPID_PRINT(src->stdin_target),
              prte_job_state_to_str(src->state), (PRTE_FLAG_TEST(src, PRTE_JOB_FLAG_ABORTED)) ? "True" : "False");
    free(tmp2);
    prte_asprintf(&pfx, "%s\t", pfx2);
    free(pfx2);

    for (i=0; i < src->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(src->apps, i))) {
            continue;
        }
        prte_dss.print(&tmp2, pfx, app, PRTE_APP_CONTEXT);
        prte_asprintf(&tmp3, "%s\n%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    if (NULL != src->map) {
        if (PRTE_SUCCESS != (rc = prte_dss.print(&tmp2, pfx, src->map, PRTE_JOB_MAP))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        prte_asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    } else {
        prte_asprintf(&tmp2, "%s\n%sNo Map", tmp, pfx);
        free(tmp);
        tmp = tmp2;
    }

    prte_asprintf(&tmp2, "%s\n%sNum procs: %ld\tOffset: %ld", tmp, pfx, (long)src->num_procs, (long)src->offset);
    free(tmp);
    tmp = tmp2;

    for (i=0; i < src->procs->size; i++) {
        if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(src->procs, i))) {
            continue;
        }
        if (PRTE_SUCCESS != (rc = prte_dss.print(&tmp2, pfx, proc, PRTE_PROC))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        prte_asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    prte_asprintf(&tmp2, "%s\n%s\tNum launched: %ld\tNum reported: %ld\tNum terminated: %ld",
             tmp, pfx, (long)src->num_launched, (long)src->num_reported,
             (long)src->num_terminated);
    free(tmp);
    tmp = tmp2;

    /* set the return */
    *output = tmp;
    free(pfx);

    return PRTE_SUCCESS;
}

/*
 * NODE
 */
int prte_dt_print_node(char **output, char *prefix, prte_node_t *src, prte_data_type_t type)
{
    char *tmp, *tmp2, *tmp3, *pfx2, *pfx;
    int32_t i;
    int rc;
    prte_proc_t *proc;
    char **alias;
    /* set default result */
    *output = NULL;

    /* protect against NULL prefix */
    if (NULL == prefix) {
        prte_asprintf(&pfx2, " ");
    } else {
        prte_asprintf(&pfx2, "%s", prefix);
    }

    if (!prte_devel_level_output) {
        /* just provide a simple output for users */
        if (0 == src->num_procs) {
            /* no procs mapped yet, so just show allocation */
            prte_asprintf(&tmp, "\n%sData for node: %s\tNum slots: %ld\tMax slots: %ld",
                     pfx2, (NULL == src->name) ? "UNKNOWN" : src->name,
                     (long)src->slots, (long)src->slots_max);
            /* does this node have any aliases? */
            tmp3 = NULL;
            if (prte_get_attribute(&src->attributes, PRTE_NODE_ALIAS, (void**)&tmp3, PRTE_STRING)) {
                alias = prte_argv_split(tmp3, ',');
                for (i=0; NULL != alias[i]; i++) {
                    prte_asprintf(&tmp2, "%s%s\tresolved from %s\n", tmp, pfx2, alias[i]);
                    free(tmp);
                    tmp = tmp2;
                }
                prte_argv_free(alias);
            }
            if (NULL != tmp3) {
                free(tmp3);
            }
            free(pfx2);
            *output = tmp;
            return PRTE_SUCCESS;
        }
        prte_asprintf(&tmp, "\n%sData for node: %s\tNum slots: %ld\tMax slots: %ld\tNum procs: %ld",
                 pfx2, (NULL == src->name) ? "UNKNOWN" : src->name,
                 (long)src->slots, (long)src->slots_max, (long)src->num_procs);
        /* does this node have any aliases? */
        tmp3 = NULL;
        if (prte_get_attribute(&src->attributes, PRTE_NODE_ALIAS, (void**)&tmp3, PRTE_STRING)) {
            alias = prte_argv_split(tmp3, ',');
            for (i=0; NULL != alias[i]; i++) {
                prte_asprintf(&tmp2, "%s%s\tresolved from %s\n", tmp, pfx2, alias[i]);
                free(tmp);
                tmp = tmp2;
            }
            prte_argv_free(alias);
        }
        if (NULL != tmp3) {
            free(tmp3);
        }
        goto PRINT_PROCS;
    }

    prte_asprintf(&tmp, "\n%sData for node: %s\tState: %0x\tFlags: %02x",
             pfx2, (NULL == src->name) ? "UNKNOWN" : src->name, src->state, src->flags);
    /* does this node have any aliases? */
    tmp3 = NULL;
    if (prte_get_attribute(&src->attributes, PRTE_NODE_ALIAS, (void**)&tmp3, PRTE_STRING)) {
        alias = prte_argv_split(tmp3, ',');
        for (i=0; NULL != alias[i]; i++) {
            prte_asprintf(&tmp2, "%s%s\tresolved from %s\n", tmp, pfx2, alias[i]);
            free(tmp);
            tmp = tmp2;
        }
        prte_argv_free(alias);
    }
    if (NULL != tmp3) {
        free(tmp3);
    }

    if (NULL == src->daemon) {
        prte_asprintf(&tmp2, "%s\n%s\tDaemon: %s\tDaemon launched: %s", tmp, pfx2,
                 "Not defined", PRTE_FLAG_TEST(src, PRTE_NODE_FLAG_DAEMON_LAUNCHED) ? "True" : "False");
    } else {
        prte_asprintf(&tmp2, "%s\n%s\tDaemon: %s\tDaemon launched: %s", tmp, pfx2,
                 PRTE_NAME_PRINT(&(src->daemon->name)),
                 PRTE_FLAG_TEST(src, PRTE_NODE_FLAG_DAEMON_LAUNCHED) ? "True" : "False");
    }
    free(tmp);
    tmp = tmp2;

    prte_asprintf(&tmp2, "%s\n%s\tNum slots: %ld\tSlots in use: %ld\tOversubscribed: %s", tmp, pfx2,
             (long)src->slots, (long)src->slots_inuse,
             PRTE_FLAG_TEST(src, PRTE_NODE_FLAG_OVERSUBSCRIBED) ? "TRUE" : "FALSE");
    free(tmp);
    tmp = tmp2;

    prte_asprintf(&tmp2, "%s\n%s\tNum slots allocated: %ld\tMax slots: %ld", tmp, pfx2,
             (long)src->slots, (long)src->slots_max);
    free(tmp);
    tmp = tmp2;

    tmp3 = NULL;
    if (prte_get_attribute(&src->attributes, PRTE_NODE_USERNAME, (void**)&tmp3, PRTE_STRING)) {
        prte_asprintf(&tmp2, "%s\n%s\tUsername on node: %s", tmp, pfx2, tmp3);
        free(tmp3);
        free(tmp);
        tmp = tmp2;
    }

    if (prte_display_topo_with_map && NULL != src->topology) {
        char *pfx3;
        prte_asprintf(&tmp2, "%s\n%s\tDetected Resources:\n", tmp, pfx2);
        free(tmp);
        tmp = tmp2;

        tmp2 = NULL;
        prte_asprintf(&pfx3, "%s\t\t", pfx2);
        prte_dss.print(&tmp2, pfx3, src->topology, PRTE_HWLOC_TOPO);
        free(pfx3);
        prte_asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    prte_asprintf(&tmp2, "%s\n%s\tNum procs: %ld\tNext node_rank: %ld", tmp, pfx2,
             (long)src->num_procs, (long)src->next_node_rank);
    free(tmp);
    tmp = tmp2;

 PRINT_PROCS:
    prte_asprintf(&pfx, "%s\t", pfx2);
    free(pfx2);

    for (i=0; i < src->procs->size; i++) {
        if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(src->procs, i))) {
            continue;
        }
        if (PRTE_SUCCESS != (rc = prte_dss.print(&tmp2, pfx, proc, PRTE_PROC))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        prte_asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }
    free(pfx);

    /* set the return */
    *output = tmp;

    return PRTE_SUCCESS;
}

/*
 * PROC
 */
int prte_dt_print_proc(char **output, char *prefix, prte_proc_t *src, prte_data_type_t type)
{
    char *tmp, *tmp3, *pfx2;
    hwloc_obj_t loc=NULL;
    char *locale, *tmp2;
    hwloc_cpuset_t mycpus;
    char *str=NULL, *cpu_bitmap=NULL;


    /* set default result */
    *output = NULL;

    /* protect against NULL prefix */
    if (NULL == prefix) {
        prte_asprintf(&pfx2, " ");
    } else {
        prte_asprintf(&pfx2, "%s", prefix);
    }

    if (!prte_devel_level_output) {
        if (prte_get_attribute(&src->attributes, PRTE_PROC_CPU_BITMAP, (void**)&cpu_bitmap, PRTE_STRING) &&
            NULL != src->node->topology && NULL != src->node->topology->topo) {
            mycpus = hwloc_bitmap_alloc();
            hwloc_bitmap_list_sscanf(mycpus, cpu_bitmap);
            if (NULL == (str = prte_hwloc_base_cset2str(mycpus, false, src->node->topology->topo))) {
                str = strdup("UNBOUND");
            }
            hwloc_bitmap_free(mycpus);
            prte_asprintf(&tmp, "\n%sProcess jobid: %s App: %ld Process rank: %s Bound: %s", pfx2,
                     PRTE_JOBID_PRINT(src->name.jobid), (long)src->app_idx,
                     PRTE_VPID_PRINT(src->name.vpid), (NULL == str) ? "N/A" : str);
            if (NULL != str) {
                free(str);
            }
            if (NULL != cpu_bitmap) {
                free(cpu_bitmap);
            }
        } else {
            /* just print a very simple output for users */
            prte_asprintf(&tmp, "\n%sProcess jobid: %s App: %ld Process rank: %s Bound: N/A", pfx2,
                     PRTE_JOBID_PRINT(src->name.jobid), (long)src->app_idx,
                     PRTE_VPID_PRINT(src->name.vpid));
        }

        /* set the return */
        *output = tmp;
        free(pfx2);
        return PRTE_SUCCESS;
    }

    prte_asprintf(&tmp, "\n%sData for proc: %s", pfx2, PRTE_NAME_PRINT(&src->name));

    prte_asprintf(&tmp3, "%s\n%s\tPid: %ld\tLocal rank: %lu\tNode rank: %lu\tApp rank: %d", tmp, pfx2,
             (long)src->pid, (unsigned long)src->local_rank, (unsigned long)src->node_rank, src->app_rank);
    free(tmp);
    tmp = tmp3;

    if (prte_get_attribute(&src->attributes, PRTE_PROC_HWLOC_LOCALE, (void**)&loc, PRTE_PTR)) {
        if (NULL != loc) {
            locale = prte_hwloc_base_cset2str(loc->cpuset, false, src->node->topology->topo);
        } else {
            locale = strdup("UNKNOWN");
        }
    } else {
        locale = strdup("UNKNOWN");
    }
    if (prte_get_attribute(&src->attributes, PRTE_PROC_CPU_BITMAP, (void**)&cpu_bitmap, PRTE_STRING) &&
        NULL != src->node->topology && NULL != src->node->topology->topo) {
        mycpus = hwloc_bitmap_alloc();
        hwloc_bitmap_list_sscanf(mycpus, cpu_bitmap);
        tmp2 = prte_hwloc_base_cset2str(mycpus, false, src->node->topology->topo);
    } else {
        tmp2 = strdup("UNBOUND");
    }
    prte_asprintf(&tmp3, "%s\n%s\tState: %s\tApp_context: %ld\n%s\tLocale:  %s\n%s\tBinding: %s", tmp, pfx2,
             prte_proc_state_to_str(src->state), (long)src->app_idx, pfx2, locale, pfx2,  tmp2);
    free(locale);
    free(tmp);
    free(tmp2);
    if (NULL != str) {
        free(str);
    }
    if (NULL != cpu_bitmap) {
        free(cpu_bitmap);
    }

    /* set the return */
    *output = tmp3;

    free(pfx2);
    return PRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prte_dt_print_app_context(char **output, char *prefix, prte_app_context_t *src, prte_data_type_t type)
{
    char *tmp, *tmp2, *tmp3, *pfx2;
    int i, count;
    prte_value_t *kv;

    /* set default result */
    *output = NULL;

    /* protect against NULL prefix */
    if (NULL == prefix) {
        prte_asprintf(&pfx2, " ");
    } else {
        prte_asprintf(&pfx2, "%s", prefix);
    }

    prte_asprintf(&tmp, "\n%sData for app_context: index %lu\tapp: %s\n%s\tNum procs: %lu\tFirstRank: %s",
             pfx2, (unsigned long)src->idx,
             (NULL == src->app) ? "NULL" : src->app,
             pfx2, (unsigned long)src->num_procs,
             PRTE_VPID_PRINT(src->first_rank));

    count = prte_argv_count(src->argv);
    for (i=0; i < count; i++) {
        prte_asprintf(&tmp2, "%s\n%s\tArgv[%d]: %s", tmp, pfx2, i, src->argv[i]);
        free(tmp);
        tmp = tmp2;
    }

    count = prte_argv_count(src->env);
    for (i=0; i < count; i++) {
        prte_asprintf(&tmp2, "%s\n%s\tEnv[%lu]: %s", tmp, pfx2, (unsigned long)i, src->env[i]);
        free(tmp);
        tmp = tmp2;
    }

    tmp3 = NULL;
    prte_get_attribute(&src->attributes, PRTE_APP_PREFIX_DIR, (void**)&tmp3, PRTE_STRING);
    prte_asprintf(&tmp2, "%s\n%s\tWorking dir: %s\n%s\tPrefix: %s\n%s\tUsed on node: %s", tmp,
             pfx2, (NULL == src->cwd) ? "NULL" : src->cwd,
             pfx2, (NULL == tmp3) ? "NULL" : tmp3,
             pfx2, PRTE_FLAG_TEST(src, PRTE_APP_FLAG_USED_ON_NODE) ? "TRUE" : "FALSE");
    free(tmp);
    tmp = tmp2;

    PRTE_LIST_FOREACH(kv, &src->attributes, prte_value_t) {
        prte_dss.print(&tmp2, pfx2, kv, PRTE_ATTRIBUTE);
        prte_asprintf(&tmp3, "%s\n%s", tmp, tmp2);
        free(tmp2);
        free(tmp);
        tmp = tmp3;
    }

    /* set the return */
    *output = tmp;

    free(pfx2);
    return PRTE_SUCCESS;
}

/*
 * JOB_MAP
 */
int prte_dt_print_map(char **output, char *prefix, prte_job_map_t *src, prte_data_type_t type)
{
    char *tmp=NULL, *tmp2, *tmp3, *pfx, *pfx2;
    int32_t i, j;
    int rc;
    prte_node_t *node;
    prte_proc_t *proc;

    /* set default result */
    *output = NULL;

    /* protect against NULL prefix */
    if (NULL == prefix) {
        prte_asprintf(&pfx2, " ");
    } else {
        prte_asprintf(&pfx2, "%s", prefix);
    }

    prte_asprintf(&pfx, "%s\t", pfx2);

    if (prte_devel_level_output) {
        prte_asprintf(&tmp, "\n%sMapper requested: %s  Last mapper: %s  Mapping policy: %s  Ranking policy: %s\n%sBinding policy: %s",
                 pfx2, (NULL == src->req_mapper) ? "NULL" : src->req_mapper,
                 (NULL == src->last_mapper) ? "NULL" : src->last_mapper,
                 prte_rmaps_base_print_mapping(src->mapping),
                 prte_rmaps_base_print_ranking(src->ranking),
                 pfx2, prte_hwloc_base_print_binding(src->binding));

        if (PRTE_VPID_INVALID == src->daemon_vpid_start) {
            prte_asprintf(&tmp2, "%s\n%sNum new daemons: %ld\tNew daemon starting vpid INVALID\n%sNum nodes: %ld",
                     tmp, pfx, (long)src->num_new_daemons, pfx, (long)src->num_nodes);
        } else {
            prte_asprintf(&tmp2, "%s\n%sNum new daemons: %ld\tNew daemon starting vpid %ld\n%sNum nodes: %ld",
                     tmp, pfx, (long)src->num_new_daemons, (long)src->daemon_vpid_start,
                     pfx, (long)src->num_nodes);
        }
        free(tmp);
        tmp = tmp2;
    } else {
        /* this is being printed for a user, so let's make it easier to see */
        prte_asprintf(&tmp, "\n%s========================   JOB MAP   ========================", pfx2);
    }


    for (i=0; i < src->nodes->size; i++) {
        if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(src->nodes, i))) {
            continue;
        }
        if (PRTE_SUCCESS != (rc = prte_dss.print(&tmp2, pfx2, node, PRTE_NODE))) {
            PRTE_ERROR_LOG(rc);
            free(pfx);
            free(tmp);
            return rc;
        }
        prte_asprintf(&tmp3, "%s\n%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }

    if (!prte_devel_level_output) {
        /* this is being printed for a user, so let's make it easier to see */
        prte_asprintf(&tmp2, "%s\n\n%s=============================================================\n", tmp, pfx2);
        free(tmp);
        tmp = tmp2;
    }
    free(pfx2);

    /* set the return */
    *output = tmp;

    free(pfx);
    return PRTE_SUCCESS;
}

/* PRTE_ATTR */
int prte_dt_print_attr(char **output, char *prefix,
                       prte_attribute_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = strdup(prefix);

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_ATTR\tValue: NULL pointer", prefx);
        free(prefx);
        return PRTE_SUCCESS;
    }

    switch (src->type) {
    case PRTE_BOOL:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_BOOL\tValue: %s",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL",
                 src->data.flag ? "TRUE" : "FALSE");
        break;
    case PRTE_STRING:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_STRING\tValue: %s",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", src->data.string);
        break;
    case PRTE_SIZE:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_SIZE\tValue: %lu",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (unsigned long)src->data.size);
        break;
    case PRTE_PID:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_PID\tValue: %lu",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (unsigned long)src->data.pid);
        break;
    case PRTE_INT:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_INT\tValue: %d",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", src->data.integer);
        break;
    case PRTE_INT8:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_INT8\tValue: %d",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (int)src->data.int8);
        break;
    case PRTE_INT16:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_INT16\tValue: %d",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (int)src->data.int16);
        break;
    case PRTE_INT32:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_INT32\tValue: %d",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", src->data.int32);
        break;
    case PRTE_INT64:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_INT64\tValue: %d",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (int)src->data.int64);
        break;
    case PRTE_UINT:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_UINT\tValue: %u",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (unsigned int)src->data.uint);
        break;
    case PRTE_UINT8:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_UINT8\tValue: %u",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (unsigned int)src->data.uint8);
        break;
    case PRTE_UINT16:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_UINT16\tValue: %u",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (unsigned int)src->data.uint16);
        break;
    case PRTE_UINT32:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_UINT32\tValue: %u",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", src->data.uint32);
        break;
    case PRTE_UINT64:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_UINT64\tValue: %lu",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", (unsigned long)src->data.uint64);
        break;
    case PRTE_BYTE_OBJECT:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_BYTE_OBJECT\tValue: UNPRINTABLE",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL");
        break;
    case PRTE_BUFFER:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_BUFFER\tValue: UNPRINTABLE",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL");
        break;
    case PRTE_FLOAT:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_FLOAT\tValue: %f",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", src->data.fval);
        break;
    case PRTE_TIMEVAL:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_TIMEVAL\tValue: %ld.%06ld", prefx,
                 prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL",
                 (long)src->data.tv.tv_sec, (long)src->data.tv.tv_usec);
        break;
    case PRTE_PTR:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_PTR", prefx,
                 prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL");
        break;
    case PRTE_VPID:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_VPID\tValue: %s", prefx,
                 prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", PRTE_VPID_PRINT(src->data.vpid));
        break;
    case PRTE_JOBID:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_JOBID\tValue: %s", prefx,
                 prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", PRTE_JOBID_PRINT(src->data.jobid));
        break;
    case PRTE_NAME:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_NAME\tValue: %s", prefx,
                 prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL", PRTE_NAME_PRINT(&src->data.name));
        break;
    case PRTE_ENVAR:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: PRTE_ENVAR\tEnvar: %s\tValue: %s", prefx,
                 prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL",
                 (NULL == src->data.envar.envar) ? "NULL" : src->data.envar.envar,
                 (NULL == src->data.envar.value) ? "NULL" : src->data.envar.value);
        break;
    default:
        prte_asprintf(output, "%sKey: %s\tPRTE_ATTR: %s Data type: UNKNOWN\tValue: UNPRINTABLE",
                 prefx, prte_attr_key_to_str(src->key), src->local ? "LOCAL" : "GLOBAL");
        break;
    }
    free(prefx);
    return PRTE_SUCCESS;
}

int prte_dt_print_sig(char **output, char *prefix, prte_grpcomm_signature_t *src, prte_data_type_t type)
{
    char *prefx;
    size_t i;
    char *tmp, *tmp2;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = strdup(prefix);

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_SIG", prefx);
        free(prefx);
        return PRTE_SUCCESS;
    }

    if (NULL == src->signature) {
        prte_asprintf(output, "%sPRTE_SIG  Procs: NULL", prefx);
        free(prefx);
        return PRTE_SUCCESS;
    }

    /* there must be at least one proc in the signature */
    prte_asprintf(&tmp, "%sPRTE_SIG  Procs: ", prefx);

    for (i=0; i < src->sz; i++) {
        prte_asprintf(&tmp2, "%s%s", tmp, PRTE_NAME_PRINT(&src->signature[i]));
        free(tmp);
        tmp = tmp2;
    }
    *output = tmp;
    return PRTE_SUCCESS;
}
