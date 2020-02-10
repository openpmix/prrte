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
 * Copyright (c) 2009-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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
#include "src/util/prrte_getcwd.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/dss/dss.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/rml/rml.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

void pmix_server_launch_resp(int status, prrte_process_name_t* sender,
                             prrte_buffer_t *buffer,
                             prrte_rml_tag_t tg, void *cbdata)
{
    pmix_server_req_t *req;
    int rc, room;
    int32_t ret, cnt;
    prrte_jobid_t jobid;
    prrte_job_t *jdata;
    char nspace[PMIX_MAX_NSLEN+1];
    pmix_proc_t proc;

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &ret, &cnt, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(rc);
        ret = prrte_pmix_convert_rc(rc);
    }

    /* unpack the jobid */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &jobid, &cnt, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        ret = prrte_pmix_convert_rc(rc);
    }
    /* we let the above errors fall thru in the vain hope that the room number can
     * be successfully unpacked, thus allowing us to respond to the requestor */

    /* unpack our tracking room number */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &room, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        /* we are hosed */
        return;
    }

    /* retrieve the request */
    prrte_hotel_checkout_and_return_occupant(&prrte_pmix_server_globals.reqs, room, (void**)&req);
    if (NULL == req) {
        /* we are hosed */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return;
    }

    /* execute the callback */
    if (NULL != req->spcbfunc) {
        PRRTE_PMIX_CONVERT_JOBID(nspace, jobid);
        req->spcbfunc(ret, nspace, req->cbdata);
    } else if (NULL != req->toolcbfunc) {
        /* if success, then add to our job info */
        if (PRRTE_SUCCESS == ret) {
            jdata = PRRTE_NEW(prrte_job_t);
            jdata->jobid = jobid;
            req->target.jobid = jobid;
            req->target.vpid = 0;
            prrte_pmix_server_tool_conn_complete(jdata, req);
            PRRTE_PMIX_CONVERT_NAME(&proc, &req->target);
        }
        req->toolcbfunc(ret, &proc, req->cbdata);
    }
    /* cleanup */
    PRRTE_RELEASE(req);
}

static void spawn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    int rc;
    prrte_buffer_t *buf;
    prrte_plm_cmd_flag_t command;
    char nspace[PMIX_MAX_NSLEN+1];
    pmix_status_t prc;

    PRRTE_ACQUIRE_OBJECT(req);

    /* add this request to our tracker hotel */
    PRRTE_ADJUST_TIMEOUT(req);
    if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
        prrte_show_help("help-orted.txt", "noroom", true, req->operation, prrte_pmix_server_globals.num_rooms);
        goto callback;
    }

    /* include the request room number for quick retrieval */
    prrte_set_attribute(&req->jdata->attributes, PRRTE_JOB_ROOM_NUM,
                       PRRTE_ATTR_GLOBAL, &req->room_num, PRRTE_INT);

    /* construct a spawn message */
    buf = PRRTE_NEW(prrte_buffer_t);
    command = PRRTE_PLM_LAUNCH_JOB_CMD;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &command, 1, PRRTE_PLM_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        goto callback;
    }

    /* pack the jdata object */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &req->jdata, 1, PRRTE_JOB))) {
        PRRTE_ERROR_LOG(rc);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        PRRTE_RELEASE(buf);
        goto callback;

    }

    /* send it to the HNP for processing - might be myself! */
    if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                                      PRRTE_RML_TAG_PLM,
                                                      prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        PRRTE_RELEASE(buf);
        goto callback;
    }
    return;

  callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->spcbfunc) {
        prc = prrte_pmix_convert_rc(rc);
        PRRTE_PMIX_CONVERT_JOBID(nspace, PRRTE_JOBID_INVALID);
        req->spcbfunc(prc, nspace, req->cbdata);
    }
    PRRTE_RELEASE(req);
}

