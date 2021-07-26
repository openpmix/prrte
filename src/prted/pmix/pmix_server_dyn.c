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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include "src/util/argv.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/prte_getcwd.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

void pmix_server_notify_spawn(pmix_nspace_t jobid, int room, pmix_status_t ret)
{
    pmix_server_req_t *req;
    prte_job_t *jdata;

    jdata = prte_get_job_data_object(jobid);
    if (NULL != jdata &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED, NULL, PMIX_BOOL)) {
        /* already done */
        return;
    }

    /* retrieve the request */
    prte_hotel_checkout_and_return_occupant(&prte_pmix_server_globals.reqs, room, (void **) &req);
    if (NULL == req) {
        /* we are hosed */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }

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
    PRTE_RELEASE(req);

    /* mark that we sent it */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_SPAWN_NOTIFIED,
                       PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
}
void pmix_server_launch_resp(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                             prte_rml_tag_t tg, void *cbdata)
{
    int rc, room;
    int32_t ret, cnt;
    pmix_nspace_t jobid;

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
    pmix_server_req_t *req = (pmix_server_req_t *) cbdata;
    int rc;
    pmix_data_buffer_t *buf;
    prte_plm_cmd_flag_t command;
    char nspace[PMIX_MAX_NSLEN + 1];
    pmix_status_t prc;

    PRTE_ACQUIRE_OBJECT(req);

    /* add this request to our tracker hotel */
    PRTE_ADJUST_TIMEOUT(req);
    if (PRTE_SUCCESS
        != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
        prte_show_help("help-prted.txt", "noroom", true, req->operation,
                       prte_pmix_server_globals.num_rooms);
        goto callback;
    }

    /* include the request room number for quick retrieval */
    prte_set_attribute(&req->jdata->attributes, PRTE_JOB_ROOM_NUM, PRTE_ATTR_GLOBAL, &req->room_num,
                       PMIX_INT);

    /* construct a spawn message */
    PMIX_DATA_BUFFER_CREATE(buf);

    command = PRTE_PLM_LAUNCH_JOB_CMD;
    rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        goto callback;
    }

    /* pack the jdata object */
    rc = prte_job_pack(buf, req->jdata);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }

    /* send it to the HNP for processing - might be myself! */
    if (PRTE_SUCCESS
        != (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_PLM,
                                         prte_rml_send_callback, NULL))) {
        PRTE_ERROR_LOG(rc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
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
    PRTE_RELEASE(req);
}

static int pmix_server_cache_job_info(prte_job_t *jdata, pmix_info_t *info);

static void interim(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    pmix_proc_t *requestor = &cd->proc;
    pmix_envar_t envar;
    prte_job_t *jdata;
    prte_app_context_t *app;
    pmix_app_t *papp;
    pmix_info_t *info;
    int rc, i;
    char cwd[PRTE_PATH_MAX];
    bool flag;
    size_t m, n;
    uint16_t u16;
    pmix_rank_t rank;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s spawn called from proc %s with %d apps",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(requestor),
                        (int) cd->napps);

    /* create the job object */
    jdata = PRTE_NEW(prte_job_t);
    jdata->map = PRTE_NEW(prte_job_map_t);
    /* default to the requestor as the originator */
    PMIX_LOAD_PROCID(&jdata->originator, requestor->nspace, requestor->rank);

    /* transfer the apps across */
    for (n = 0; n < cd->napps; n++) {
        papp = &cd->apps[n];
        app = PRTE_NEW(prte_app_context_t);
        app->idx = prte_pointer_array_add(jdata->apps, app);
        jdata->num_apps++;
        if (NULL != papp->cmd) {
            app->app = strdup(papp->cmd);
        } else if (NULL == papp->argv || NULL == papp->argv[0]) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            PRTE_RELEASE(jdata);
            rc = PRTE_ERR_BAD_PARAM;
            goto complete;
        } else {
            app->app = strdup(papp->argv[0]);
        }
        if (NULL != papp->argv) {
            app->argv = prte_argv_copy(papp->argv);
        }
        if (NULL != papp->env) {
            app->env = prte_argv_copy(papp->env);
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
                    prte_set_attribute(&app->attributes, PRTE_APP_PREFIX_DIR, PRTE_ATTR_GLOBAL,
                                       info->value.data.string, PMIX_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_WDIR)) {
                    /* if this is a relative path, convert it to an absolute path */
                    if (prte_path_is_absolute(info->value.data.string)) {
                        app->cwd = strdup(info->value.data.string);
                    } else {
                        /* get the cwd */
                        if (PRTE_SUCCESS != (rc = prte_getcwd(cwd, sizeof(cwd)))) {
                            prte_show_help("help-prted.txt", "cwd", true, "spawn", rc);
                            PRTE_RELEASE(jdata);
                            goto complete;
                        }
                        /* construct the absolute path */
                        app->cwd = prte_os_path(false, cwd, info->value.data.string, NULL);
                    }
                } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_BIN)) {
                    flag = PMIX_INFO_TRUE(info);
                    prte_set_attribute(&app->attributes, PRTE_APP_PRELOAD_BIN, PRTE_ATTR_GLOBAL,
                                       &flag, PMIX_BOOL);
                } else if (PMIX_CHECK_KEY(info, PMIX_PRELOAD_FILES)) {
                    prte_set_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES, PRTE_ATTR_GLOBAL,
                                       info->value.data.string, PMIX_STRING);

                    /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
                    /* there can be multiple of these, so we add them to the attribute list */
                } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prte_add_attribute(&app->attributes, PRTE_APP_SET_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envar, PMIX_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prte_add_attribute(&app->attributes, PRTE_APP_ADD_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envar, PMIX_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
                    prte_add_attribute(&app->attributes, PRTE_APP_UNSET_ENVAR, PRTE_ATTR_GLOBAL,
                                       info->value.data.string, PMIX_STRING);
                } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prte_add_attribute(&app->attributes, PRTE_APP_PREPEND_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envar, PMIX_ENVAR);
                } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
                    envar.envar = info->value.data.envar.envar;
                    envar.value = info->value.data.envar.value;
                    envar.separator = info->value.data.envar.separator;
                    prte_add_attribute(&app->attributes, PRTE_APP_APPEND_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envar, PMIX_ENVAR);

                } else if (PMIX_CHECK_KEY(info, PMIX_PSET_NAME)) {
                    prte_set_attribute(&app->attributes, PRTE_APP_PSET_NAME, PRTE_ATTR_GLOBAL,
                                       info->value.data.string, PMIX_STRING);
                } else {
                    /* unrecognized key */
                    if (9 < prte_output_get_verbosity(prte_pmix_server_globals.output)) {
                        prte_show_help("help-prted.txt", "bad-key", true, "spawn", "application",
                                       info->key);
                    }
                }
            }
        }
    }

    /* transfer the job info across */
    for (m = 0; m < cd->ninfo; m++) {
        info = &cd->info[m];
        /***   PERSONALITY   ***/
        if (PMIX_CHECK_KEY(info, PMIX_PERSONALITY)) {
            jdata->personality = prte_argv_split(info->value.data.string, ',');
            pmix_server_cache_job_info(jdata, info);

            /***   REQUESTED MAPPER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPPER)) {
            jdata->map->req_mapper = strdup(info->value.data.string);

            /***   DISPLAY MAP   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DISPLAY_MAP)) {
            if (PMIX_INFO_TRUE(info)) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, PRTE_ATTR_GLOBAL, NULL,
                                   PMIX_BOOL);
            }

            /***   PPR (PROCS-PER-RESOURCE)   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_PPR)) {
            if (PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
                /* not allowed to provide multiple mapping policies */
                prte_show_help("help-prte-rmaps-base.txt", "redefining-policy", true, "mapping",
                               info->value.data.string,
                               prte_rmaps_base_print_mapping(prte_rmaps_base.mapping));
                rc = PRTE_ERR_BAD_PARAM;
                goto complete;
            }
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_PPR);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

            /***   MAP-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAPBY)) {
            rc = prte_rmaps_base_set_mapping_policy(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                goto complete;
            }
            /***   RANK-BY   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_RANKBY)) {
            rc = prte_rmaps_base_set_ranking_policy(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                goto complete;
            }

            /***   BIND-TO   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_BINDTO)) {
            rc = prte_hwloc_base_set_binding_policy(jdata, info->value.data.string);
            if (PRTE_SUCCESS != rc) {
                goto complete;
            }

            /***   CPUS/RANK   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPUS_PER_PROC)) {
            u16 = info->value.data.uint32;
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, &u16,
                               PMIX_UINT16);

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

            /***   REPORT BINDINGS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_REPORT_BINDINGS)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_REPORT_BINDINGS, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   CPU LIST  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_CPU_LIST)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);

            /***   RECOVERABLE  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_RECOVERABLE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_RECOVERABLE);
            } else {
                PRTE_FLAG_UNSET(jdata, PRTE_JOB_FLAG_RECOVERABLE);
            }

            /***   MAX RESTARTS  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_MAX_RESTARTS)) {
            for (i = 0; i < jdata->apps->size; i++) {
                if (NULL
                    == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                prte_set_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, PRTE_ATTR_GLOBAL,
                                   &info->value.data.uint32, PMIX_INT32);
            }

            /***   CONTINUOUS OPERATION  ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_JOB_CONTINUOUS)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS_OP, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

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
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DVM_JOB, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);
            if (flag) {
                /* request that IO be forwarded to the requesting tool */
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FWDIO_TO_TOOL, PRTE_ATTR_GLOBAL,
                                   &flag, PMIX_BOOL);
            }

            /***   NOTIFY UPON JOB COMPLETION   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NOTIFY_COMPLETION)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_NOTIFY_COMPLETION, PRTE_ATTR_GLOBAL,
                               &flag, PMIX_BOOL);

            /***   STOP ON EXEC FOR DEBUGGER   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_ON_EXEC)) {
#if PRTE_HAVE_STOP_ON_EXEC
            if (PMIX_PROC_RANK == info->value.type) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_ON_EXEC, PRTE_ATTR_GLOBAL,
                                   &info->value.data.rank, PMIX_PROC_RANK);
            } else if (PMIX_BOOL == info->value.type) {
                flag = PMIX_INFO_TRUE(info);
                if (flag) {
                    rank = PMIX_RANK_WILDCARD;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_ON_EXEC, PRTE_ATTR_GLOBAL,
                                       &rank, PMIX_PROC_RANK);
                }
            } else {
                /* we cannot support the request at this time */
                rc = PRTE_ERR_NOT_SUPPORTED;
                goto complete;
            }
