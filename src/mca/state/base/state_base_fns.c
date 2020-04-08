/*
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prrte_config.h"
#include "constants.h"

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <pmix.h>
#include <pmix_server.h>

#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prrte_data_server.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rml/rml.h"
#include "src/mca/routed/routed.h"
#include "src/util/session_dir.h"
#include "src/threads/threads.h"
#include "src/util/show_help.h"

#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"

void prrte_state_base_activate_job_state(prrte_job_t *jdata,
                                        prrte_job_state_t state)
{
    prrte_list_item_t *itm, *any=NULL, *error=NULL;
    prrte_state_t *s;
    prrte_state_caddy_t *caddy;

    for (itm = prrte_list_get_first(&prrte_job_states);
         itm != prrte_list_get_end(&prrte_job_states);
         itm = prrte_list_get_next(itm)) {
        s = (prrte_state_t*)itm;
        if (s->job_state == PRRTE_JOB_STATE_ANY) {
            /* save this place */
            any = itm;
        }
        if (s->job_state == PRRTE_JOB_STATE_ERROR) {
            error = itm;
        }
        if (s->job_state == state) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                                 "%s ACTIVATING JOB %s STATE %s PRI %d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == jdata) ? "NULL" : PRRTE_JOBID_PRINT(jdata->jobid),
                                 prrte_job_state_to_str(state), s->priority));
            if (NULL == s->cbfunc) {
                PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                                     "%s NULL CBFUNC FOR JOB %s STATE %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     (NULL == jdata) ? "ALL" : PRRTE_JOBID_PRINT(jdata->jobid),
                                     prrte_job_state_to_str(state)));
                return;
            }
            caddy = PRRTE_NEW(prrte_state_caddy_t);
            if (NULL != jdata) {
                caddy->jdata = jdata;
                caddy->job_state = state;
                PRRTE_RETAIN(jdata);
            }
            PRRTE_THREADSHIFT(caddy, prrte_event_base, s->cbfunc, s->priority);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PRRTE_JOB_STATE_ERROR < state && NULL != error) {
        s = (prrte_state_t*)error;
    } else if (NULL != any) {
        s = (prrte_state_t*)any;
    } else {
        PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                             "ACTIVATE: JOB STATE %s NOT REGISTERED", prrte_job_state_to_str(state)));
        return;
    }
    if (NULL == s->cbfunc) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                             "ACTIVATE: ANY STATE HANDLER NOT DEFINED"));
        return;
    }
    caddy = PRRTE_NEW(prrte_state_caddy_t);
    if (NULL != jdata) {
        caddy->jdata = jdata;
        caddy->job_state = state;
        PRRTE_RETAIN(jdata);
    }
    PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                         "%s ACTIVATING JOB %s STATE %s PRI %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (NULL == jdata) ? "NULL" : PRRTE_JOBID_PRINT(jdata->jobid),
                         prrte_job_state_to_str(state), s->priority));
    PRRTE_THREADSHIFT(caddy, prrte_event_base, s->cbfunc, s->priority);
}


int prrte_state_base_add_job_state(prrte_job_state_t state,
                                  prrte_state_cbfunc_t cbfunc,
                                  int priority)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    /* check for uniqueness */
    for (item = prrte_list_get_first(&prrte_job_states);
         item != prrte_list_get_end(&prrte_job_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->job_state == state) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                                 "DUPLICATE STATE DEFINED: %s",
                                 prrte_job_state_to_str(state)));
            return PRRTE_ERR_BAD_PARAM;
        }
    }

    st = PRRTE_NEW(prrte_state_t);
    st->job_state = state;
    st->cbfunc = cbfunc;
    st->priority = priority;
    prrte_list_append(&prrte_job_states, &(st->super));

    return PRRTE_SUCCESS;
}

int prrte_state_base_set_job_state_callback(prrte_job_state_t state,
                                           prrte_state_cbfunc_t cbfunc)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    for (item = prrte_list_get_first(&prrte_job_states);
         item != prrte_list_get_end(&prrte_job_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->job_state == state) {
            st->cbfunc = cbfunc;
            return PRRTE_SUCCESS;
        }
    }

    /* if not found, assume SYS priority and install it */
    st = PRRTE_NEW(prrte_state_t);
    st->job_state = state;
    st->cbfunc = cbfunc;
    st->priority = PRRTE_SYS_PRI;
    prrte_list_append(&prrte_job_states, &(st->super));

    return PRRTE_SUCCESS;
}

