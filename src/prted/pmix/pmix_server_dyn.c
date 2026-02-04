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
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include "src/mca/pinstalldirs/pinstalldirs_types.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_getcwd.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_cmd_line.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

void pmix_server_notify_spawn(pmix_nspace_t jobid, int room, pmix_status_t ret)
{
    prte_pmix_server_req_t *req;
    prte_job_t *jdata;

    jdata = prte_get_job_data_object(jobid);
    if (NULL != jdata &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED, NULL, PMIX_BOOL)) {
        /* already done */
        return;
    }

    /* retrieve the request */
    req = (prte_pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, room);
    if (NULL == req) {
        /* we are hosed */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, room, NULL);

    /* execute the callback */
    if (NULL != req->spcbfunc) {
        req->spcbfunc(ret, jobid, req->cbdata);
    } else if (NULL != req->toolcbfunc) {
        if (PMIX_SUCCESS == ret) {
            PMIX_LOAD_PROCID(&req->target, jobid, 0);
        }
        req->toolcbfunc(ret, &req->target, req->cbdata);
    }
    /* cleanup */
    PMIX_RELEASE(req);

    /* mark that we sent it */
    if (NULL != jdata) {
        prte_set_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED,
                           PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
    }
}
void pmix_server_launch_resp(int status, pmix_proc_t *sender,
                             pmix_data_buffer_t *buffer,
                             prte_rml_tag_t tg, void *cbdata)
{
    int rc, room;
    int32_t ret, cnt;
    pmix_nspace_t jobid;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
    }

    /* unpack the jobid */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &jobid, &cnt, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
    }
    /* we let the above errors fall thru in the vain hope that the room number can
     * be successfully unpacked, thus allowing us to respond to the requestor */

    /* unpack our tracking room number */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &room, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* we are hosed */
        return;
    }

    pmix_server_notify_spawn(jobid, room, ret);
}

static void spawn(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;
    int rc;
    pmix_data_buffer_t *buf;
    prte_plm_cmd_flag_t command;
    char nspace[PMIX_MAX_NSLEN + 1];
    pmix_status_t prc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(req);

    /* add this request to our tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    /* include the request room number for quick retrieval */
    prte_set_attribute(&req->jdata->attributes, PRTE_JOB_ROOM_NUM,
                       PRTE_ATTR_GLOBAL, &req->local_index, PMIX_INT);

    /* construct a spawn message */
    PMIX_DATA_BUFFER_CREATE(buf);

    command = PRTE_PLM_LAUNCH_JOB_CMD;
    rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        goto callback;
    }

    /* pack the jdata object */
    rc = prte_job_pack(buf, req->jdata);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }

    /* send it to the HNP for processing - might be myself! */
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_PLM);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }
    return;

callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->spcbfunc) {
        prc = prte_pmix_convert_rc(rc);
        PMIX_LOAD_NSPACE(nspace, NULL);
        req->spcbfunc(prc, nspace, req->cbdata);
    }
    PMIX_RELEASE(req);
}

