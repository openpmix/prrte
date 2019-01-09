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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "orte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/util/argv.h"
#include "opal/util/opal_getcwd.h"
#include "opal/util/os_path.h"
#include "opal/util/output.h"
#include "opal/util/path.h"
#include "opal/dss/dss.h"
#include "opal/hwloc/hwloc-internal.h"
#include "opal/pmix/pmix-internal.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rmaps/base/base.h"
#include "orte/mca/state/state.h"
#include "orte/util/name_fns.h"
#include "orte/util/show_help.h"
#include "orte/util/threads.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/rml/rml.h"

#include "orte/orted/pmix/pmix_server.h"
#include "orte/orted/pmix/pmix_server_internal.h"

void pmix_server_launch_resp(int status, orte_process_name_t* sender,
                             opal_buffer_t *buffer,
                             orte_rml_tag_t tg, void *cbdata)
{
    pmix_server_req_t *req;
    int rc, room;
    int32_t ret, cnt;
    orte_jobid_t jobid;
    orte_job_t *jdata;
    char nspace[PMIX_MAX_NSLEN+1];
    pmix_proc_t proc;
    pmix_status_t xrc;

    /* unpack the status */
    cnt = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &ret, &cnt, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the jobid */
    cnt = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &jobid, &cnt, ORTE_JOBID))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* unpack our tracking room number */
    cnt = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &room, &cnt, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* retrieve the request */
    opal_hotel_checkout_and_return_occupant(&orte_pmix_server_globals.reqs, room, (void**)&req);
    if (NULL == req) {
        /* we are hosed */
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return;
    }

    /* execute the callback */
    if (NULL != req->spcbfunc) {
        OPAL_PMIX_CONVERT_JOBID(nspace, jobid);
        req->spcbfunc(ret, nspace, req->cbdata);
    } else if (NULL != req->toolcbfunc) {
        xrc = opal_pmix_convert_rc(ret);
        /* if success, then add to our job info */
        if (ORTE_SUCCESS == ret) {
            jdata = OBJ_NEW(orte_job_t);
            jdata->jobid = jobid;
            req->target.jobid = jobid;
            req->target.vpid = 0;
            orte_pmix_server_tool_conn_complete(jdata, req);
            OPAL_PMIX_CONVERT_NAME(&proc, &req->target);
        }
        req->toolcbfunc(xrc, &proc, req->cbdata);
    }
    /* cleanup */
    OBJ_RELEASE(req);
}

static void spawn(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    int rc;
    opal_buffer_t *buf;
    orte_plm_cmd_flag_t command;
    char nspace[PMIX_MAX_NSLEN+1];

    ORTE_ACQUIRE_OBJECT(req);

    /* add this request to our tracker hotel */
    if (OPAL_SUCCESS != (rc = opal_hotel_checkin(&orte_pmix_server_globals.reqs, req, &req->room_num))) {
        orte_show_help("help-orted.txt", "noroom", true, req->operation, orte_pmix_server_globals.num_rooms);
        goto callback;
    }

    /* include the request room number for quick retrieval */
    orte_set_attribute(&req->jdata->attributes, ORTE_JOB_ROOM_NUM,
                       ORTE_ATTR_GLOBAL, &req->room_num, OPAL_INT);

    /* construct a spawn message */
    buf = OBJ_NEW(opal_buffer_t);
    command = ORTE_PLM_LAUNCH_JOB_CMD;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(buf, &command, 1, ORTE_PLM_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(buf);
        opal_hotel_checkout(&orte_pmix_server_globals.reqs, req->room_num);
        goto callback;
    }

    /* pack the jdata object */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(buf, &req->jdata, 1, ORTE_JOB))) {
        ORTE_ERROR_LOG(rc);
        opal_hotel_checkout(&orte_pmix_server_globals.reqs, req->room_num);
        OBJ_RELEASE(buf);
        goto callback;

    }

    /* send it to the HNP for processing - might be myself! */
    if (ORTE_SUCCESS != (rc = orte_rml.send_buffer_nb(orte_mgmt_conduit,
                                                      ORTE_PROC_MY_HNP, buf,
                                                      ORTE_RML_TAG_PLM,
                                                      orte_rml_send_callback, NULL))) {
        ORTE_ERROR_LOG(rc);
        opal_hotel_checkout(&orte_pmix_server_globals.reqs, req->room_num);
        OBJ_RELEASE(buf);
        goto callback;
    }
    return;

  callback:
    /* this section gets executed solely upon an error */
    if (NULL != req->spcbfunc) {
        OPAL_PMIX_CONVERT_JOBID(nspace, ORTE_JOBID_INVALID);
        req->spcbfunc(rc, nspace, req->cbdata);
    }
    OBJ_RELEASE(req);
}