int prrte_state_base_set_job_state_priority(prrte_job_state_t state,
                                           int priority)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    for (item = prrte_list_get_first(&prrte_job_states);
         item != prrte_list_get_end(&prrte_job_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->job_state == state) {
            st->priority = priority;
            return PRRTE_SUCCESS;
        }
    }
    return PRRTE_ERR_NOT_FOUND;
}

int prrte_state_base_remove_job_state(prrte_job_state_t state)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    for (item = prrte_list_get_first(&prrte_job_states);
         item != prrte_list_get_end(&prrte_job_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->job_state == state) {
            prrte_list_remove_item(&prrte_job_states, item);
            PRRTE_RELEASE(item);
            return PRRTE_SUCCESS;
        }
    }
    return PRRTE_ERR_NOT_FOUND;
}

void prrte_state_base_print_job_state_machine(void)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    prrte_output(0, "PRRTE_JOB_STATE_MACHINE:");
    for (item = prrte_list_get_first(&prrte_job_states);
         item != prrte_list_get_end(&prrte_job_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        prrte_output(0, "\tState: %s cbfunc: %s",
                    prrte_job_state_to_str(st->job_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}


/****    PROC STATE MACHINE    ****/
void prrte_state_base_activate_proc_state(prrte_process_name_t *proc,
                                         prrte_proc_state_t state)
{
    prrte_list_item_t *itm, *any=NULL, *error=NULL;
    prrte_state_t *s;
    prrte_state_caddy_t *caddy;

    for (itm = prrte_list_get_first(&prrte_proc_states);
         itm != prrte_list_get_end(&prrte_proc_states);
         itm = prrte_list_get_next(itm)) {
        s = (prrte_state_t*)itm;
        if (s->proc_state == PRRTE_PROC_STATE_ANY) {
            /* save this place */
            any = itm;
        }
        if (s->proc_state == PRRTE_PROC_STATE_ERROR) {
            error = itm;
        }
        if (s->proc_state == state) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                                 "%s ACTIVATING PROC %s STATE %s PRI %d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(proc),
                                 prrte_proc_state_to_str(state), s->priority));
            if (NULL == s->cbfunc) {
                PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                                     "%s NULL CBFUNC FOR PROC %s STATE %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(proc),
                                     prrte_proc_state_to_str(state)));
                return;
            }
            caddy = PRRTE_NEW(prrte_state_caddy_t);
            caddy->name = *proc;
            caddy->proc_state = state;
            PRRTE_THREADSHIFT(caddy, prrte_event_base, s->cbfunc, s->priority);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PRRTE_PROC_STATE_ERROR < state && NULL != error) {
        s = (prrte_state_t*)error;
    } else if (NULL != any) {
        s = (prrte_state_t*)any;
    } else {
        PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                             "INCREMENT: ANY STATE NOT FOUND"));
        return;
    }
    if (NULL == s->cbfunc) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                             "ACTIVATE: ANY STATE HANDLER NOT DEFINED"));
        return;
    }
    caddy = PRRTE_NEW(prrte_state_caddy_t);
    caddy->name = *proc;
    caddy->proc_state = state;
    PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                         "%s ACTIVATING PROC %s STATE %s PRI %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc),
                         prrte_proc_state_to_str(state), s->priority));
     PRRTE_THREADSHIFT(caddy, prrte_event_base, s->cbfunc, s->priority);
}

int prrte_state_base_add_proc_state(prrte_proc_state_t state,
                                   prrte_state_cbfunc_t cbfunc,
                                   int priority)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    /* check for uniqueness */
    for (item = prrte_list_get_first(&prrte_proc_states);
         item != prrte_list_get_end(&prrte_proc_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->proc_state == state) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_state_base_framework.framework_output,
                                 "DUPLICATE STATE DEFINED: %s",
                                 prrte_proc_state_to_str(state)));
            return PRRTE_ERR_BAD_PARAM;
        }
    }

    st = PRRTE_NEW(prrte_state_t);
    st->proc_state = state;
    st->cbfunc = cbfunc;
    st->priority = priority;
    prrte_list_append(&prrte_proc_states, &(st->super));

    return PRRTE_SUCCESS;
}