int prte_pmix_xfer_job_info(prte_job_t *jdata,
                            pmix_info_t *iptr,
                            size_t ninfo)
{
    pmix_info_t *info;
    size_t n;
    int i, m, rc;
    bool flag;
    uint32_t u32;
    uint16_t u16;
    prte_job_t *djob;
    prte_app_context_t *app;
    pmix_envar_t envar;

    for (n = 0; n < ninfo; n++) {
        info = &iptr[n];

            /***   PREFIX   ***/
        if (PMIX_CHECK_KEY(info, PMIX_PREFIX)) {
            /* this is the default PMIx library prefix for the
             * job - apply it to all apps, but avoid overwriting
             * any value specified at the app-level */
            for (m=0; m < jdata->apps->size; m++) {
                app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, m);
                if (NULL == app) {
                    continue;
                }
                if (!prte_get_attribute(&app->attributes, PRTE_APP_PMIX_PREFIX, NULL, PMIX_STRING)) {
                    // an app-level prefix wasn't given, so use this one
                    prte_set_attribute(&app->attributes, PRTE_APP_PMIX_PREFIX,
                                       PRTE_ATTR_GLOBAL, info->value.data.string, PMIX_STRING);
                }
            }

            /***   REQUESTED MAPPER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPPER)) {
            jdata->map->req_mapper = strdup(info->value.data.string);

            /***   DISPLAY ALLOCATION   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_ALLOCATION)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_ALLOC,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /***   ALLOC/SESSION IDs  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info->value, u32, uint32_t);
            if (PMIX_SUCCESS != rc) {
                return PRTE_ERR_BAD_PARAM;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_SESSION_ID,
                               PRTE_ATTR_GLOBAL, &u32, PMIX_UINT32);
        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_ID)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ALLOC_ID,
                               PRTE_ATTR_GLOBAL, info->value.data.string, PMIX_STRING);
        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_REQ_ID)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_REF_ID,
                               PRTE_ATTR_GLOBAL, info->value.data.string, PMIX_STRING);

            /***   DISPLAY MAP   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_MAP)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /***   DISPLAY MAP-DEVEL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_MAP_DETAILED)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /***   REPORT BINDINGS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REPORT_BINDINGS)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_REPORT_BINDINGS,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /***   USE PHYSICAL CPUS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REPORT_PHYSICAL_CPUS)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /***   DISPLAY TOPOLOGY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_TOPOLOGY)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_TOPO,
                               PRTE_ATTR_GLOBAL, info->value.data.string, PMIX_STRING);

            /***   DISPLAY PROCESSORS   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_PROCESSORS)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PROCESSORS,
                               PRTE_ATTR_GLOBAL, info->value.data.string, PMIX_STRING);

            /***   DISPLAY PARSEABLE OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_PARSEABLE_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PARSEABLE_OUTPUT,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

        /***   PPR (PROCS-PER-RESOURCE)   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_PPR)) {
            if (PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
                /* not allowed to provide multiple mapping policies */
                pmix_show_help("help-prte-rmaps-base.txt", "redefining-policy", true, "mapping",
                               info->value.data.string,
                               prte_rmaps_base_print_mapping(prte_rmaps_base.mapping));
                return PRTE_ERR_BAD_PARAM;
            }
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_PPR);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

            /***   MAP-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPBY)) {
            if (PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
                /* not allowed to provide multiple mapping policies */
                pmix_show_help("help-prte-rmaps-base.txt", "redefining-policy", true, "mapping",
                               info->value.data.string,
                               prte_rmaps_base_print_mapping(jdata->map->mapping));
                return PRTE_ERR_BAD_PARAM;
            }
            rc = prte_rmaps_base_set_mapping_policy(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                return rc;
            }

            /*** colocation directives ***/
            /***   PROCS WHERE NEW PROCS ARE TO BE COLOCATED   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_COLOCATE_PROCS)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_PROCS,
                               PRTE_ATTR_GLOBAL, info->value.data.darray, PMIX_DATA_ARRAY);

            /***   NUMBER OF PROCS TO SPAWN AT EACH COLOCATION  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_COLOCATE_NPERPROC)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_NPERPROC,
                               PRTE_ATTR_GLOBAL, &info->value.data.uint16, PMIX_UINT16);

            /***   NUMBER OF PROCS TO SPAWN AT EACH COLOCATION  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_COLOCATE_NPERNODE)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_NPERNODE,
                               PRTE_ATTR_GLOBAL, &info->value.data.uint16, PMIX_UINT16);

            /***   RANK-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_RANKBY)) {
            if (PRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
                /* not allowed to provide multiple mapping policies */
                pmix_show_help("help-prte-rmaps-base.txt", "redefining-policy", true, "ranking",
                               info->value.data.string,
                               prte_rmaps_base_print_ranking(jdata->map->ranking));
                return PRTE_ERR_BAD_PARAM;
            }
            rc = prte_rmaps_base_set_ranking_policy(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                return rc;
            }

            /***   BIND-TO   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_BINDTO)) {
            if (PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                /* not allowed to provide multiple mapping policies */
                pmix_show_help("help-prte-rmaps-base.txt", "redefining-policy", true, "binding",
                               info->value.data.string,
                               prte_hwloc_base_print_binding(jdata->map->binding));
                return PRTE_ERR_BAD_PARAM;
            }
            rc = prte_hwloc_base_set_binding_policy(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                return rc;
            }

            /***   RUNTIME OPTIONS  - SHOULD ONLY APPEAR IF NOT PRE-PROCESSED BY SCHIZO ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_RUNTIME_OPTIONS)) {
            rc = prte_state_base_set_runtime_options(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                return rc;
            }

            /*** ABORT_NON_ZERO  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_ABORT_NON_ZERO_TERM)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ERROR_NONZERO_EXIT,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /*** DO_NOT_LAUNCH  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DO_NOT_LAUNCH)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);
            /* if we are not in a persistent DVM, then make sure we also
             * apply this to the daemons */
            if (!prte_persistent) {
                djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
                prte_set_attribute(&djob->attributes, PRTE_JOB_DO_NOT_LAUNCH, PRTE_ATTR_GLOBAL,
                                   &flag, PMIX_BOOL);
            }

                /*** SHOW_PROGRESS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_SHOW_LAUNCH_PROGRESS)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_SHOW_PROGRESS, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /*** RECOVER  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_RECOVERABLE)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /*** CONTINUOUS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_CONTINUOUS)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /*** CHILD INDEPENDENCE  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_SPAWN_CHILD_SEP)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_CHILD_SEP, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   MAX RESTARTS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAX_RESTARTS)) {
            for (i = 0; i < jdata->apps->size; i++) {
                app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
                if (NULL == app) {
                    continue;
                }
                prte_set_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, PRTE_ATTR_GLOBAL,
                                   &info->value.data.uint32, PMIX_INT32);
            }

           /*** EXEC AGENT ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_EXEC_AGENT)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_EXEC_AGENT, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

            /***   STOP ON EXEC FOR DEBUGGER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_ON_EXEC)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_ON_EXEC,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_IN_INIT)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_INIT,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
            /* also must add to job-level cache */
            pmix_server_cache_job_info(jdata, info);

        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_IN_APP)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
            /* also must add to job-level cache */
            pmix_server_cache_job_info(jdata, info);

            /***   CPUS/RANK   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPUS_PER_PROC)) {
            u16 = info->value.data.uint32;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC,
                               PRTE_ATTR_GLOBAL, &u16, PMIX_UINT16);

            /***   NO USE LOCAL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NO_PROCS_ON_HEAD)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
            } else {
                PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
            }
            /* mark that the user specified it */
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_LOCAL_GIVEN);

            /***   OVERSUBSCRIBE   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NO_OVERSUBSCRIBE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            } else {
                PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            }
            /* mark that the user specified it */
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);

            /***   CPU LIST  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPU_LIST)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

            /***   NON-PMI JOB   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NON_PMI)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_NON_PRTE_JOB, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_PARENT_ID)) {
            PMIX_XFER_PROCID(&jdata->originator, info->value.data.proc);

            /***   SPAWN REQUESTOR IS TOOL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REQUESTOR_IS_TOOL)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DVM_JOB, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);
            /* request that IO be forwarded to the requesting tool */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_FWDIO_TO_TOOL, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   NOTIFY UPON JOB COMPLETION   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NOTIFY_COMPLETION)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_NOTIFY_COMPLETION, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   TAG STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_OUTPUT) ||
                   PMIX_CHECK_KEY(info, PMIX_TAG_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /*** DETAILED OIUTPUT TAG */
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_DETAILED_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT_DETAILED,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /*** FULL NAMESPACE IN OUTPUT TAG */
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_FULLNAME_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT_FULLNAME,
                               PRTE_ATTR_GLOBAL, &flag, PMIX_BOOL);

            /***   RANK STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_RANK_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_RANK_OUTPUT, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /***   TIMESTAMP OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TIMESTAMP_OUTPUT) ||
                   PMIX_CHECK_KEY(info, PMIX_TIMESTAMP_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   XML OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_XML_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /***   OUTPUT TO FILES   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_OUTPUT_TO_FILE) ||
                   PMIX_CHECK_KEY(info, PMIX_OUTPUT_TO_FILE)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_TO_FILE, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_OUTPUT_TO_DIRECTORY) ||
                   PMIX_CHECK_KEY(info, PMIX_OUTPUT_TO_DIRECTORY)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_TO_DIRECTORY, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_FILE_ONLY) ||
                   PMIX_CHECK_KEY(info, PMIX_OUTPUT_NOCOPY)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_NOCOPY, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   MERGE STDERR TO STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_MERGE_STDERR_STDOUT) ||
                   PMIX_CHECK_KEY(info, PMIX_MERGE_STDERR_STDOUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   RAW OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_OUTPUT_RAW)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_RAW_OUTPUT, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /***   STDIN TARGET   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_STDIN_TGT)) {
            if (0 == strcmp(info->value.data.string, "all")) {
                jdata->stdin_target = PMIX_RANK_WILDCARD;
            } else if (0 == strcmp(info->value.data.string, "none")) {
                jdata->stdin_target = PMIX_RANK_INVALID;
            } else {
                jdata->stdin_target = strtoul(info->value.data.string, NULL, 10);
            }

            /***   INDEX ARGV   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_INDEX_ARGV)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_INDEX_ARGV, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /***   DEBUGGER DAEMONS   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUGGER_DAEMONS)) {
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);

            /***   CO-LOCATE TARGET FOR DEBUGGER DAEMONS    ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_TARGET)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DEBUG_TARGET, PRTE_ATTR_GLOBAL,
                               info->value.data.proc, PMIX_PROC);
            pmix_server_cache_job_info(jdata, info);

            /***   NUMBER OF DEBUGGER_DAEMONS PER NODE   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_DAEMONS_PER_NODE)) {
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_NODE,
                               PRTE_ATTR_GLOBAL, &info->value.data.uint16, PMIX_UINT16);

            /***   NUMBER OF DEBUGGER_DAEMONS PER PROC   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_DAEMONS_PER_PROC)) {
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_PROC,
                               PRTE_ATTR_GLOBAL, &info->value.data.uint16, PMIX_UINT16);

            /* there can be multiple of these, so we add them to the attribute list */
        } else if (PMIX_CHECK_KEY(info, PMIX_ENVARS_HARVESTED)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ENVARS_HARVESTED,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
        } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_SET_ENVAR,
                               PRTE_ATTR_GLOBAL, &envar, PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ADD_ENVAR,
                               PRTE_ATTR_GLOBAL, &envar, PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_UNSET_ENVAR,
                               PRTE_ATTR_GLOBAL, info->value.data.string, PMIX_STRING);
        } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PREPEND_ENVAR,
                               PRTE_ATTR_GLOBAL, &envar, PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_APPEND_ENVAR,
                               PRTE_ATTR_GLOBAL, &envar, PMIX_ENVAR);

        } else if (PMIX_CHECK_KEY(info, PMIX_SPAWN_TOOL)) {
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_SPAWN_TIMEOUT) ||
                   PMIX_CHECK_KEY(info, PMIX_TIMEOUT)) {
            if (PMIX_STRING == info->value.type) {
                rc = PMIX_CONVERT_TIME(info->value.data.string);
            } else {
                PMIX_VALUE_GET_NUMBER(i, &info->value, rc, int);
                if (PMIX_SUCCESS != i) {
                    return PRTE_ERR_BAD_PARAM;
                }
            }
            prte_set_attribute(&jdata->attributes, PRTE_SPAWN_TIMEOUT,
                               PRTE_ATTR_GLOBAL, &rc, PMIX_INT);

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT)) {
            prte_set_attribute(&jdata->attributes, PRTE_SPAWN_TIMEOUT, PRTE_ATTR_GLOBAL,
                               &info->value.data.integer, PMIX_INT);

        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_TIMEOUT)) {
            if (PMIX_STRING == info->value.type) {
                rc = PMIX_CONVERT_TIME(info->value.data.string);
            } else {
                PMIX_VALUE_GET_NUMBER(i, &info->value, rc, int);
                if (PMIX_SUCCESS != i) {
                    return PRTE_ERR_BAD_PARAM;
                }
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT,
                               PRTE_ATTR_GLOBAL, &rc, PMIX_INT);

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT_STACKTRACES)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_STACKTRACES, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT_REPORT_STATE)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_REPORT_STATE, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_LOG_AGG)) {
            flag = !PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_NOAGG_HELP, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_AGGREGATE_HELP)) {
            flag = !PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_NOAGG_HELP, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_GPU_SUPPORT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_GPU_SUPPORT, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

        } else if (PMIX_CHECK_KEY(info, PMIX_FWD_ENVIRONMENT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   DEFAULT - CACHE FOR INCLUSION WITH JOB INFO   ***/
        } else {
            pmix_server_cache_job_info(jdata, info);
        }
    }

    return PRTE_SUCCESS;
}