static void interim(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    prrte_process_name_t *requestor = &cd->proc;
    prrte_envar_t envar;
    prrte_job_t *jdata;
    prrte_app_context_t *app;
    pmix_app_t *papp;
    pmix_info_t *info;
    prrte_list_t *cache;
    int rc, i;
    char cwd[PRRTE_PATH_MAX];
    bool flag;
    size_t m, n;
    prrte_value_t *kv;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s spawn called from proc %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(requestor));

    /* create the job object */
    jdata = PRRTE_NEW(prrte_job_t);
    jdata->map = PRRTE_NEW(prrte_job_map_t);

    /* transfer the apps across */
    for (n=0; n < cd->napps; n++) {
        papp = &cd->apps[n];
        app = PRRTE_NEW(prrte_app_context_t);
        app->idx = prrte_pointer_array_add(jdata->apps, app);
        jdata->num_apps++;
        if (NULL != papp->cmd) {
            app->app = strdup(papp->cmd);
        } else if (NULL == papp->argv ||
                   NULL == papp->argv[0]) {
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            PRRTE_RELEASE(jdata);
            rc = PRRTE_ERR_BAD_PARAM;
            goto complete;
        } else {
            app->app = strdup(papp->argv[0]);
        }
        if (NULL != papp->argv) {
            app->argv = prrte_argv_copy(papp->argv);
        }
        if (NULL != papp->env) {
            app->env = prrte_argv_copy(papp->env);
        }
        if (NULL != papp->cwd) {
            app->cwd = strdup(papp->cwd);
        }
        app->num_procs = papp->maxprocs;
        if (NULL != papp->info) {
            for (m=0; m < papp->ninfo; m++) {
                info = &papp->info[m];
                if (PMIX_CHECK_KEY(info, PMIX_HOST)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_DASH_HOST,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_HOSTFILE)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_HOSTFILE,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_HOSTFILE)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_ADD_HOSTFILE,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_HOST)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_ADD_HOST,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_PREFIX)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_WDIR)) {
                    /* if this is a relative path, convert it to an absolute path */
                    if (prrte_path_is_absolute(info->value.data.string)) {
                        app->cwd = strdup(info->value.data.string);
                    } else {
                        /* get the cwd */
                        if (PRRTE_SUCCESS != (rc = prrte_getcwd(cwd, sizeof(cwd)))) {
                            prrte_show_help("help-orted.txt", "cwd", true, "spawn", rc);
                            PRRTE_RELEASE(jdata);
                            goto complete;
                        }
                        /* construct the absolute path */
                        app->cwd = prrte_os_path(false, cwd, info->value.data.string, NULL);
                    }
                } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_BIN)) {
                    flag = PMIX_INFO_TRUE(info);
                    prrte_set_attribute(&app->attributes, PRRTE_APP_PRELOAD_BIN,
                                       PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);
                } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_FILES)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_PRELOAD_FILES,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);

                } else if (PMIX_CHECK_KEY(info, PMIX_COSPAWN_APP)) {
                    flag = PMIX_INFO_TRUE(info);
                    prrte_set_attribute(&app->attributes, PRRTE_APP_DEBUGGER_DAEMON,
                                       PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);
                /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
                /* there can be multiple of these, so we add them to the attribute list */
                } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prrte_add_attribute(&app->attributes, PRRTE_APP_SET_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prrte_add_attribute(&app->attributes, PRRTE_APP_ADD_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
                    prrte_add_attribute(&app->attributes, PRRTE_APP_UNSET_ENVAR,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prrte_add_attribute(&app->attributes, PRRTE_APP_PREPEND_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prrte_add_attribute(&app->attributes, PRRTE_APP_APPEND_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);

#if PMIX_NUMERIC_VERSION >= 0x00040000
                } else if (PMIX_CHECK_KEY(info, PMIX_PSET_NAME)) {
                    prrte_set_attribute(&app->attributes, PRRTE_APP_PSET_NAME,
                                       PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
#endif
                } else {
                    /* unrecognized key */
                    prrte_show_help("help-orted.txt", "bad-key",
                                   true, "spawn", "application", info->key);
                }
            }
        }
    }

    /* transfer the job info across */
    for (m=0; m < cd->ninfo; m++) {
        info = &cd->info[m];
        /***   PERSONALITY   ***/
        if (PMIX_CHECK_KEY(info, PMIX_PERSONALITY)) {
            jdata->personality = prrte_argv_split(info->value.data.string, ',');

        /***   REQUESTED MAPPER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPPER)) {
            jdata->map->req_mapper = strdup(info->value.data.string);

        /***   DISPLAY MAP   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_MAP)) {
            jdata->map->display_map = PMIX_INFO_TRUE(info);

        /***   PPR (PROCS-PER-RESOURCE)   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_PPR)) {
            if (PRRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
                /* not allowed to provide multiple mapping policies */
                prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy",
                               true, "mapping", info->value.data.string,
                               prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
                rc = PRRTE_ERR_BAD_PARAM;
                goto complete;
            }
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_PPR);
            jdata->map->ppr = strdup(info->value.data.string);

        /***   MAP-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPBY)) {
            rc = prrte_rmaps_base_set_mapping_policy(jdata, &jdata->map->mapping,
                                                    NULL, info->value.data.string);
            if (PRRTE_SUCCESS != rc) {
                goto complete;
            }
        /***   RANK-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_RANKBY)) {
            rc = prrte_rmaps_base_set_ranking_policy(&jdata->map->ranking,
                                                    jdata->map->mapping,
                                                    info->value.data.string);
            if (PRRTE_SUCCESS != rc) {
                goto complete;
            }

        /***   BIND-TO   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_BINDTO)) {
            rc = prrte_hwloc_base_set_binding_policy(&jdata->map->binding,
                                                    info->value.data.string);
            if (PRRTE_SUCCESS != rc) {
                goto complete;
            }

        /***   CPUS/RANK   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPUS_PER_PROC)) {
            jdata->map->cpus_per_rank = info->value.data.uint32;

        /***   NO USE LOCAL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NO_PROCS_ON_HEAD)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_USE_LOCAL);
            } else {
                PRRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_USE_LOCAL);
            }
            /* mark that the user specified it */
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_LOCAL_GIVEN);

        /***   OVERSUBSCRIBE   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NO_OVERSUBSCRIBE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            } else {
                PRRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            }
            /* mark that the user specified it */
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_SUBSCRIBE_GIVEN);

        /***   REPORT BINDINGS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REPORT_BINDINGS)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_REPORT_BINDINGS,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   CPU LIST  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPU_LIST)) {
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_CPU_LIST,
                               PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_BOOL);

        /***   RECOVERABLE  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_RECOVERABLE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_RECOVERABLE);
            } else {
                PRRTE_FLAG_UNSET(jdata, PRRTE_JOB_FLAG_RECOVERABLE);
            }

        /***   MAX RESTARTS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAX_RESTARTS)) {
            for (i=0; i < jdata->apps->size; i++) {
                if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                prrte_set_attribute(&app->attributes, PRRTE_APP_MAX_RESTARTS,
                                   PRRTE_ATTR_GLOBAL, &info->value.data.uint32, PRRTE_INT32);
            }

        /***   CONTINUOUS OPERATION  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_CONTINUOUS)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_CONTINUOUS_OP,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   NON-PMI JOB   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NON_PMI)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NON_PRRTE_JOB,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   SPAWN REQUESTOR IS TOOL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REQUESTOR_IS_TOOL)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DVM_JOB,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);
            if (flag) {
                /* request that IO be forwarded to the requesting tool */
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_FWDIO_TO_TOOL,
                                   PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);
            }

        /***   NOTIFY UPON JOB COMPLETION   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NOTIFY_COMPLETION)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NOTIFY_COMPLETION,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   STOP ON EXEC FOR DEBUGGER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_ON_EXEC)) {
#if PRRTE_HAVE_STOP_ON_EXEC
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_STOP_ON_EXEC,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);
#else
            /* we cannot support the request */
            rc = PRRTE_ERR_NOT_SUPPORTED;
            goto complete;