int prrte_state_base_set_proc_state_callback(prrte_proc_state_t state,
                                            prrte_state_cbfunc_t cbfunc)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    for (item = prrte_list_get_first(&prrte_proc_states);
         item != prrte_list_get_end(&prrte_proc_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->proc_state == state) {
            st->cbfunc = cbfunc;
            return PRRTE_SUCCESS;
        }
    }
    return PRRTE_ERR_NOT_FOUND;
}

int prrte_state_base_set_proc_state_priority(prrte_proc_state_t state,
                                            int priority)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    for (item = prrte_list_get_first(&prrte_proc_states);
         item != prrte_list_get_end(&prrte_proc_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->proc_state == state) {
            st->priority = priority;
            return PRRTE_SUCCESS;
        }
    }
    return PRRTE_ERR_NOT_FOUND;
}

int prrte_state_base_remove_proc_state(prrte_proc_state_t state)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    for (item = prrte_list_get_first(&prrte_proc_states);
         item != prrte_list_get_end(&prrte_proc_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        if (st->proc_state == state) {
            prrte_list_remove_item(&prrte_proc_states, item);
            PRRTE_RELEASE(item);
            return PRRTE_SUCCESS;
        }
    }
    return PRRTE_ERR_NOT_FOUND;
}

void prrte_state_base_print_proc_state_machine(void)
{
    prrte_list_item_t *item;
    prrte_state_t *st;

    prrte_output(0, "PRRTE_PROC_STATE_MACHINE:");
    for (item = prrte_list_get_first(&prrte_proc_states);
         item != prrte_list_get_end(&prrte_proc_states);
         item = prrte_list_get_next(item)) {
        st = (prrte_state_t*)item;
        prrte_output(0, "\tState: %s cbfunc: %s",
                    prrte_proc_state_to_str(st->proc_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}

void prrte_state_base_local_launch_complete(int fd, short argc, void *cbdata)
{
    prrte_state_caddy_t *state = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata = state->jdata;

    if (prrte_report_launch_progress) {
        if (0 == jdata->num_daemons_reported % 100 ||
            jdata->num_daemons_reported == prrte_process_info.num_daemons) {
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_REPORT_PROGRESS);
        }
    }
    PRRTE_RELEASE(state);
}

void prrte_state_base_cleanup_job(int fd, short argc, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;

    PRRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                         "%s state:base:cleanup on job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (NULL == jdata) ? "NULL" : PRRTE_JOBID_PRINT(jdata->jobid)));

    /* flag that we were notified */
    jdata->state = PRRTE_JOB_STATE_NOTIFIED;
    /* send us back thru job complete */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
    PRRTE_RELEASE(caddy);
}

void prrte_state_base_report_progress(int fd, short argc, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;

     PRRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

   prrte_output(prrte_clean_output, "App launch reported: %d (out of %d) daemons - %d (out of %d) procs",
                (int)jdata->num_daemons_reported, (int)prrte_process_info.num_daemons,
                (int)jdata->num_launched, (int)jdata->num_procs);
    PRRTE_RELEASE(caddy);
}

void prrte_state_base_notify_data_server(prrte_process_name_t *target)
{
    prrte_buffer_t *buf;
    int rc, room = -1;
    uint8_t cmd = PRRTE_PMIX_PURGE_PROC_CMD;

    /* if nobody local to us published anything, then we can ignore this */
    if (PRRTE_JOBID_INVALID == prrte_pmix_server_globals.server.jobid) {
        return;
    }

    buf = PRRTE_NEW(prrte_buffer_t);

    /* pack the room number */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &room, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return;
    }

    /* load the command */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &cmd, 1, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return;
    }

    /* provide the target */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, target, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return;
    }

    /* send the request to the server */
    rc = prrte_rml.send_buffer_nb(&prrte_pmix_server_globals.server, buf,
                                 PRRTE_RML_TAG_DATA_SERVER,
                                 prrte_rml_send_callback, NULL);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_RELEASE(buf);
    }
}