int prte_pmix_xfer_app(prte_job_t *jdata, pmix_app_t *papp)
{
    prte_app_context_t *app;
    pmix_info_t *info;
    size_t m;
    int rc;
    bool flag;
    pmix_envar_t envar;
    char cwd[PRTE_PATH_MAX];

    app = PMIX_NEW(prte_app_context_t);
    app->job = (struct prte_job_t*)jdata;
    app->idx = pmix_pointer_array_add(jdata->apps, app);
    jdata->num_apps++;
    if (NULL != papp->cmd) {
        app->app = strdup(papp->cmd);
    } else if (NULL == papp->argv || NULL == papp->argv[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    } else {
        app->app = strdup(papp->argv[0]);
    }
    if (NULL != papp->argv) {
        app->argv = PMIx_Argv_copy(papp->argv);
    }
    if (NULL != papp->env) {
        app->env = PMIx_Argv_copy(papp->env);
    }
    if (NULL != papp->cwd) {
        app->cwd = strdup(papp->cwd);
    }
    app->num_procs = papp->maxprocs;

    if (NULL != papp->info) {
        for (m = 0; m < papp->ninfo; m++) {
            info = &papp->info[m];
            if (PMIX_CHECK_KEY(info, PMIX_HOST)) {
                prte_set_attribute(&app->attributes, PRTE_APP_DASH_HOST, PRTE_ATTR_GLOBAL,
                                   info->value.data.string, PMIX_STRING);

            } else if (PMIX_CHECK_KEY(info, PMIX_HOSTFILE)) {
                prte_set_attribute(&app->attributes, PRTE_APP_HOSTFILE, PRTE_ATTR_GLOBAL,
                                   info->value.data.string, PMIX_STRING);

            } else if (PMIX_CHECK_KEY(info, PMIX_ADD_HOSTFILE)) {
                prte_set_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE, PRTE_ATTR_GLOBAL,
                                   info->value.data.string, PMIX_STRING);

            } else if (PMIX_CHECK_KEY(info, PMIX_ADD_HOST)) {
                prte_set_attribute(&app->attributes, PRTE_APP_ADD_HOST, PRTE_ATTR_GLOBAL,
                                   info->value.data.string, PMIX_STRING);

            } else if (PMIX_CHECK_KEY(info, PMIX_PREFIX)) {
                prte_prepend_attribute(&app->attributes, PRTE_APP_PMIX_PREFIX,
                                       PRTE_ATTR_GLOBAL,
                                       info->value.data.string, PMIX_STRING);

            } else if (PMIX_CHECK_KEY(info, PMIX_WDIR)) {
                /* if this is a relative path, convert it to an absolute path */
                if (pmix_path_is_absolute(info->value.data.string)) {
                    app->cwd = strdup(info->value.data.string);
                } else {
                    /* get the cwd */
                    if (PRTE_SUCCESS != (rc = pmix_getcwd(cwd, sizeof(cwd)))) {
                        pmix_show_help("help-prted.txt", "cwd", true, "spawn", rc);
                        PMIX_RELEASE(jdata);
                        return rc;
                    }
                    /* construct the absolute path */
                    app->cwd = pmix_os_path(false, cwd, info->value.data.string, NULL);
                }

            } else if (PMIX_CHECK_KEY(info, PMIX_WDIR_USER_SPECIFIED)) {
                flag = PMIX_INFO_TRUE(info);
                prte_set_attribute(&app->attributes, PRTE_APP_USER_CWD, PRTE_ATTR_GLOBAL,
                                   &flag, PMIX_BOOL);

            } else if (PMIX_CHECK_KEY(info, PMIX_SET_SESSION_CWD)) {
                flag = PMIX_INFO_TRUE(info);
                prte_set_attribute(&app->attributes, PRTE_APP_SSNDIR_CWD, PRTE_ATTR_GLOBAL,
                                   &flag, PMIX_BOOL);

            } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_FILES)) {
                prte_set_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES, PRTE_ATTR_GLOBAL,
                                   info->value.data.string, PMIX_STRING);

            } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_BIN)) {
                prte_set_attribute(&app->attributes, PRTE_APP_PRELOAD_BIN, PRTE_ATTR_GLOBAL,
                                   NULL, PMIX_BOOL);

            /***   PPR (PROCS-PER-RESOURCE)   ***/
            } else if (PMIX_CHECK_KEY(info, PMIX_PPR)) {
                char **ck, *p;
                uint16_t ppn, pes;
                int n;
                ck = PMIx_Argv_split(info->value.data.string, ':');
                if (3 > PMIx_Argv_count(ck)) {
                    PMIx_Argv_free(ck);
                    return PMIX_ERR_BAD_PARAM;
                }
                if (0 == strcasecmp(ck[0], "ppr")) {
                    ppn =  strtoul(ck[1], NULL, 10);
                    prte_set_attribute(&app->attributes, PRTE_APP_PPR,
                                       PRTE_ATTR_GLOBAL, &ppn, PMIX_UINT16);
                    // ck[2] has the object type
                    pes = 0;
                    for (n=2; NULL != ck[n]; n++) {
                        p = strchr(ck[n], '=');
                        if (NULL != p && 0 == strncmp(ck[n], "pe", 2)) {
                            ++p;
                            pes = strtol(p, NULL, 10);
                            break;
                        }
                    }
                    if (0 < pes) {
                        prte_set_attribute(&app->attributes, PRTE_APP_PES_PER_PROC,
                                           PRTE_ATTR_GLOBAL, &pes, PMIX_UINT16);
                    }
                } 
                PMIx_Argv_free(ck);
 
                /***   MAP-BY   ***/
            } else if (PMIX_CHECK_KEY(info, PMIX_MAPBY)) {
                char **ck, *p;
                uint16_t ppn, pes;
                int n;
                ck = PMIx_Argv_split(info->value.data.string, ':');
                for (n=0; NULL != ck[n]; n++) {
                    if (0 == strcasecmp(ck[n], "ppr")) {
                        ppn =  strtoul(ck[1], NULL, 10);
                        prte_set_attribute(&app->attributes, PRTE_APP_PPR,
                                           PRTE_ATTR_GLOBAL, &ppn, PMIX_UINT16);
                    } else if (0 == strncmp(ck[n], "pe", 2) &&
                               0 != strncmp(ck[n], "pe-", 3)) {
                        p = strchr(ck[n], '=');
                        if (NULL == p) {
                            /* missing the value or value is invalid */
                            pmix_show_help("help-prte-rmaps-base.txt", "invalid-value", true, "mapping policy",
                                           "PE", ck[n]);
                            PMIx_Argv_free(ck);
                            return PRTE_ERR_SILENT;
                        }
                        ++p;
                        if (NULL == p || '\0' == *p) {
                            /* missing the value or value is invalid */
                            pmix_show_help("help-prte-rmaps-base.txt", "invalid-value", true, "mapping policy",
                                           "PE", ck[n]);
                            PMIx_Argv_free(ck);
                            return PRTE_ERR_SILENT;
                        }
                        pes = strtol(p, NULL, 10);                
                        if (0 < pes) {
                            prte_set_attribute(&app->attributes, PRTE_APP_PES_PER_PROC,
                                               PRTE_ATTR_GLOBAL, &pes, PMIX_UINT16);
                        }
                    }
                }
                PMIx_Argv_free(ck);

                /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
                /* there can be multiple of these, so we add them to the attribute list */
            } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
                envar.envar = info->value.data.envar.envar;
                envar.value = info->value.data.envar.value;
                envar.separator = info->value.data.envar.separator;
                if (0 == app->idx) {
                    prte_prepend_attribute(&jdata->attributes, PRTE_JOB_SET_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                } else {
                    prte_prepend_attribute(&app->attributes, PRTE_APP_SET_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                }
            } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
                envar.envar = info->value.data.envar.envar;
                envar.value = info->value.data.envar.value;
                envar.separator = info->value.data.envar.separator;
                if (0 == app->idx) {
                    prte_prepend_attribute(&jdata->attributes, PRTE_JOB_ADD_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                } else {
                    prte_prepend_attribute(&app->attributes, PRTE_APP_ADD_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                }
            } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
                if (0 == app->idx) {
                    prte_prepend_attribute(&jdata->attributes, PRTE_JOB_UNSET_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           info->value.data.string, PMIX_STRING);
                } else {
                    prte_prepend_attribute(&app->attributes, PRTE_APP_UNSET_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           info->value.data.string, PMIX_STRING);
                }
            } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
                envar.envar = info->value.data.envar.envar;
                envar.value = info->value.data.envar.value;
                envar.separator = info->value.data.envar.separator;
                if (0 == app->idx) {
                    prte_prepend_attribute(&jdata->attributes, PRTE_JOB_PREPEND_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                } else {
                    prte_prepend_attribute(&app->attributes, PRTE_APP_PREPEND_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                }
            } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
                envar.envar = info->value.data.envar.envar;
                envar.value = info->value.data.envar.value;
                envar.separator = info->value.data.envar.separator;
                if (0 == app->idx) {
                    prte_prepend_attribute(&jdata->attributes, PRTE_JOB_APPEND_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                } else {
                    prte_prepend_attribute(&app->attributes, PRTE_APP_APPEND_ENVAR,
                                           PRTE_ATTR_GLOBAL,
                                           &envar, PMIX_ENVAR);
                }

            } else if (PMIX_CHECK_KEY(info, PMIX_PSET_NAME)) {
                prte_set_attribute(&app->attributes, PRTE_APP_PSET_NAME, PRTE_ATTR_GLOBAL,
                                   info->value.data.string, PMIX_STRING);
            } else {
                /* unrecognized key */
                if (9 < pmix_output_get_verbosity(prte_pmix_server_globals.output)) {
                    pmix_show_help("help-prted.txt", "bad-key", true, "spawn", "application",
                                   info->key);
                }
            }
        }
    }
    return PRTE_SUCCESS;
}