#else
            /* we cannot support the request */
            rc = PRTE_ERR_NOT_SUPPORTED;
            goto complete;
#endif

        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_IN_INIT)) {
            if (PMIX_PROC_RANK == info->value.type) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_INIT, PRTE_ATTR_GLOBAL,
                                   &info->value.data.rank, PMIX_PROC_RANK);
            } else if (PMIX_BOOL == info->value.type) {
                flag = PMIX_INFO_TRUE(info);
                if (flag) {
                    rank = PMIX_RANK_WILDCARD;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_INIT, PRTE_ATTR_GLOBAL,
                                       &rank, PMIX_PROC_RANK);
                }
            } else {
                /* we cannot support the request at this time */
                rc = PRTE_ERR_NOT_SUPPORTED;
                goto complete;
            }
            /* also must add to job-level cache */
            pmix_server_cache_job_info(jdata, info);

        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_STOP_IN_APP)) {
            if (PMIX_PROC_RANK == info->value.type) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, PRTE_ATTR_GLOBAL,
                                   &info->value.data.rank, PMIX_PROC_RANK);
            } else if (PMIX_BOOL == info->value.type) {
                flag = PMIX_INFO_TRUE(info);
                if (flag) {
                    rank = PMIX_RANK_WILDCARD;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, PRTE_ATTR_GLOBAL,
                                       &rank, PMIX_PROC_RANK);
                }
            } else {
                /* we cannot support the request at this time */
                rc = PRTE_ERR_NOT_SUPPORTED;
                goto complete;
            }
            /* also must add to job-level cache */
            pmix_server_cache_job_info(jdata, info);

            /***   TAG STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_OUTPUT) ||
                   PMIX_CHECK_KEY(info, PMIX_TAG_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /***   RANK STDOUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_RANK_OUTPUT)) {
            flag = PMIX_INFO_TRUE(info);
            prte_set_attribute(&jdata->attributes, PRTE_JOB_RANK_OUTPUT, PRTE_ATTR_GLOBAL, &flag,
                               PMIX_BOOL);

            /***   TIMESTAMP OUTPUT   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TIMESTAMP_OUTPUT)
                   || PMIX_CHECK_KEY(info, PMIX_TIMESTAMP_OUTPUT)) {
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
            for (n=0; n < (size_t)jdata->apps->size; n++) {
                app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, n);
                if (NULL != app) {
                    PRTE_FLAG_SET(app, PRTE_APP_FLAG_TOOL);
                }
            }

            /***   CO-LOCATE TARGET FOR DEBUGGER DAEMONS    ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_TARGET)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DEBUG_TARGET, PRTE_ATTR_GLOBAL,
                               info->value.data.proc, PMIX_PROC);
            pmix_server_cache_job_info(jdata, info);

            /***   NUMBER OF DEBUGGER_DAEMONS PER NODE   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_DAEMONS_PER_NODE)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_NODE,
                               PRTE_ATTR_GLOBAL, &info->value.data.uint16, PMIX_UINT16);

            /***   NUMBER OF DEBUGGER_DAEMONS PER PROC   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_DEBUG_DAEMONS_PER_PROC)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_PROC,
                               PRTE_ATTR_GLOBAL, &info->value.data.uint16, PMIX_UINT16);

            /***   ENVIRONMENTAL VARIABLE DIRECTIVES   ***/
            /* there can be multiple of these, so we add them to the attribute list */
        } else if (PMIX_CHECK_KEY(info, PMIX_ENVARS_HARVESTED)) {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_ENVARS_HARVESTED,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
        } else if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_add_attribute(&jdata->attributes, PRTE_JOB_SET_ENVAR,
                               PRTE_ATTR_GLOBAL, &envar, PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_add_attribute(&jdata->attributes, PRTE_JOB_ADD_ENVAR, PRTE_ATTR_GLOBAL, &envar,
                               PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
            prte_add_attribute(&jdata->attributes, PRTE_JOB_UNSET_ENVAR, PRTE_ATTR_GLOBAL,
                               info->value.data.string, PMIX_STRING);
        } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_add_attribute(&jdata->attributes, PRTE_JOB_PREPEND_ENVAR, PRTE_ATTR_GLOBAL, &envar,
                               PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
            envar.envar = info->value.data.envar.envar;
            envar.value = info->value.data.envar.value;
            envar.separator = info->value.data.envar.separator;
            prte_add_attribute(&jdata->attributes, PRTE_JOB_APPEND_ENVAR, PRTE_ATTR_GLOBAL, &envar,
                               PMIX_ENVAR);
        } else if (PMIX_CHECK_KEY(info, PMIX_SPAWN_TOOL)) {
            PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);
            for (n=0; n < (size_t)jdata->apps->size; n++) {
                app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, n);
                if (NULL != app) {
                    PRTE_FLAG_SET(app, PRTE_APP_FLAG_TOOL);
                }
            }

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT)) {
            prte_add_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT, PRTE_ATTR_GLOBAL,
                               &info->value.data.integer, PMIX_INT);

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT_STACKTRACES)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                prte_add_attribute(&jdata->attributes, PRTE_JOB_STACKTRACES, PRTE_ATTR_GLOBAL,
                                   &flag, PMIX_BOOL);
            }

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT_REPORT_STATE)) {
            flag = PMIX_INFO_TRUE(info);
            if (flag) {
                prte_add_attribute(&jdata->attributes, PRTE_JOB_REPORT_STATE, PRTE_ATTR_GLOBAL,
                                   &flag, PMIX_BOOL);
            }
            /***   DEFAULT - CACHE FOR INCLUSION WITH JOB INFO   ***/
        } else {
            pmix_server_cache_job_info(jdata, info);
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
    PRTE_RELEASE(cd);
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
    PRTE_RELEASE(cd);
}