#endif

        /***   STOP IN INIT  AND WAIT AT SOME PROGRAMMATIC POINT FOR DEBUGGER   ***/
        /***   ALLOW TO FALL INTO THE JOB-LEVEL CACHE AS THEY ARE INCLUDED IN   ***/
        /***   THE INITIAL JOB-INFO DELIVERED TO PROCS                          ***/

        /***   TAG STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_TAG_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_TAG_OUTPUT,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   TIMESTAMP OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_TIMESTAMP_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_TIMESTAMP_OUTPUT,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   OUTPUT TO FILES   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_OUTPUT_TO_FILE)) {
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_OUTPUT_TO_FILE,
                               PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);

        /***   MERGE STDERR TO STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MERGE_STDERR_STDOUT)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_MERGE_STDERR_STDOUT,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   STDIN TARGET   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_STDIN_TGT)) {
            if (0 == strcmp(info->value.data.string, "all")) {
                jdata->stdin_target = PRRTE_VPID_WILDCARD;
            } else if (0 == strcmp(info->value.data.string, "none")) {
                jdata->stdin_target = PRRTE_VPID_INVALID;
            } else {
                jdata->stdin_target = strtoul(info->value.data.string, NULL, 10);
            }

        /***   INDEX ARGV   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_INDEX_ARGV)) {
            flag = PMIX_INFO_TRUE(info);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_INDEX_ARGV,
                               PRRTE_ATTR_GLOBAL, &flag, PRRTE_BOOL);

        /***   DEBUGGER DAEMONS   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUGGER_DAEMONS)) {
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_DEBUGGER_DAEMON);
            PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_DEBUGGER);

        /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
        /* there can be multiple of these, so we add them to the attribute list */
        } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prrte_add_attribute(&jdata->attributes, PRRTE_JOB_SET_ENVAR,
                               PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prrte_add_attribute(&jdata->attributes, PRRTE_JOB_ADD_ENVAR,
                               PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
            prrte_add_attribute(&jdata->attributes, PRRTE_JOB_UNSET_ENVAR,
                               PRRTE_ATTR_GLOBAL, info->value.data.string, PRRTE_STRING);
        } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prrte_add_attribute(&jdata->attributes, PRRTE_JOB_PREPEND_ENVAR,
                               PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prrte_add_attribute(&jdata->attributes, PRRTE_JOB_APPEND_ENVAR,
                               PRRTE_ATTR_GLOBAL, &envar, PRRTE_ENVAR);
#if PMIX_NUMERIC_VERSION >= 0x00040000
        } else if (PMIX_CHECK_KEY(info, PMIX_SPAWN_TOOL)) {
            PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_TOOL);
#endif
        /***   DEFAULT - CACHE FOR INCLUSION WITH JOB INFO   ***/
        } else {
            /* cache for inclusion with job info at registration */
            kv = PRRTE_NEW(prrte_value_t);
            kv->key = strdup(info->key);
            prrte_pmix_value_unload(kv, &info->value);
            if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_INFO_CACHE, (void**)&cache, PRRTE_PTR)) {
                prrte_list_append(cache, &kv->super);
            } else {
                cache = PRRTE_NEW(prrte_list_t);
                prrte_list_append(cache, &kv->super);
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_INFO_CACHE, PRRTE_ATTR_LOCAL, (void*)cache, PRRTE_PTR);
            }
        }
    }
    /* if the job is missing a personality setting, add it */
    if (NULL == jdata->personality) {
        prrte_argv_append_nosize(&jdata->personality, "ompi");
    }

    /* indicate the requestor so bookmarks can be correctly set */
    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_LAUNCH_PROXY,
                       PRRTE_ATTR_GLOBAL, requestor, PRRTE_NAME);

    /* indicate that IO is to be forwarded */
    PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_FORWARD_OUTPUT);

    /* setup a spawn tracker so we know who to call back when this is done
     * and thread-shift the entire thing so it can be safely added to
     * our tracking list */
    PRRTE_SPN_REQ(jdata, spawn, cd->spcbfunc, cd->cbdata);
    PRRTE_RELEASE(cd);
    return;

  complete:
    if (NULL != cd->spcbfunc) {
        pmix_proc_t pproc;
        pmix_status_t prc;
        pmix_nspace_t nspace;
        PRRTE_PMIX_CONVERT_JOBID(pproc.nspace, PRRTE_JOBID_INVALID);
        PMIX_LOAD_NSPACE(nspace, NULL);
        prc = prrte_pmix_convert_rc(rc);
        cd->spcbfunc(prc, nspace, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

int pmix_server_spawn_fn(const pmix_proc_t *proc,
                         const pmix_info_t job_info[], size_t ninfo,
                         const pmix_app_t apps[], size_t napps,
                         pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd;
    int rc;

    cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    PRRTE_PMIX_CONVERT_NSPACE(rc, &cd->proc.jobid, proc->nspace);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        return PMIX_ERR_BAD_PARAM;
    }
    PRRTE_PMIX_CONVERT_RANK(cd->proc.vpid, proc->rank);
    cd->info = (pmix_info_t*)job_info;
    cd->ninfo = ninfo;
    cd->apps = (pmix_app_t*)apps;
    cd->napps = napps;
    cd->spcbfunc = cbfunc;
    cd->cbdata = cbdata;
    prrte_event_set(prrte_event_base, &cd->ev, -1,
                   PRRTE_EV_WRITE, interim, cd);
    prrte_event_set_priority(&cd->ev, PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(cd);
    prrte_event_active(&cd->ev, PRRTE_EV_WRITE, 1);
    return PRRTE_SUCCESS;
}

static void _cnct(int sd, short args, void *cbdata);

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    lock->status = status;
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

static void _cnlk(pmix_status_t status,
                  pmix_pdata_t data[], size_t ndata,
                  void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    int cnt;
    prrte_job_t *jdata;
    pmix_status_t ret;
    pmix_data_buffer_t pbkt;
    prrte_pmix_lock_t lock;
    pmix_info_t  *info = NULL;
    size_t ninfo;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* if we failed to get the required data, then just inform
     * the embedded server that the connect cannot succeed */
    if (PMIX_SUCCESS != status) {
        ret = status;
        goto release;
    }
    if (NULL == data) {
        ret = PMIX_ERR_NOT_FOUND;
        goto release;
    }

    /* if we have more than one data returned, that's an error */
    if (1 != ndata) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        ret = PMIX_ERR_BAD_PARAM;
        goto release;
    }

    /* the data will consist of a byte object containing
     * a packed buffer of the job data */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    PMIX_DATA_BUFFER_LOAD(&pbkt, data[0].value.data.bo.bytes, data[0].value.data.bo.size);
    data[0].value.data.bo.bytes = NULL;
    data[0].value.data.bo.size = 0;

    /* extract the number of returned info */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&data[0].proc, &pbkt, &ninfo, &cnt, PMIX_SIZE))) {
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        goto release;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&data[0].proc, &pbkt, info, &cnt, PMIX_INFO))) {
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_INFO_FREE(info, ninfo);
            goto release;
        }
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

    /* we have to process the data to convert it into an prrte_job_t
     * that describes this job as we didn't already have it */
    jdata = PRRTE_NEW(prrte_job_t);

    /* register the data with the local server */
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(data[0].proc.nspace,
                                      jdata->num_local_procs,
                                      info, ninfo, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        PRRTE_PMIX_DESTRUCT_LOCK(&lock);
        goto release;
    }
    PRRTE_PMIX_WAIT_THREAD(&lock);
    ret = lock.status;
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(info, ninfo);

    /* restart the cnct processor */
    PRRTE_PMIX_OPERATION(cd->procs, cd->nprocs, cd->info, cd->ninfo, _cnct, cd->cbfunc, cd->cbdata);
    /* we don't need to protect the re-referenced data as
     * the prrte_pmix_server_op_caddy_t does not have
     * a destructor! */
    PRRTE_RELEASE(cd);
    return;

  release:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(ret, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

static void _cnct(int sd, short args, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;
    char **keys = NULL;
    prrte_job_t *jdata;
    int rc = PRRTE_SUCCESS;
    size_t n;
    prrte_jobid_t jobid;
    uint32_t uid;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* at some point, we need to add bookeeping to track which
     * procs are "connected" so we know who to notify upon
     * termination or failure. For now, we have to ensure
     * that we have registered all participating nspaces so
     * the embedded PMIx server can provide them to the client.
     * Otherwise, the client will receive an error as it won't
     * be able to resolve any of the required data for the
     * missing nspaces */

    /* cycle thru the procs */
    for(n=0; n < cd->nprocs; n++) {
        PRRTE_PMIX_CONVERT_NSPACE(rc, &jobid, cd->procs[n].nspace);
        if (PRRTE_SUCCESS != rc) {
            PRRTE_ERROR_LOG(rc);
            goto release;
        }
        /* see if we have the job object for this job */
        if (NULL == (jdata = prrte_get_job_data_object(jobid))) {
            /* we don't know about this job. If our "global" data
             * server is just our HNP, then we have no way of finding
             * out about it, and all we can do is return an error */
            if (prrte_pmix_server_globals.server.jobid == PRRTE_PROC_MY_HNP->jobid &&
                prrte_pmix_server_globals.server.vpid == PRRTE_PROC_MY_HNP->vpid) {
                rc = PRRTE_ERR_NOT_SUPPORTED;
                goto release;
            }
            /* ask the global data server for the data - if we get it,
             * then we can complete the request */
            prrte_argv_append_nosize(&keys, cd->procs[n].nspace);
            /* we have to add the user's id to the directives */
            cd->ndirs = 1;
            PMIX_INFO_CREATE(cd->directives, cd->ndirs);
            uid = geteuid();
            PMIX_INFO_LOAD(&cd->directives[0], PMIX_USERID, &uid, PRRTE_UINT32);
            if (PRRTE_SUCCESS != (rc = pmix_server_lookup_fn(&cd->procs[n], keys,
                                                            cd->directives, cd->ndirs, _cnlk, cd))) {
                prrte_argv_free(keys);
                PMIX_INFO_FREE(cd->directives, cd->ndirs);
                goto release;
            }
            prrte_argv_free(keys);
            /* the callback function on this lookup will return us to this
             * routine so we can continue the process */
            return;
        }
        /* we know about the job - check to ensure it has been
         * registered with the local PMIx server */
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_NSPACE_REGISTERED, NULL, PRRTE_BOOL)) {
            /* it hasn't been registered yet, so register it now */
            if (PRRTE_SUCCESS != (rc = prrte_pmix_server_register_nspace(jdata))) {
                goto release;
            }
        }
    }

  release:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