static void interim(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    pmix_proc_t *requestor = &cd->proc;
    prte_job_t *jdata;
    prte_app_context_t *app;
    int rc;
    size_t n;
    prte_rmaps_options_t options;
    prte_schizo_base_module_t *schizo;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s spawn called from proc %s with %d apps",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(requestor),
                        (int) cd->napps);

    /* create the job object */
    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    /* default to the requestor as the originator */
    PMIX_LOAD_PROCID(&jdata->originator, requestor->nspace, requestor->rank);
    /* find the personality being passed - we need this info to direct
     * option parsing */
    for (n=0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_PERSONALITY)) {
            jdata->personality = PMIx_Argv_split(cd->info[n].value.data.string, ',');
            jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy(cd->info[n].value.data.string);
            pmix_server_cache_job_info(jdata, &cd->info[n]);
            break;
        }
    }
    if (NULL == jdata->personality) {
        /* use the default */
        jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy(NULL);
    }

    /* transfer the apps across */
    for (n = 0; n < cd->napps; n++) {
        rc = prte_pmix_xfer_app(jdata, &cd->apps[n]);
        if (PRTE_SUCCESS != rc) {
            goto complete;
        }
    }

    /* initiate the default runtime options - had to delay this until
     * after we parsed the apps as some runtime options are for
     * the apps themselves */
    memset(&options, 0, sizeof(prte_rmaps_options_t));
    options.stream = prte_rmaps_base_framework.framework_output;
    options.verbosity = 5;  // usual value for base-level functions
    schizo = (prte_schizo_base_module_t*)jdata->schizo;
    rc = schizo->set_default_rto(jdata, &options);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        goto complete;
    }

    /* transfer the job info across */
    rc = prte_pmix_xfer_job_info(jdata, cd->info, cd->ninfo);
    if (PRTE_SUCCESS != rc) {
        goto complete;
    }

    /* set debugger flags on apps if needed */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
        for (n=0; n < (size_t)jdata->apps->size; n++) {
            app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, n);
            if (NULL != app) {
                PRTE_FLAG_SET(app, PRTE_APP_FLAG_TOOL);
            }
        }
    }

    /* indicate the requestor so bookmarks can be correctly set */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, PRTE_ATTR_GLOBAL,
                       &jdata->originator, PMIX_PROC);

    /* indicate that IO is to be forwarded */
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_FORWARD_OUTPUT);

    /* setup a spawn tracker so we know who to call back when this is done
     * and thread-shift the entire thing so it can be safely added to
     * our tracking list */
    PRTE_SPN_REQ(jdata, spawn, cd->spcbfunc, cd->cbdata);
    PMIX_RELEASE(cd);
    return;