static int pmix_server_cache_job_info(prte_job_t *jdata, pmix_info_t *info)
{
    prte_info_item_t *kv;
    prte_list_t *cache;

    /* cache for inclusion with job info at registration */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_XFER(&kv->info, info);
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, (void **) &cache,
                           PMIX_POINTER)) {
        prte_list_append(cache, &kv->super);
    } else {
        cache = PRTE_NEW(prte_list_t);
        prte_list_append(cache, &kv->super);
        prte_set_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, PRTE_ATTR_LOCAL, (void *) cache,
                           PMIX_POINTER);
    }
    return 0;
}

int pmix_server_spawn_fn(const pmix_proc_t *proc, const pmix_info_t job_info[], size_t ninfo,
                         const pmix_app_t apps[], size_t napps, pmix_spawn_cbfunc_t cbfunc,
                         void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s spawn upcalled on behalf of proc %s:%u with %" PRIsize_t " job infos",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), proc->nspace, proc->rank, ninfo);

    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);
    PMIX_LOAD_PROCID(&cd->proc, proc->nspace, proc->rank);
    cd->info = (pmix_info_t *) job_info;
    cd->ninfo = ninfo;
    cd->apps = (pmix_app_t *) apps;
    cd->napps = napps;
    cd->spcbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, interim, cd);
    prte_event_set_priority(&cd->ev, PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void _cnct(int sd, short args, void *cbdata);

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    lock->status = status;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void _cnlk(pmix_status_t status, pmix_pdata_t data[], size_t ndata, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    int cnt;
    prte_job_t *jdata;
    pmix_status_t ret;
    pmix_data_buffer_t pbkt;
    prte_pmix_lock_t lock;
    pmix_info_t *info = NULL;
    size_t ninfo;

    PRTE_ACQUIRE_OBJECT(cd);

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
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        ret = PMIX_ERR_BAD_PARAM;
        goto release;
    }

    /* the data will consist of a byte object containing
     * a packed buffer of the job data */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    ret = PMIx_Data_load(&pbkt, &data[0].value.data.bo);
    if (PMIX_SUCCESS != ret) {
        goto release;
    }
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

    /* we have to process the data to convert it into an prte_job_t
     * that describes this job as we didn't already have it */
    jdata = PRTE_NEW(prte_job_t);

    /* register the data with the local server */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(data[0].proc.nspace, jdata->num_local_procs, info, ninfo,
                                      opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        goto release;
    }
    PRTE_PMIX_WAIT_THREAD(&lock);
    ret = lock.status;
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(info, ninfo);

    /* restart the cnct processor */
    PRTE_PMIX_OPERATION(cd->procs, cd->nprocs, cd->info, cd->ninfo, _cnct, cd->cbfunc, cd->cbdata);
    /* we don't need to protect the re-referenced data as
     * the prte_pmix_server_op_caddy_t does not have
     * a destructor! */
    PRTE_RELEASE(cd);
    return;

release:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(ret, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

static void cndbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;
    lock->status = status;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void connect_release(int status, pmix_data_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *md = (prte_pmix_mdx_caddy_t*)cbdata;
    pmix_nspace_t nspace;
    pmix_info_t info[2];
    int rc = PMIX_SUCCESS;
    int cnt, n=0;
    prte_pmix_lock_t lock;
    uint32_t ctxid;
    bool first = true;

    PRTE_ACQUIRE_OBJECT(cd);

    /* process returned data */
    if (NULL != buf && 0 != buf->bytes_used) {
        /* unpack the context ID */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &ctxid, &cnt, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* pass it down */
        PMIX_INFO_LOAD(&info[1], PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_UINT32);

        /* there is a byte object for each proc in the connect operation */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &nspace, &cnt, PMIX_PROC_NSPACE);
        while (PMIX_SUCCESS == rc) {
            ++n;
           /* unpack the proc data for this entry */
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, buf, &info[0], &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto next;
            }
            if (first) {
                cnt = 2;
                first = false;
            } else {
                cnt = 1;
            }
            /* use the register_nspace API to enter this information - it
             * will see that it already knows the nspace, and so it will
             * simply cache the data for later retrieval when requested */
            PRTE_PMIX_CONSTRUCT_LOCK(&lock);
            rc = PMIx_server_register_nspace(nspace, -1, info, cnt, cndbfunc, &lock);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PRTE_PMIX_DESTRUCT_LOCK(&lock);
            } else {
                PRTE_PMIX_WAIT_THREAD(&lock);
                rc = lock.status;
                PRTE_PMIX_DESTRUCT_LOCK(&lock);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_DESTRUCT(&info[0]);
        next:
            /* get the next nspace */
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, buf, &nspace, &cnt, PMIX_PROC_NSPACE);
        }
        PMIX_INFO_DESTRUCT(&info[1]);
    }

    /* now release the connect call */
    if (NULL != md->opcbfunc) {
        md->opcbfunc(status, md->cbdata);
    }

    PRTE_RELEASE(md);
}