static void _send_notification(int status,
                               prrte_proc_state_t state,
                               prrte_process_name_t *proc,
                               prrte_process_name_t *target)
{
    prrte_buffer_t *buf;
    prrte_grpcomm_signature_t sig;
    int rc;
    prrte_process_name_t daemon;
    prrte_byte_object_t bo, *boptr;
    pmix_byte_object_t pbo;
    pmix_info_t *info;
    size_t ninfo;
    pmix_proc_t psource;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_CUSTOM;
    pmix_status_t code, ret;

    prrte_output_verbose(5, prrte_state_base_framework.framework_output,
                        "%s state:base:sending notification %s proc %s target %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_ERROR_NAME(status),
                        PRRTE_NAME_PRINT(proc),
                        PRRTE_NAME_PRINT(target));

    /* pack the info for sending */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* pack the status code */
    code = prrte_pmix_convert_rc(status);
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prrte_process_info.myproc, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* pack the source - it cannot be me as that will cause
     * the pmix server to upcall the event back to me */
    PRRTE_PMIX_CONVERT_NAME(rc, &psource, proc);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        return;
    }
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prrte_process_info.myproc, &pbkt, &psource, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prrte_process_info.myproc, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* setup the info */
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &psource, PMIX_PROC);
    PRRTE_PMIX_CONVERT_NAME(rc, &psource, target);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &psource, PMIX_PROC);

    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prrte_process_info.myproc, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    /* pack the infos themselves */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&prrte_process_info.myproc, &pbkt, info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    PMIX_INFO_FREE(info, ninfo);

    /* unload the data buffer */
    PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
    bo.bytes = (uint8_t*)pbo.bytes;
    bo.size = pbo.size;

    /* insert into prrte_buffer_t */
    buf = PRRTE_NEW(prrte_buffer_t);
    /* we need to add a flag indicating this came from an invalid proc so that we will
     * inject it into our own PMIx server library */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &PRRTE_NAME_INVALID->vpid, 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        free(bo.bytes);
        PRRTE_RELEASE(buf);
        return;
    }
    boptr = &bo;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &boptr, 1, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        free(bo.bytes);
        return;
    }
    free(bo.bytes);

    if (PRRTE_VPID_WILDCARD == target->vpid) {
        /* xcast it to everyone */
        PRRTE_CONSTRUCT(&sig, prrte_grpcomm_signature_t);
        sig.signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
        sig.signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
        sig.signature[0].vpid = PRRTE_VPID_WILDCARD;
        sig.sz = 1;

        if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(&sig, PRRTE_RML_TAG_NOTIFICATION, buf))) {
            PRRTE_ERROR_LOG(rc);
        }
        PRRTE_DESTRUCT(&sig);
        PRRTE_RELEASE(buf);
    } else {
        /* get the daemon hosting the proc to be notified */
        daemon.jobid = PRRTE_PROC_MY_NAME->jobid;
        daemon.vpid = prrte_get_proc_daemon_vpid(target);
        /* send the notification to that daemon */
        prrte_output_verbose(5, prrte_state_base_framework.framework_output,
                            "%s state:base:sending notification %s to proc %s at daemon %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_ERROR_NAME(status),
                            PRRTE_NAME_PRINT(target),
                            PRRTE_NAME_PRINT(&daemon));
        if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(&daemon, buf,
                                                          PRRTE_RML_TAG_NOTIFICATION,
                                                          prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(buf);
        }
    }
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