complete:
    if (NULL != cd->spcbfunc) {
        pmix_status_t prc;
        pmix_nspace_t nspace;
        PMIX_LOAD_NSPACE(nspace, NULL);
        prc = prte_pmix_convert_rc(rc);
        cd->spcbfunc(prc, nspace, cd->cbdata);
        /* this isn't going to launch, so indicate that */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
    }
    PMIX_RELEASE(cd);
}

int pmix_server_spawn_fn(const pmix_proc_t *proc, const pmix_info_t job_info[], size_t ninfo,
                         const pmix_app_t apps[], size_t napps, pmix_spawn_cbfunc_t cbfunc,
                         void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s spawn upcalled on behalf of proc %s:%u with %" PRIsize_t " job infos",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), proc->nspace, proc->rank, ninfo);

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    PMIX_LOAD_PROCID(&cd->proc, proc->nspace, proc->rank);
    cd->info = (pmix_info_t *) job_info;
    cd->ninfo = ninfo;
    cd->apps = (pmix_app_t *) apps;
    cd->napps = napps;
    cd->spcbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, interim, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

// modex callback func for connect
static void connect_release(pmix_status_t status,
                            const char *data, size_t sz,
                            void *cbdata,
                            pmix_release_cbfunc_t rel, void *relcbdata)
{
    prte_pmix_server_req_t *md = (prte_pmix_server_req_t*)cbdata;
    pmix_byte_object_t bo;
    pmix_data_buffer_t pbkt;
    pmix_info_t *info = NULL, infostat;
    pmix_proc_t *procID;
    pmix_nspace_t *nspace;
    pmix_status_t rc;
    int cnt;

    PMIX_ACQUIRE_OBJECT(md);

    rc = status;
    if (PMIX_SUCCESS != status) {
        goto release;
    }

    /* process returned data */
    if (NULL != data && 0 != sz) {
        /* prep for unpacking */
        bo.bytes = (char*)data;
        bo.size = sz;
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
        rc = PMIx_Data_embed(&pbkt, &bo);
        if (PMIX_SUCCESS != rc) {
            goto release;
        }
        // the payload consists of packed info containing
        // endpoint info for each involved process - we have to
        // convert each entry into an array of info, and then
        // load that into a PMIX_PROC_DATA info
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &pbkt, &infostat, &cnt, PMIX_INFO);
        while (PMIX_SUCCESS == rc) {
            // only interested in the endpt data
            if (PMIX_CHECK_KEY(&infostat, PMIX_PROC_DATA)) {
                // contains an array of info
                info = (pmix_info_t*)infostat.value.data.darray->array;
                // procID is in first place
                procID = info[0].value.data.proc;
                // register this data
                rc = PMIx_server_register_nspace(procID->nspace, -1, &infostat, 1, NULL, NULL);
                if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            } else if (PMIX_CHECK_KEY(&infostat, PMIX_JOB_INFO_ARRAY)) {
                // contains an array of job-level info
                info = (pmix_info_t*)infostat.value.data.darray->array;
                // npace is in first place
                nspace = info[0].value.data.nspace;
                // register this data
                rc = PMIx_server_register_nspace(*nspace, -1, &infostat, 1, NULL, NULL);
                if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_DESTRUCT(&infostat);
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &pbkt, &infostat, &cnt, PMIX_INFO);
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        } else {
            rc = PMIX_SUCCESS;
        }
    }

release:
    /* now release the connect call */
    if (NULL != md->opcbfunc) {
        md->opcbfunc(rc, md->cbdata);
    }

    if (NULL != rel) {
        rel(relcbdata);
    }
    PMIX_RELEASE(md);
}