static void _cnct(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    char **keys = NULL;
    prte_job_t *jdata;
    int rc = PRTE_SUCCESS;
    size_t k, n, ninfo;
    int m;
    uint32_t uid;
    pmix_value_t *val;
    pmix_info_t info[2], *isrc, *idest, procdata;
    prte_proc_t *proc;
    pmix_data_buffer_t dbuf;
    pmix_data_array_t *darray;
    pmix_scope_t scope;
    prte_pmix_mdx_caddy_t *md;

    PRTE_ACQUIRE_OBJECT(cd);

    /* at some point, we need to add bookeeping to track which
     * procs are "connected" so we know who to notify upon
     * termination or failure. For now, we have to ensure
     * that we have registered all participating nspaces so
     * the embedded PMIx server can provide them to the client.
     * Otherwise, the client will receive an error as it won't
     * be able to resolve any of the required data for the
     * missing nspaces */

    /* cycle thru the procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    PMIX_INFO_LOAD(&info[0], PMIX_OPTIONAL, NULL, PMIX_BOOL);
    scope = PMIX_REMOTE;
    PMIX_INFO_LOAD(&info[1], PMIX_DATA_SCOPE, &scope, PMIX_SCOPE);
    for (n = 0; n < cd->nprocs; n++) {
        /* see if we have the job object for this job */
        if (NULL == (jdata = prte_get_job_data_object(cd->procs[n].nspace))) {
            /* we don't know about this job. If our "global" data
             * server is just our HNP, then we have no way of finding
             * out about it, and all we can do is return an error */
            if (PMIX_CHECK_PROCID(&prte_pmix_server_globals.server, PRTE_PROC_MY_HNP)) {
                rc = PRTE_ERR_NOT_SUPPORTED;
                goto release;
            }
            /* ask the global data server for the data - if we get it,
             * then we can complete the request */
            prte_argv_append_nosize(&keys, cd->procs[n].nspace);
            /* we have to add the user's id to the directives */
            cd->ndirs = 1;
            PMIX_INFO_CREATE(cd->directives, cd->ndirs);
            uid = geteuid();
            PMIX_INFO_LOAD(&cd->directives[0], PMIX_USERID, &uid, PMIX_UINT32);
            if (PRTE_SUCCESS
                != (rc = pmix_server_lookup_fn(&cd->procs[n], keys, cd->directives, cd->ndirs,
                                               _cnlk, cd))) {
                prte_argv_free(keys);
                PMIX_INFO_FREE(cd->directives, cd->ndirs);
                goto release;
            }
            prte_argv_free(keys);
            /* the callback function on this lookup will return us to this
             * routine so we can continue the process */
            return;
        }
        /* we know about the job - check to ensure it has been
         * registered with the local PMIx server */
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_NSPACE_REGISTERED, NULL, PMIX_BOOL)) {
            /* it hasn't been registered yet, so register it now */
            if (PRTE_SUCCESS != (rc = prte_pmix_server_register_nspace(jdata))) {
                goto release;
            }
        }
        /* cycle thru our local children and collect any info they have posted */
        for (m=0; m < prte_local_children->size; m++) {
            if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, m))) {
                continue;
            }
            if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                continue;
            }
            /* we cannot use PMIx_server_dmodex_request here as it will wait
             * for the requested data to be posted by the process - this can lead
             * to a "hang" should that process never post something. So use
             * PMIx_Get instead as we can then pass an "optional" flag to it.
             * Note also that we don't need any local data as the server can
             * already provide it */
            rc = PMIx_Get(&proc->name, NULL, info, 2, &val);
            if (PMIX_SUCCESS != rc) {
                /* if the proc didn't post any remote data, then we will
                 * get a "not found" response - this is okay */
                continue;
            }
            /* we should have received a data array containing all the requested info */
            /* first pack the nspace */
            rc = PMIx_Data_pack(NULL, &dbuf, &jdata->nspace, 1, PMIX_PROC_NSPACE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
                PMIX_VALUE_RELEASE(val);
                goto release;
            }
            /* transfer the returned data to a data array suitable for PMIX_PROC_DATA */
            ninfo = 1 + val->data.darray->size;
            PMIX_DATA_ARRAY_CREATE(darray, ninfo, PMIX_INFO);
            idest = (pmix_info_t*)darray->array;
            /* the array starts with the proc's rank */
            PMIX_INFO_LOAD(&idest[0], PMIX_RANK, &proc->name.rank, PMIX_PROC_RANK);
            /* now transfer the returned data */
            isrc = (pmix_info_t*)val->data.darray->array;
            for (k=1; k < ninfo; k++) {
                PMIX_INFO_XFER(&idest[k], &isrc[k-1]);
            }
            PMIX_VALUE_RELEASE(val);
            /* load the proc_data info */
            PMIX_INFO_LOAD(&procdata, PMIX_PROC_DATA, darray, PMIX_DATA_ARRAY);
            /* now pack it */
            rc = PMIx_Data_pack(NULL, &dbuf, &procdata, 1, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
                goto release;
            }
       }
    }
    /* PMIx server only calls us if this is a multi-node operation. In this
     * case, we also need to (a) execute a "fence" across the participating
     * nodes, (b) send along any information posted by the participants
     * for "remote" scope, and (c) request assignment of a unique context ID
     * that the app can use for things like a communicator ID */
    md = PRTE_NEW(prte_pmix_mdx_caddy_t);
    md->sig = PRTE_NEW(prte_grpcomm_signature_t);
    md->sig->sz = cd->nprocs;
    md->sig->signature = (pmix_proc_t *) malloc(md->sig->sz * sizeof(pmix_proc_t));
    memcpy(md->sig->signature, cd->procs, md->sig->sz * sizeof(pmix_proc_t));
    md->opcbfunc = cd->cbfunc;
    md->cbdata = cd->cbdata;
    PRTE_RELEASE(cd);
    cd = NULL;

    /* pass it to the global collective algorithm */
    /* pass along any data that was collected locally */
    rc = prte_grpcomm.allgather(md->sig, &dbuf, 1, connect_release, md);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(md);
        PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
        goto release;
    }
    return;