pmix_status_t pmix_server_connect_fn(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *op;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s connect called with %d procs",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (int)nprocs);

    /* protect ourselves */
    if (NULL == procs || 0 == nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* must thread shift this as we will be accessing global data */
    op = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    op->procs = (pmix_proc_t*)procs;
    op->nprocs = nprocs;
    op->info = (pmix_info_t*)info;
    op->ninfo = ninfo;
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    prrte_event_set(prrte_event_base, &(op->ev), -1,
                   PRRTE_EV_WRITE, _cnct, op);
    prrte_event_set_priority(&(op->ev), PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(op);
    prrte_event_active(&(op->ev), PRRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void mdxcbfunc(pmix_status_t status,
                      const char *data, size_t ndata, void *cbdata,
                      pmix_release_cbfunc_t relcbfunc, void *relcbdata)
{
    prrte_pmix_server_op_caddy_t *cd = (prrte_pmix_server_op_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(cd);
    /* ack the call */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(status, cd->cbdata);
    }
    PRRTE_RELEASE(cd);
}

pmix_status_t pmix_server_disconnect_fn(const pmix_proc_t procs[], size_t nprocs,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_pmix_server_op_caddy_t *cd;
    pmix_status_t rc;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s disconnect called",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* at some point, we need to add bookeeping to track which
     * procs are "connected" so we know who to notify upon
     * termination or failure. For now, just execute a fence
     * Note that we do not need to thread-shift here as the
     * fence function will do it for us */
    cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    if (PMIX_SUCCESS != (rc = pmix_server_fencenb_fn(procs, nprocs,
                                                     info, ninfo,
                                                     NULL, 0,
                                                     mdxcbfunc, cd))) {
        PMIX_ERROR_LOG(rc);
        PRRTE_RELEASE(cd);
    }

    return rc;
}

pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                   pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    /* PRRTE currently has no way of supporting allocation requests */
    return PRRTE_ERR_NOT_SUPPORTED;
}