void prrte_state_base_track_procs(int fd, short argc, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_process_name_t *proc;
    prrte_proc_state_t state;
    prrte_job_t *jdata;
    prrte_proc_t *pdata;
    int i, rc;
    prrte_process_name_t parent, target;
    prrte_pmix_lock_t lock;

    PRRTE_ACQUIRE_OBJECT(caddy);
    proc = &caddy->name;
    state = caddy->proc_state;

    prrte_output_verbose(5, prrte_state_base_framework.framework_output,
                        "%s state:base:track_procs called for proc %s state %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(proc),
                        prrte_proc_state_to_str(state));

    /* get the job object for this proc */
    if (NULL == (jdata = prrte_get_job_data_object(proc->jobid))) {
        goto cleanup;
    }
    pdata = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid);
    if (NULL == pdata) {
        goto cleanup;
    }

    if (PRRTE_PROC_STATE_RUNNING == state) {
        /* update the proc state */
        if (pdata->state < PRRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        jdata->num_launched++;
        if (jdata->num_launched == jdata->num_procs) {
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_RUNNING);
        }
    } else if (PRRTE_PROC_STATE_REGISTERED == state) {
        /* update the proc state */
        if (pdata->state < PRRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        jdata->num_reported++;
        if (jdata->num_reported == jdata->num_procs) {
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_REGISTERED);
        }
    } else if (PRRTE_PROC_STATE_IOF_COMPLETE == state) {
        /* update the proc state */
        if (pdata->state < PRRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        /* Release the IOF file descriptors */
        if (NULL != prrte_iof.close) {
            prrte_iof.close(proc, PRRTE_IOF_STDALL);
        }
        PRRTE_FLAG_SET(pdata, PRRTE_PROC_FLAG_IOF_COMPLETE);
        if (PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_WAITPID)) {
            PRRTE_ACTIVATE_PROC_STATE(proc, PRRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRRTE_PROC_STATE_WAITPID_FIRED == state) {
        /* update the proc state */
        if (pdata->state < PRRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        PRRTE_FLAG_SET(pdata, PRRTE_PROC_FLAG_WAITPID);
        if (PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_IOF_COMPLETE)) {
            PRRTE_ACTIVATE_PROC_STATE(proc, PRRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRRTE_PROC_STATE_TERMINATED == state) {
        if (pdata->state == state) {
            prrte_output_verbose(5, prrte_state_base_framework.framework_output,
                                 "%s state:base:track_procs proc %s already in state %s. Skip transition.",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(proc),
                                 prrte_proc_state_to_str(state));
            goto cleanup;
        }

        /* update the proc state */
        PRRTE_FLAG_UNSET(pdata, PRRTE_PROC_FLAG_ALIVE);
        if (pdata->state < PRRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        if (PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_LOCAL)) {
            pmix_proc_t pproc;
            /* tell the PMIx subsystem to cleanup this client */
            PRRTE_PMIX_CONVERT_NAME(rc, &pproc, proc);
            if (PRRTE_SUCCESS != rc) {
                PRRTE_ERROR_LOG(rc);
                return;
            }
            PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
            PMIx_server_deregister_client(&pproc, opcbfunc, &lock);
            PRRTE_PMIX_WAIT_THREAD(&lock);
            PRRTE_PMIX_DESTRUCT_LOCK(&lock);

            /* Clean up the session directory as if we were the process
             * itself.  This covers the case where the process died abnormally
             * and didn't cleanup its own session directory.
             */
            if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
                prrte_session_dir_finalize(proc);
            }
        }
        /* if we are trying to terminate and our routes are
         * gone, then terminate ourselves IF no local procs
         * remain (might be some from another job)
         */
        if (prrte_prteds_term_ordered &&
            0 == prrte_routed.num_routes()) {
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL != (pdata = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i)) &&
                    PRRTE_FLAG_TEST(pdata, PRRTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    goto cleanup;
                }
            }
            /* call our appropriate exit procedure */
            PRRTE_OUTPUT_VERBOSE((5, prrte_state_base_framework.framework_output,
                                 "%s state:base all routes and children gone - exiting",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            goto cleanup;
        }
        /* track job status */
        jdata->num_terminated++;
        if (jdata->num_terminated == jdata->num_procs) {
            /* if requested, check fd status for leaks */
            if (prrte_state_base_run_fdcheck) {
                prrte_state_base_check_fds(jdata);
            }
            /* if ompi-server is around, then notify it to purge
             * any session-related info */
            if (NULL != prrte_data_server_uri) {
                target.jobid = jdata->jobid;
                target.vpid = PRRTE_VPID_WILDCARD;
                prrte_state_base_notify_data_server(&target);
            }
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_TERMINATED);
        } else if (PRRTE_PROC_STATE_TERMINATED < pdata->state &&
                   !prrte_job_term_ordered) {
            /* if this was an abnormal term, notify the other procs of the termination */
            parent.jobid = jdata->jobid;
            parent.vpid = PRRTE_VPID_WILDCARD;
            _send_notification(PRRTE_ERR_PROC_ABORTED, pdata->state, &pdata->name, &parent);
        }
    }

 cleanup:
    PRRTE_RELEASE(caddy);
}

void prrte_state_base_check_all_complete(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    prrte_job_t *jdata;
    prrte_proc_t *proc;
    int i;
    prrte_std_cntr_t j;
    prrte_job_t *job;
    prrte_node_t *node;
    prrte_job_map_t *map;
    prrte_std_cntr_t index;
    bool one_still_alive;
    prrte_vpid_t lowest=0;
    int32_t i32, *i32ptr;
    uint32_t u32;
    void *nptr;
    prrte_pmix_lock_t lock;

    PRRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    prrte_output_verbose(2, prrte_state_base_framework.framework_output,
                        "%s state:base:check_job_complete on job %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == jdata) ? "NULL" : PRRTE_JOBID_PRINT(jdata->jobid));

    if (NULL == jdata || jdata->jobid == PRRTE_PROC_MY_NAME->jobid) {
        /* just check to see if the daemons are complete */
        PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                             "%s state:base:check_job_complete - received NULL job, checking daemons",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        goto CHECK_DAEMONS;
    } else {
        /* mark the job as terminated, but don't override any
         * abnormal termination flags
         */
        if (jdata->state < PRRTE_JOB_STATE_UNTERMINATED) {
            jdata->state = PRRTE_JOB_STATE_TERMINATED;
        }
    }

    /* tell the IOF that the job is complete */
    if (NULL != prrte_iof.complete) {
        prrte_iof.complete(jdata);
    }

    /* tell the PMIx server to release its data */
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_server_deregister_nspace(jdata->nspace, opcbfunc, &lock);
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);

    i32ptr = &i32;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_NUM_NONZERO_EXIT, (void**)&i32ptr, PRRTE_INT32) && !prrte_abort_non_zero_exit) {
        if (!prrte_report_child_jobs_separately || 1 == PRRTE_LOCAL_JOBID(jdata->jobid)) {
            /* update the exit code */
            PRRTE_UPDATE_EXIT_STATUS(lowest);
        }

        /* warn user */
        prrte_show_help("help-state-base.txt", "normal-termination-but", true,
                    (1 == PRRTE_LOCAL_JOBID(jdata->jobid)) ? "the primary" : "child",
                    (1 == PRRTE_LOCAL_JOBID(jdata->jobid)) ? "" : PRRTE_LOCAL_JOBID_PRINT(jdata->jobid),
                    i32, (1 == i32) ? "process returned\na non-zero exit code." :
                    "processes returned\nnon-zero exit codes.");
    }

    PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                         "%s state:base:check_job_completed declared job %s terminated with state %s - checking all jobs",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid),
                         prrte_job_state_to_str(jdata->state)));

    /* if this job is a continuously operating one, then don't do
     * anything further - just return here
     */
    if (NULL != jdata &&
        (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CONTINUOUS_OP, NULL, PRRTE_BOOL) ||
         PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RECOVERABLE))) {
        goto CHECK_ALIVE;
    }

    /* if the job that is being checked is the HNP, then we are
     * trying to terminate the orteds. In that situation, we
     * do -not- check all jobs - we simply notify the HNP
     * that the orteds are complete. Also check special case
     * if jdata is NULL - we want
     * to definitely declare the job done if the orteds
     * have completed, no matter what else may be happening.
     * This can happen if a ctrl-c hits in the "wrong" place
     * while launching
     */
 CHECK_DAEMONS:
    if (jdata == NULL || jdata->jobid == PRRTE_PROC_MY_NAME->jobid) {
        if (0 == prrte_routed.num_routes()) {
            /* orteds are done! */
            PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                 "%s orteds complete - exiting",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            if (NULL == jdata) {
                jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
            }
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
            PRRTE_RELEASE(caddy);
            return;
        }
        PRRTE_RELEASE(caddy);
        return;
    }

    /* Release the resources used by this job. Since some errmgrs may want
     * to continue using resources allocated to the job as part of their
     * fault recovery procedure, we only do this once the job is "complete".
     * Note that an aborted/killed job -is- flagged as complete and will
     * therefore have its resources released. We need to do this after
     * we call the errmgr so that any attempt to restart the job will
     * avoid doing so in the exact same place as the current job
     */
    if (NULL != jdata->map && jdata->state == PRRTE_JOB_STATE_TERMINATED) {
        map = jdata->map;
        for (index = 0; index < map->nodes->size; index++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, index))) {
                continue;
            }
            PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                 "%s releasing procs for job %s from node %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_JOBID_PRINT(jdata->jobid), node->name));
            for (i = 0; i < node->procs->size; i++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                    continue;
                }
                if (proc->name.jobid != jdata->jobid) {
                    /* skip procs from another job */
                    continue;
                }
                node->slots_inuse--;
                node->num_procs--;
                PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                     "%s releasing proc %s from node %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&proc->name), node->name));
                /* set the entry in the node array to NULL */
                prrte_pointer_array_set_item(node->procs, i, NULL);
                /* release the proc once for the map entry */
                PRRTE_RELEASE(proc);
            }
            /* set the node location to NULL */
            prrte_pointer_array_set_item(map->nodes, index, NULL);
            /* maintain accounting */
            PRRTE_RELEASE(node);
        }
        PRRTE_RELEASE(map);
        jdata->map = NULL;
    }

 CHECK_ALIVE:
    /* now check to see if all jobs are done - trigger notification of this jdata
     * object when we find it
     */
    one_still_alive = false;
    j = prrte_hash_table_get_first_key_uint32(prrte_job_data, &u32, (void **)&job, &nptr);
    while (PRRTE_SUCCESS == j) {
        /* skip the daemon job */
        if (job->jobid == PRRTE_PROC_MY_NAME->jobid) {
            goto next;
        }
        /* if this is the job we are checking AND it normally terminated,
         * then activate the "notify_completed" state - this will release
         * the job state, but is provided so that the HNP main code can
         * take alternative actions if desired. If the state is killed_by_cmd,
         * then go ahead and release it. We cannot release it if it
         * abnormally terminated as mpirun needs the info so it can
         * report appropriately to the user
         *
         * NOTE: do not release the primary job (j=1) so we
         * can pretty-print completion message
         */
        if (NULL != jdata && job->jobid == jdata->jobid) {
            if (jdata->state == PRRTE_JOB_STATE_TERMINATED) {
                PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                     "%s state:base:check_job_completed state is terminated - activating notify",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_NOTIFY_COMPLETED);
                one_still_alive = true;
            } else if (jdata->state == PRRTE_JOB_STATE_KILLED_BY_CMD ||
                       jdata->state == PRRTE_JOB_STATE_NOTIFIED) {
                PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                     "%s state:base:check_job_completed state is killed or notified - cleaning up",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                /* release this object, ensuring that the
                 * pointer array internal accounting
                 * is maintained!
                 */
                if (1 < j) {
                    prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, NULL);
                    PRRTE_RELEASE(jdata);
                }
            }
            goto next;
        }
        /* if the job is flagged to not be monitored, skip it */
        if (PRRTE_FLAG_TEST(job, PRRTE_JOB_FLAG_DO_NOT_MONITOR)) {
            goto next;
        }
        /* when checking for job termination, we must be sure to NOT check
         * our own job as it - rather obviously - has NOT terminated!
         */
        if (PRRTE_JOB_STATE_NOTIFIED != job->state) {
            /* we have at least one job that is not done yet - we cannot
             * just return, though, as we need to ensure we cleanout the
             * job data for the job that just completed
             */
            PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                 "%s state:base:check_job_completed job %s is not terminated (%d:%d)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_JOBID_PRINT(job->jobid),
                                 job->num_terminated, job->num_procs));
            one_still_alive = true;
        }
        else {
            PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                                 "%s state:base:check_job_completed job %s is terminated (%d vs %d [%s])",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_JOBID_PRINT(job->jobid),
                                 job->num_terminated, job->num_procs,
                                 (NULL == jdata) ? "UNKNOWN" : prrte_job_state_to_str(jdata->state) ));
        }
      next:
        j = prrte_hash_table_get_next_key_uint32(prrte_job_data, &u32, (void **)&job, nptr, &nptr);
    }

    /* if a job is still alive, we just return */
    if (one_still_alive) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                             "%s state:base:check_job_completed at least one job is not terminated",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_RELEASE(caddy);
        return;
    }
    /* if we get here, then all jobs are done, so terminate */
    PRRTE_OUTPUT_VERBOSE((2, prrte_state_base_framework.framework_output,
                         "%s state:base:check_job_completed all jobs terminated",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* stop the job timeout event, if set */
    if (NULL != prrte_mpiexec_timeout) {
        PRRTE_RELEASE(prrte_mpiexec_timeout);
        prrte_mpiexec_timeout = NULL;
    }

    /* set the exit status to 0 - this will only happen if it
     * wasn't already set by an error condition
     */
    PRRTE_UPDATE_EXIT_STATUS(0);

    /* order daemon termination - this tells us to cleanup
     * our local procs as well as telling remote daemons
     * to die
     */
    prrte_plm.terminate_orteds();

    PRRTE_RELEASE(caddy);
}


void prrte_state_base_check_fds(prrte_job_t *jdata)
{
    int nfds, i, fdflags, flflags;
    char path[1024], info[256], **list=NULL, *status, *result, *r2;
    ssize_t rc;
    struct flock fl;
    bool flk;
    int cnt = 0;

    /* get the number of available file descriptors
     * for this daemon */
    nfds = getdtablesize();
    result = NULL;
    /* loop over them and get their info */
    for (i=0; i < nfds; i++) {
        fdflags = fcntl(i, F_GETFD);
        if (-1 == fdflags) {
            /* no open fd in that slot */
            continue;
        }
        flflags = fcntl(i, F_GETFL);
        if (-1 == flflags) {
            /* no open fd in that slot */
            continue;
        }
        snprintf(path, 1024, "/proc/self/fd/%d", i);
        memset(info, 0, 256);
        /* read the info about this fd */
        rc = readlink(path, info, 256);
        if (-1 == rc) {
            /* this fd is unavailable */
            continue;
        }
        /* get any file locking status */
        fl.l_type = F_WRLCK;
        fl.l_whence = 0;
        fl.l_start = 0;
        fl.l_len = 0;
        if (-1 == fcntl(i, F_GETLK, &fl)) {
            flk = false;
        } else {
            flk = true;
        }
        /* construct the list of capabilities */
        if (fdflags & FD_CLOEXEC) {
            prrte_argv_append_nosize(&list, "cloexec");
        }
        if (flflags & O_APPEND) {
            prrte_argv_append_nosize(&list, "append");
        }
        if (flflags & O_NONBLOCK) {
            prrte_argv_append_nosize(&list, "nonblock");
        }
        /* from the man page:
         *  Unlike the other values that can be specified in flags,
         * the access mode values O_RDONLY, O_WRONLY, and O_RDWR,
         * do not specify individual bits.  Rather, they define
         * the low order two bits of flags, and defined respectively
         * as 0, 1, and 2. */
        if (O_RDONLY == (flflags & 3)) {
            prrte_argv_append_nosize(&list, "rdonly");
        } else if (O_WRONLY == (flflags & 3)) {
            prrte_argv_append_nosize(&list, "wronly");
        } else {
            prrte_argv_append_nosize(&list, "rdwr");
        }
        if (flk && F_UNLCK != fl.l_type) {
            if (F_WRLCK == fl.l_type) {
                prrte_argv_append_nosize(&list, "wrlock");
            } else {
                prrte_argv_append_nosize(&list, "rdlock");
            }
        }
        if (NULL != list) {
            status = prrte_argv_join(list, ' ');
            prrte_argv_free(list);
            list = NULL;
            if (NULL == result) {
                prrte_asprintf(&result, "    %d\t(%s)\t%s\n", i, info, status);
            } else {
                prrte_asprintf(&r2, "%s    %d\t(%s)\t%s\n", result, i, info, status);
                free(result);
                result = r2;
            }
            free(status);
        }
        ++cnt;
    }
    prrte_asprintf(&r2, "%s: %d open file descriptors after job %d completed\n%s",
             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), cnt, PRRTE_LOCAL_JOBID(jdata->jobid), result);
    prrte_output(0, "%s", r2);
    free(result);
    free(r2);
}