release:
    rc = prte_pmix_convert_rc(rc);
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    if (NULL != cd) {
        PRTE_RELEASE(cd);
    }
}

pmix_status_t pmix_server_connect_fn(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *op;

    prte_output_verbose(2, prte_pmix_server_globals.output, "%s connect called with %d procs",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nprocs);

    /* protect ourselves */
    if (NULL == procs || 0 == nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* must thread shift this as we will be accessing global data */
    op = PRTE_NEW(prte_pmix_server_op_caddy_t);
    op->procs = (pmix_proc_t *) procs;
    op->nprocs = nprocs;
    op->info = (pmix_info_t *) info;
    op->ninfo = ninfo;
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    prte_event_set(prte_event_base, &(op->ev), -1, PRTE_EV_WRITE, _cnct, op);
    prte_event_set_priority(&(op->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(op);
    prte_event_active(&(op->ev), PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void mdxcbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                      pmix_release_cbfunc_t relcbfunc, void *relcbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;

    PRTE_ACQUIRE_OBJECT(cd);
    /* ack the call */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(status, cd->cbdata);
    }
    PRTE_RELEASE(cd);
}

pmix_status_t pmix_server_disconnect_fn(const pmix_proc_t procs[], size_t nprocs,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    pmix_status_t rc;

    prte_output_verbose(2, prte_pmix_server_globals.output, "%s disconnect called",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* at some point, we need to add bookeeping to track which
     * procs are "connected" so we know who to notify upon
     * termination or failure. For now, just execute a fence
     * Note that we do not need to thread-shift here as the
     * fence function will do it for us */
    cd = PRTE_NEW(prte_pmix_server_op_caddy_t);
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    if (PMIX_SUCCESS
        != (rc = pmix_server_fencenb_fn(procs, nprocs, info, ninfo, NULL, 0, mdxcbfunc, cd))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(cd);
    }

    return rc;
}

pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client, pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    /* PRTE currently has no way of supporting allocation requests */
    return PRTE_ERR_NOT_SUPPORTED;
}