static void interim(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    opal_process_name_t *requestor = &cd->proc;
#if PMIX_NUMERIC_VERSION >= 0x00030000
    opal_envar_t envar;
#endif
    orte_job_t *jdata;
    orte_app_context_t *app;
    pmix_app_t *papp;
    pmix_info_t *info;
    opal_list_t *cache;
    int rc, i;
    char cwd[OPAL_PATH_MAX];
    bool flag;
    size_t m, n;
    opal_value_t *kv;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s spawn called from proc %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(requestor));

    /* create the job object */
    jdata = OBJ_NEW(orte_job_t);
    jdata->map = OBJ_NEW(orte_job_map_t);

    /* transfer the apps across */
    for (n=0; n < cd->napps; n++) {
        papp = &cd->apps[n];
        app = OBJ_NEW(orte_app_context_t);
        app->idx = opal_pointer_array_add(jdata->apps, app);
        jdata->num_apps++;
        if (NULL != papp->cmd) {
            app->app = strdup(papp->cmd);
        } else if (NULL == papp->argv ||
                   NULL == papp->argv[0]) {
            ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
            OBJ_RELEASE(jdata);
            rc = ORTE_ERR_BAD_PARAM;
            goto complete;
        } else {
            app->app = strdup(papp->argv[0]);
        }
        if (NULL != papp->argv) {
            app->argv = opal_argv_copy(papp->argv);
        }
        if (NULL != papp->env) {
            app->env = opal_argv_copy(papp->env);
        }
        if (NULL != papp->cwd) {
            app->cwd = strdup(papp->cwd);
        }
        app->num_procs = papp->maxprocs;
        if (NULL != papp->info) {
            for (m=0; m < papp->ninfo; m++) {
                info = &papp->info[m];
                if (PMIX_CHECK_KEY(info, PMIX_HOST)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_DASH_HOST,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_HOSTFILE)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_HOSTFILE,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_HOSTFILE)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_ADD_HOSTFILE,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_HOST)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_ADD_HOST,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_PREFIX)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_PREFIX_DIR,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_WDIR)) {
                    /* if this is a relative path, convert it to an absolute path */
                    if (opal_path_is_absolute(info->value.data.string)) {
                        app->cwd = strdup(info->value.data.string);
                    } else {
                        /* get the cwd */
                        if (OPAL_SUCCESS != (rc = opal_getcwd(cwd, sizeof(cwd)))) {
                            orte_show_help("help-orted.txt", "cwd", true, "spawn", rc);
                            OBJ_RELEASE(jdata);
                            goto complete;
                        }
                        /* construct the absolute path */
                        app->cwd = opal_os_path(false, cwd, info->value.data.string, NULL);
                    }
                } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_BIN)) {
                    flag = PMIX_INFO_TRUE(info);
                    orte_set_attribute(&app->attributes, ORTE_APP_PRELOAD_BIN,
                                       ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);
                } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_FILES)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_PRELOAD_FILES,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);

                } else if (PMIX_CHECK_KEY(info, PMIX_COSPAWN_APP)) {
                    flag = PMIX_INFO_TRUE(info);
                    orte_set_attribute(&app->attributes, ORTE_APP_DEBUGGER_DAEMON,
                                       ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);
#if PMIX_NUMERIC_VERSION >= 0x00030000
                /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
                /* there can be multiple of these, so we add them to the attribute list */
                } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    orte_add_attribute(&app->attributes, ORTE_APP_SET_ENVAR,
                                       ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    orte_add_attribute(&app->attributes, ORTE_APP_ADD_ENVAR,
                                       ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
                    orte_add_attribute(&app->attributes, ORTE_APP_UNSET_ENVAR,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    orte_add_attribute(&app->attributes, ORTE_APP_PREPEND_ENVAR,
                                       ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    orte_add_attribute(&app->attributes, ORTE_APP_APPEND_ENVAR,
                                       ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);

#if PMIX_NUMERIC_VERSION >= 0x00040000
                } else if (PMIX_CHECK_KEY(info, PMIX_PSET_NAME)) {
                    orte_set_attribute(&app->attributes, ORTE_APP_PSET_NAME,
                                       ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
#endif
                } else {
                    /* unrecognized key */
                    orte_show_help("help-orted.txt", "bad-key",
                                   true, "spawn", "application", info->key);
#endif
                }
            }
        }
    }

    /* transfer the job info across */
    for (m=0; m < cd->ninfo; m++) {
        info = &cd->info[m];
        /***   PERSONALITY   ***/
        if (PMIX_CHECK_KEY(info, PMIX_PERSONALITY)) {
            jdata->personality = opal_argv_split(info->value.data.string, ',');

        /***   REQUESTED MAPPER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPPER)) {
            jdata->map->req_mapper = strdup(info->value.data.string);

        /***   DISPLAY MAP   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_MAP)) {
            jdata->map->display_map = PMIX_INFO_TRUE(info);

        /***   PPR (PROCS-PER-RESOURCE)   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_PPR)) {
            if (ORTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
                /* not allowed to provide multiple mapping policies */
                orte_show_help("help-orte-rmaps-base.txt", "redefining-policy",
                               true, "mapping", info->value.data.string,
                               orte_rmaps_base_print_mapping(orte_rmaps_base.mapping));
                rc = ORTE_ERR_BAD_PARAM;
                goto complete;
            }
            ORTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_PPR);
            jdata->map->ppr = strdup(info->value.data.string);

        /***   MAP-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPBY)) {
            if (ORTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
                /* not allowed to provide multiple mapping policies */
                orte_show_help("help-orte-rmaps-base.txt", "redefining-policy",
                               true, "mapping", info->value.data.string,
                               orte_rmaps_base_print_mapping(orte_rmaps_base.mapping));
                rc = ORTE_ERR_BAD_PARAM;
                goto complete;
            }
            rc = orte_rmaps_base_set_mapping_policy(jdata, &jdata->map->mapping,
                                                    NULL, info->value.data.string);
            if (ORTE_SUCCESS != rc) {
                goto complete;
            }
        /***   RANK-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_RANKBY)) {
            if (ORTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
                /* not allowed to provide multiple ranking policies */
                orte_show_help("help-orte-rmaps-base.txt", "redefining-policy",
                               true, "ranking", info->value.data.string,
                               orte_rmaps_base_print_ranking(orte_rmaps_base.ranking));
                rc = ORTE_ERR_BAD_PARAM;
                goto complete;
            }
            rc = orte_rmaps_base_set_ranking_policy(&jdata->map->ranking,
                                                    jdata->map->mapping,
                                                    info->value.data.string);
            if (ORTE_SUCCESS != rc) {
                goto complete;
            }

        /***   BIND-TO   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_BINDTO)) {
            if (OPAL_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                /* not allowed to provide multiple mapping policies */
                orte_show_help("help-opal-hwloc-base.txt", "redefining-policy", true,
                               info->value.data.string,
                               opal_hwloc_base_print_binding(opal_hwloc_binding_policy));
                rc = ORTE_ERR_BAD_PARAM;
                goto complete;
            }
            rc = opal_hwloc_base_set_binding_policy(&jdata->map->binding,
                                                    info->value.data.string);
            if (ORTE_SUCCESS != rc) {
                goto complete;
            }

        /***   CPUS/RANK   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPUS_PER_PROC)) {
            jdata->map->cpus_per_rank = info->value.data.uint32;

        /***   NO USE LOCAL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NO_PROCS_ON_HEAD)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                ORTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_NO_USE_LOCAL);
            } else {
                ORTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_NO_USE_LOCAL);
            }
            /* mark that the user specified it */
            ORTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_LOCAL_GIVEN);

        /***   OVERSUBSCRIBE   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NO_OVERSUBSCRIBE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                ORTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_NO_OVERSUBSCRIBE);
            } else {
                ORTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_NO_OVERSUBSCRIBE);
            }
            /* mark that the user specified it */
            ORTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_SUBSCRIBE_GIVEN);

        /***   REPORT BINDINGS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REPORT_BINDINGS)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_REPORT_BINDINGS,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   CPU LIST  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPU_LIST)) {
            orte_set_attribute(&jdata->attributes, ORTE_JOB_CPU_LIST,
                               ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_BOOL);

        /***   RECOVERABLE  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_RECOVERABLE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                ORTE_FLAG_SET(jdata, ORTE_JOB_FLAG_RECOVERABLE);
            } else {
                ORTE_FLAG_UNSET(jdata, ORTE_JOB_FLAG_RECOVERABLE);
            }

        /***   MAX RESTARTS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAX_RESTARTS)) {
            for (i=0; i < jdata->apps->size; i++) {
                if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                orte_set_attribute(&app->attributes, ORTE_APP_MAX_RESTARTS,
                                   ORTE_ATTR_GLOBAL, &info->value.data.uint32, OPAL_INT32);
            }

        /***   CONTINUOUS OPERATION  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_CONTINUOUS)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_CONTINUOUS_OP,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   NON-PMI JOB   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NON_PMI)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_NON_ORTE_JOB,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   SPAWN REQUESTOR IS TOOL   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REQUESTOR_IS_TOOL)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_DVM_JOB,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);
            if (flag) {
                /* request that IO be forwarded to the requesting tool */
                orte_set_attribute(&jdata->attributes, ORTE_JOB_FWDIO_TO_TOOL,
                                   ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);
            }

        /***   NOTIFY UPON JOB COMPLETION   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NOTIFY_COMPLETION)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_NOTIFY_COMPLETION,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   STOP ON EXEC FOR DEBUGGER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_ON_EXEC)) {
            /* we don't know how to do this */
            rc = ORTE_ERR_NOT_SUPPORTED;
            goto complete;

        /***   TAG STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_TAG_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_TAG_OUTPUT,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   TIMESTAMP OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_TIMESTAMP_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_TIMESTAMP_OUTPUT,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   OUTPUT TO FILES   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_OUTPUT_TO_FILE)) {
            orte_set_attribute(&jdata->attributes, ORTE_JOB_OUTPUT_TO_FILE,
                               ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);

        /***   MERGE STDERR TO STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MERGE_STDERR_STDOUT)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_MERGE_STDERR_STDOUT,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   STDIN TARGET   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_STDIN_TGT)) {
            if (0 == strcmp(info->value.data.string, "all")) {
                jdata->stdin_target = ORTE_VPID_WILDCARD;
            } else if (0 == strcmp(info->value.data.string, "none")) {
                jdata->stdin_target = ORTE_VPID_INVALID;
            } else {
                jdata->stdin_target = strtoul(info->value.data.string, NULL, 10);
            }

        /***   INDEX ARGV   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_INDEX_ARGV)) {
            flag = PMIX_INFO_TRUE(info);
            orte_set_attribute(&jdata->attributes, ORTE_JOB_INDEX_ARGV,
                               ORTE_ATTR_GLOBAL, &flag, OPAL_BOOL);

        /***   DEBUGGER DAEMONS   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUGGER_DAEMONS)) {
            ORTE_FLAG_SET(jdata, ORTE_JOB_FLAG_DEBUGGER_DAEMON);
            ORTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, ORTE_MAPPING_DEBUGGER);

#if PMIX_NUMERIC_VERSION >= 0x00030000
        /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
        /* there can be multiple of these, so we add them to the attribute list */
        } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            orte_add_attribute(&jdata->attributes, ORTE_JOB_SET_ENVAR,
                               ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            orte_add_attribute(&jdata->attributes, ORTE_JOB_ADD_ENVAR,
                               ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
            orte_add_attribute(&jdata->attributes, ORTE_JOB_UNSET_ENVAR,
                               ORTE_ATTR_GLOBAL, info->value.data.string, OPAL_STRING);
        } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            orte_add_attribute(&jdata->attributes, ORTE_JOB_PREPEND_ENVAR,
                               ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            orte_add_attribute(&jdata->attributes, ORTE_JOB_APPEND_ENVAR,
                               ORTE_ATTR_GLOBAL, &envar, OPAL_ENVAR);
#endif
#if PMIX_NUMERIC_VERSION >= 0x00040000
        } else if (PMIX_CHECK_KEY(info, PMIX_SPAWN_TOOL)) {
            ORTE_FLAG_SET(jdata, ORTE_JOB_FLAG_TOOL);
#endif
        /***   DEFAULT - CACHE FOR INCLUSION WITH JOB INFO   ***/
        } else {
            /* cache for inclusion with job info at registration */
            kv = OBJ_NEW(opal_value_t);
            kv->key = strdup(info->key);
            opal_pmix_value_unload(kv, &info->value);
            if (orte_get_attribute(&jdata->attributes, ORTE_JOB_INFO_CACHE, (void**)&cache, OPAL_PTR)) {
                opal_list_append(cache, &kv->super);
            } else {
                cache = OBJ_NEW(opal_list_t);
                opal_list_append(cache, &kv->super);
                orte_set_attribute(&jdata->attributes, ORTE_JOB_INFO_CACHE, ORTE_ATTR_LOCAL, (void*)cache, OPAL_PTR);
            }
        }
    }
    /* if the job is missing a personality setting, add it */
    if (NULL == jdata->personality) {
        opal_argv_append_nosize(&jdata->personality, "ompi");
    }

    /* indicate the requestor so bookmarks can be correctly set */
    orte_set_attribute(&jdata->attributes, ORTE_JOB_LAUNCH_PROXY,
                       ORTE_ATTR_GLOBAL, requestor, OPAL_NAME);

    /* setup a spawn tracker so we know who to call back when this is done
     * and thread-shift the entire thing so it can be safely added to
     * our tracking list */
    ORTE_SPN_REQ(jdata, spawn, cd->spcbfunc, cd->cbdata);
    OBJ_RELEASE(cd);
    return;

  complete:
    if (NULL != cd->spcbfunc) {
        pmix_proc_t pproc;
        OPAL_PMIX_CONVERT_JOBID(pproc.nspace, ORTE_JOBID_INVALID);
        cd->spcbfunc(rc, pproc.nspace, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

int pmix_server_spawn_fn(const pmix_proc_t *proc,
                         const pmix_info_t job_info[], size_t ninfo,
                         const pmix_app_t apps[], size_t napps,
                         pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd;
    int rc;

    cd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    OPAL_PMIX_CONVERT_NSPACE(rc, &cd->proc.jobid, proc->nspace);
    if (ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return PMIX_ERR_BAD_PARAM;
    }
    OPAL_PMIX_CONVERT_RANK(cd->proc.vpid, proc->rank);
    cd->info = (pmix_info_t*)job_info;
    cd->ninfo = ninfo;
    cd->apps = (pmix_app_t*)apps;
    cd->napps = napps;
    cd->spcbfunc = cbfunc;
    cd->cbdata = cbdata;
    opal_event_set(orte_event_base, &cd->ev, -1,
                   OPAL_EV_WRITE, interim, cd);
    opal_event_set_priority(&cd->ev, ORTE_MSG_PRI);
    ORTE_POST_OBJECT(cd);
    opal_event_active(&cd->ev, OPAL_EV_WRITE, 1);
    return ORTE_SUCCESS;
}

static void _cnct(int sd, short args, void *cbdata);

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;
    lock->status = status;
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

static void _cnlk(pmix_status_t status,
                  pmix_pdata_t data[], size_t ndata,
                  void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    int cnt;
    orte_job_t *jdata;
    pmix_status_t ret;
    pmix_data_buffer_t pbkt;
    opal_pmix_lock_t lock;
    pmix_info_t  *info = NULL;
    size_t ninfo;

    ORTE_ACQUIRE_OBJECT(cd);

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
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
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

    /* we have to process the data to convert it into an orte_job_t
     * that describes this job as we didn't already have it */
    jdata = OBJ_NEW(orte_job_t);

    /* register the data with the local server */
    OPAL_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(data[0].proc.nspace,
                                      jdata->num_local_procs,
                                      info, ninfo, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        OPAL_PMIX_DESTRUCT_LOCK(&lock);
        goto release;
    }
    OPAL_PMIX_WAIT_THREAD(&lock);
    ret = lock.status;
    OPAL_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(info, ninfo);

    /* restart the cnct processor */
    ORTE_PMIX_OPERATION(cd->procs, cd->nprocs, cd->info, cd->ninfo, _cnct, cd->cbfunc, cd->cbdata);
    /* protect the re-referenced data */
    cd->procs = NULL;
    cd->info = NULL;
    OBJ_RELEASE(cd);
    return;

  release:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(ret, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

static void _cnct(int sd, short args, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;
    char **keys = NULL;
    orte_job_t *jdata;
    int rc = ORTE_SUCCESS;
    size_t n;
    orte_jobid_t jobid;
    uint32_t uid;

    ORTE_ACQUIRE_OBJECT(cd);

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
        OPAL_PMIX_CONVERT_NSPACE(rc, &jobid, cd->procs[n].nspace);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            goto release;
        }
        /* see if we have the job object for this job */
        if (NULL == (jdata = orte_get_job_data_object(jobid))) {
            /* we don't know about this job. If our "global" data
             * server is just our HNP, then we have no way of finding
             * out about it, and all we can do is return an error */
            if (orte_pmix_server_globals.server.jobid == ORTE_PROC_MY_HNP->jobid &&
                orte_pmix_server_globals.server.vpid == ORTE_PROC_MY_HNP->vpid) {
                rc = ORTE_ERR_NOT_SUPPORTED;
                goto release;
            }
            /* ask the global data server for the data - if we get it,
             * then we can complete the request */
            opal_argv_append_nosize(&keys, cd->procs[n].nspace);
            /* we have to add the user's id to the directives */
            cd->ndirs = 1;
            PMIX_INFO_CREATE(cd->directives, cd->ndirs);
            uid = geteuid();
            PMIX_INFO_LOAD(&cd->directives[0], PMIX_USERID, &uid, OPAL_UINT32);
            if (ORTE_SUCCESS != (rc = pmix_server_lookup_fn(&cd->procs[n], keys,
                                                            cd->directives, cd->ndirs, _cnlk, cd))) {
                opal_argv_free(keys);
                PMIX_INFO_FREE(cd->directives, cd->ndirs);
                goto release;
            }
            opal_argv_free(keys);
            /* the callback function on this lookup will return us to this
             * routine so we can continue the process */
            return;
        }
        /* we know about the job - check to ensure it has been
         * registered with the local PMIx server */
        if (!orte_get_attribute(&jdata->attributes, ORTE_JOB_NSPACE_REGISTERED, NULL, OPAL_BOOL)) {
            /* it hasn't been registered yet, so register it now */
            if (ORTE_SUCCESS != (rc = orte_pmix_server_register_nspace(jdata))) {
                goto release;
            }
        }
    }

  release:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

pmix_status_t pmix_server_connect_fn(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_pmix_server_op_caddy_t *op;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s connect called with %d procs",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (int)nprocs);

    /* protect ourselves */
    if (NULL == procs || 0 == nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* must thread shift this as we will be accessing global data */
    op = OBJ_NEW(orte_pmix_server_op_caddy_t);
    op->procs = (pmix_proc_t*)procs;
    op->nprocs = nprocs;
    op->info = (pmix_info_t*)info;
    op->ninfo = ninfo;
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    opal_event_set(orte_event_base, &(op->ev), -1,
                   OPAL_EV_WRITE, _cnct, op);
    opal_event_set_priority(&(op->ev), ORTE_MSG_PRI);
    ORTE_POST_OBJECT(op);
    opal_event_active(&(op->ev), OPAL_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void mdxcbfunc(pmix_status_t status,
                      const char *data, size_t ndata, void *cbdata,
                      pmix_release_cbfunc_t relcbfunc, void *relcbdata)
{
    orte_pmix_server_op_caddy_t *cd = (orte_pmix_server_op_caddy_t*)cbdata;

    ORTE_ACQUIRE_OBJECT(cd);
    /* ack the call */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(status, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

pmix_status_t pmix_server_disconnect_fn(const pmix_proc_t procs[], size_t nprocs,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    orte_pmix_server_op_caddy_t *cd;
    pmix_status_t rc;

    opal_output_verbose(2, orte_pmix_server_globals.output,
                        "%s disconnect called",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* at some point, we need to add bookeeping to track which
     * procs are "connected" so we know who to notify upon
     * termination or failure. For now, just execute a fence
     * Note that we do not need to thread-shift here as the
     * fence function will do it for us */
    cd = OBJ_NEW(orte_pmix_server_op_caddy_t);
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    if (PMIX_SUCCESS != (rc = pmix_server_fencenb_fn(procs, nprocs,
                                                     info, ninfo,
                                                     NULL, 0,
                                                     mdxcbfunc, cd))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cd);
    }

    return rc;
}

pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                   pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    /* ORTE currently has no way of supporting allocation requests */
    return ORTE_ERR_NOT_SUPPORTED;
}