pmix_status_t pmix_server_connect_fn(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *cd;
    size_t n;
    pmix_status_t rc;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s connect called with %d procs",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nprocs);

    /* protect ourselves */
    if (NULL == procs || 0 == nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* PMIx server only calls us if this is a multi-node operation. In this
     * case, we also need to (a) execute a "fence" across the participating
     * nodes, and (b) send along any endpt information posted by the participants
     * for "remote" scope */

    cd = PMIX_NEW(prte_pmix_server_req_t);
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_DATA) ||
            PMIX_CHECK_KEY(&info[n], PMIX_JOB_INFO_ARRAY)) {
            rc = PMIx_Data_pack(NULL, &cd->msg, (pmix_info_t*)&info[n], 1, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(cd);
                return rc;
            }
        }
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    rc = prte_grpcomm.fence(procs, nprocs, info, ninfo,
                            cd->msg.unpack_ptr, cd->msg.bytes_used,
                            connect_release, cd);
    return rc;
}

static void mdxcbfunc(pmix_status_t status,
                      const char *data, size_t ndata, void *cbdata,
                      pmix_release_cbfunc_t relcbfunc, void *relcbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(data, ndata, relcbfunc, relcbdata);

    PMIX_ACQUIRE_OBJECT(cd);
    /* ack the call */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_disconnect_fn(const pmix_proc_t procs[], size_t nprocs,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    pmix_status_t rc;

    pmix_output_verbose(2, prte_pmix_server_globals.output, "%s disconnect called",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* at some point, we need to add bookeeping to track which
     * procs are "connected" so we know who to notify upon
     * termination or failure. For now, just execute a fence
     * Note that we do not need to thread-shift here as the
     * fence function will do it for us */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    rc = pmix_server_fencenb_fn(procs, nprocs, info, ninfo, NULL, 0, mdxcbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
    }

    return rc;
}
