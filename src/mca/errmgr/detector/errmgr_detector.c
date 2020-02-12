/*
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <pmix.h>
#include <pmix_server.h>

#include "src/util/output.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/base/prrte_mca_base_var.h"
#include "src/threads/threads.h"
#include "src/mca/rml/rml.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/bmg/grpcomm_bmg.h"
#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

#include "src/mca/propagate/propagate.h"
#include "errmgr_detector.h"
#include <math.h>

static int init(void);
static int finalize(void);

int prrte_errmgr_enable_detector(bool flag);
/******************
 * detector module
 ******************/
prrte_errmgr_base_module_t prrte_errmgr_detector_module = {
    init,
    finalize,
    prrte_errmgr_base_log,
    prrte_errmgr_base_abort,
    prrte_errmgr_base_abort_peers,
    prrte_errmgr_enable_detector
};

prrte_errmgr_base_module_t prrte_errmgr = {
    init,
    finalize,
    prrte_errmgr_base_log,
    NULL,
    NULL,
    prrte_errmgr_enable_detector
};

/*
 * Local functions
 */
static int fd_heartbeat_request(prrte_errmgr_detector_t* detector);
static int fd_heartbeat_send(prrte_errmgr_detector_t* detector);

static int fd_heartbeat_request_cb(int status,
        prrte_process_name_t* sender,
        prrte_buffer_t *buffer,
        prrte_rml_tag_t tg,
        void *cbdata);

static int fd_heartbeat_recv_cb(int status,
        prrte_process_name_t* sender,
        prrte_buffer_t *buffer,
        prrte_rml_tag_t tg,
        void *cbdata);

static double Wtime();
//static double prrte_errmgr_heartbeat_period = 5;
//static double prrte_errmgr_heartbeat_timeout = 10;
static prrte_event_base_t* fd_event_base = NULL;

static void fd_event_cb(int fd, short flags, void* pdetector);


static int pack_state_for_proc(prrte_buffer_t *alert, prrte_proc_t *child)
{
    int rc;

    /* pack the child's vpid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &(child->name.vpid), 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack the pid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->pid, 1, PRRTE_PID))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its state */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->state, 1, PRRTE_PROC_STATE))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its exit code */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &child->exit_code, 1, PRRTE_EXIT_CODE))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}

static void register_cbfunc(int status, size_t errhndler, void *cbdata)
{
    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                "errmgr:detector:event register cbfunc with status %d ", status));
}

static void error_notify_cbfunc(size_t evhdlr_registration_id,
        pmix_status_t status,
        const pmix_proc_t *psource,
        pmix_info_t info[], size_t ninfo,
        pmix_info_t *results, size_t nresults,
        pmix_event_notification_cbfunc_fn_t cbfunc,
        void *cbdata)
{
    prrte_process_name_t proc, source;
    proc.jobid = PRRTE_JOBID_INVALID;
    proc.vpid = PRRTE_VPID_INVALID;

    int rc;
    prrte_proc_t *temp_prrte_proc;
    prrte_buffer_t *alert;
    prrte_job_t *jdata;
    prrte_plm_cmd_flag_t cmd;
    size_t n;
    PRRTE_PMIX_CONVERT_PROCT(rc, &source, psource);
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PRRTE_PMIX_CONVERT_PROCT(rc, &proc, info[n].value.data.proc);

                if( prrte_get_proc_daemon_vpid(&proc) != PRRTE_PROC_MY_NAME->vpid){
                    return;
                }
                PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                            "%s errmgr: detector: error proc %s with key-value %s notified from %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_NAME_PRINT(&proc),
                            info[n].key, PRRTE_NAME_PRINT(&source)));

                if (NULL == (jdata = prrte_get_job_data_object(proc.jobid))) {
                    /* must already be complete */
                    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                "%s errmgr:detector:error_notify_callback NULL jdata - ignoring error",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                }
                temp_prrte_proc= (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc.vpid);

                alert = PRRTE_NEW(prrte_buffer_t);
                /* pack update state command */
                cmd = PRRTE_PLM_UPDATE_PROC_STATE;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
                    PRRTE_ERROR_LOG(rc);
                    return;
                }

                /* pack jobid */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &proc.jobid, 1, PRRTE_JOBID))) {
                    PRRTE_ERROR_LOG(rc);
                    return;
                }

                /* proc state now is PRRTE_PROC_STATE_ABORTED_BY_SIG, cause odls set state to this; code is 128+9 */
                temp_prrte_proc->state = PRRTE_PROC_STATE_ABORTED_BY_SIG;
                /* now pack the child's info */
                if (PRRTE_SUCCESS != (rc = pack_state_for_proc(alert, temp_prrte_proc))) {
                    PRRTE_ERROR_LOG(rc);
                    return;
                }

                /* send this process's info to hnp */
                if (0 > (rc = prrte_rml.send_buffer_nb(
                                PRRTE_PROC_MY_HNP, alert,
                                PRRTE_RML_TAG_PLM,
                                prrte_rml_send_callback, NULL))) {
                    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                                "%s errmgr:detector: send to hnp failed",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(alert);
                }
                if (PRRTE_FLAG_TEST(temp_prrte_proc, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
                        PRRTE_FLAG_TEST(temp_prrte_proc, PRRTE_PROC_FLAG_WAITPID) &&
                        !PRRTE_FLAG_TEST(temp_prrte_proc, PRRTE_PROC_FLAG_RECORDED)) {
                    PRRTE_ACTIVATE_PROC_STATE(&proc, PRRTE_PROC_STATE_TERMINATED);
                }

                prrte_propagate.prp(&source.jobid, &source, &proc, PRRTE_ERR_PROC_ABORTED);
                break;
            }
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PRRTE_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

static int init(void) {
    fd_event_base = prrte_sync_event_base;

    if ( PRRTE_PROC_IS_DAEMON )
    {
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_HEARTBEAT_REQUEST,
                PRRTE_RML_PERSISTENT,fd_heartbeat_request_cb,NULL);
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_HEARTBEAT,
                PRRTE_RML_PERSISTENT,fd_heartbeat_recv_cb,NULL);
    }
    return PRRTE_SUCCESS;
}

int finalize(void) {
    if ( PRRTE_PROC_IS_DAEMON )
    {
        prrte_errmgr_detector_t* detector = &prrte_errmgr_world_detector;

        if(detector->hb_observer != PRRTE_VPID_INVALID)
        {
            detector->hb_observer = prrte_process_info.my_name.vpid;
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,"errmgr:detector: send last heartbeat message"));
            fd_heartbeat_send(detector);
            detector->hb_period = INFINITY;
        }
        prrte_event_del(&prrte_errmgr_world_detector.fd_event);
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_HEARTBEAT_REQUEST);
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_HEARTBEAT);
        if( prrte_sync_event_base != fd_event_base ) prrte_event_base_free(fd_event_base);

        /* set heartbeat peroid to infinity and observer to invalid */
        prrte_errmgr_world_detector.hb_period = INFINITY;
        prrte_errmgr_world_detector.hb_observer = PRRTE_VPID_INVALID;
    }
    return PRRTE_SUCCESS;
}

bool errmgr_get_daemon_status(prrte_process_name_t daemon)
{
    prrte_errmgr_detector_t* detector = &prrte_errmgr_world_detector;
    int i;
    for ( i=0; i<detector->failed_node_count; i++)
    {
        if( *(detector->daemons_state +i) == daemon.vpid)
        {
            return false;
        }
    }
    return true;
}

void errmgr_set_daemon_status(prrte_process_name_t daemon)
{
    prrte_errmgr_detector_t* detector = &prrte_errmgr_world_detector;
    *(detector->daemons_state + detector->failed_node_count) = daemon.vpid;
}

static double Wtime(void)
{
    double wtime;

#if PRRTE_TIMER_CYCLE_NATIVE
    wtime = ((double) prrte_timer_base_get_cycles()) / prrte_timer_base_get_freq();
#elif PRRTE_TIMER_USEC_NATIVE
    wtime = ((double) prrte_timer_base_get_usec()) / 1000000.0;
#else
    /* Fall back to gettimeofday() if we have nothing else */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    wtime = tv.tv_sec;
    wtime += (double)tv.tv_usec / 1000000.0;
#endif
    return wtime;
}

int prrte_errmgr_enable_detector(bool enable_flag)
{
    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                "%s errmgr:detector report detector_enable_status %d",
                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), enable_flag));

    if ( PRRTE_PROC_IS_DAEMON && enable_flag )
    {
        prrte_errmgr_detector_t* detector = &prrte_errmgr_world_detector;
        int  ndmns, i;
        uint32_t vpid;

        pmix_status_t pcode = prrte_pmix_convert_rc(PRRTE_ERR_PROC_ABORTED);

        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "%s errmgr:detector: register evhandler in errmgr",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PMIx_Register_event_handler(&pcode, 1, NULL, 0, error_notify_cbfunc, register_cbfunc, NULL);
        prrte_propagate.register_cb();

        /* num of daemon in this jobid */
        ndmns = prrte_process_info.num_procs-1;
        vpid = prrte_process_info.my_name.vpid;
        /*  we observing somebody {n,1,2,...n-1}, the ring */
        if( 0 != (vpid - 1) )
            detector->hb_observing = vpid - 1;
        else detector->hb_observing = ndmns;
        /* someone is observing us: range [1~n], the observing ring */
        detector->hb_observer = (ndmns+vpid) % ndmns + 1 ;
        detector->hb_period = prrte_errmgr_heartbeat_period;
        detector->hb_timeout = prrte_errmgr_heartbeat_timeout;
        detector->hb_sstamp = 0.;
        /* give some slack for MPI_Init */
        detector->hb_rstamp = Wtime()+(double)ndmns;

        detector->daemons_state = malloc(8* sizeof(int));
        for(i=0; i<8; i++)
        {
            *(detector->daemons_state + i) = -1;
        }

        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector daemon %d observering %d observer %d",
                    vpid,
                    detector->hb_observing,
                    detector->hb_observer));

        prrte_event_set(fd_event_base, &detector->fd_event, -1, PRRTE_EV_TIMEOUT | PRRTE_EV_PERSIST, fd_event_cb, detector);
        struct timeval tv;
        tv.tv_sec = (int)(detector->hb_period / 10.);
        tv.tv_usec = (-tv.tv_sec + (detector->hb_period / 10.)) * 1e6;
        prrte_event_add(&prrte_errmgr_world_detector.fd_event, &tv);
    }
    return PRRTE_SUCCESS;
}

static int fd_heartbeat_request(prrte_errmgr_detector_t* detector) {

    int ret,  ndmns;
    uint32_t vpid;

    prrte_process_name_t temp_proc_name;
    temp_proc_name.jobid = prrte_process_info.my_name.jobid;
    temp_proc_name.vpid = detector->hb_observing;

    if( errmgr_get_daemon_status(temp_proc_name) )
    {
        /* already observing a live process, so nothing to do. */
        return PRRTE_SUCCESS;
    }

    ndmns = prrte_process_info.num_procs-1;

    prrte_buffer_t *buffer = NULL;
    prrte_process_name_t daemon;
    for( vpid = (ndmns+detector->hb_observing) % ndmns;
            vpid != prrte_process_info.my_name.vpid;
            vpid = (ndmns+vpid-1) % ndmns ) {
        daemon.jobid = prrte_process_info.my_name.jobid;
        if(0 != vpid){
            daemon.vpid = vpid;
        }
        else daemon.vpid = ndmns;

        // this daemon is not alive
        if(!errmgr_get_daemon_status(daemon)) continue;

        /* everyone is gone, i dont need to monitor myself */
        if(daemon.vpid == prrte_process_info.my_name.vpid)
        {
            detector->hb_observer = detector->hb_observing = PRRTE_VPID_INVALID;
            detector->hb_rstamp = INFINITY;
            detector->hb_period = INFINITY;
            return PRRTE_SUCCESS;
        }

        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector hb request updating ring"));
        detector->hb_observing = daemon.vpid;
        buffer = PRRTE_NEW(prrte_buffer_t);
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &prrte_process_info.my_name.jobid, 1,PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(ret);
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &prrte_process_info.my_name.vpid, 1,PRRTE_VPID))) {
            PRRTE_ERROR_LOG(ret);
        }
        if (0 > (ret = prrte_rml.send_buffer_nb(
                        &daemon, buffer,
                        PRRTE_RML_TAG_HEARTBEAT_REQUEST, prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
        }
        break;
    }
    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                "errmgr:detector updated ring daemon %d observering %d observer %d",
                PRRTE_PROC_MY_NAME->vpid,
                detector->hb_observing,
                detector->hb_observer));
    /* we add one timeout slack to account for the send time */
    detector->hb_rstamp = Wtime()+detector->hb_timeout;
    return PRRTE_SUCCESS;
}

static int fd_heartbeat_request_cb(int status, prrte_process_name_t* sender,
        prrte_buffer_t *buffer,
        prrte_rml_tag_t tg, void *cbdata) {
    prrte_errmgr_detector_t* detector = &prrte_errmgr_world_detector;
    int ndmns, rr, ro;
    prrte_jobid_t vpid, jobid;
    int temp;
    temp =1;
    int rc;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &jobid,&temp,PRRTE_JOBID)))
        PRRTE_ERROR_LOG(rc);
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &vpid, &temp,PRRTE_VPID)))
        PRRTE_ERROR_LOG(rc);
    PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                "errmgr:detector %d receive %d",
                prrte_process_info.my_name.vpid,
                detector->hb_observer));
    ndmns = prrte_process_info.num_nodes;
    rr = (ndmns-prrte_process_info.my_name.vpid+vpid) % ndmns; /* translate msg->from in circular space so that myrank==0 */
    ro = (ndmns-prrte_process_info.my_name.vpid+detector->hb_observer) % ndmns; /* same for the observer rank */
    if( rr < ro ) {
        return false; /* never forward on the rbcast */
    }

    detector->hb_observer = vpid;
    detector->hb_sstamp = 0.;

    fd_heartbeat_send(detector);
    return false;
}

/*
 * event loop and thread
 */

static void fd_event_cb(int fd, short flags, void* pdetector) {
    // need to find a new time func
    double stamp = Wtime();
    prrte_errmgr_detector_t* detector = pdetector;

    // temp proc name for get the prrte object
    prrte_process_name_t temp_proc_name;

    if( (stamp - detector->hb_sstamp) >= detector->hb_period ) {
        fd_heartbeat_send(detector);
    }
    if( INFINITY == detector->hb_rstamp ) return;

    if( (stamp - detector->hb_rstamp) > detector->hb_timeout ) {
        /* this process is now suspected dead. */
        temp_proc_name.jobid = prrte_process_info.my_name.jobid;
        temp_proc_name.vpid = detector->hb_observing;
        /* if first time detected */
        if (errmgr_get_daemon_status(temp_proc_name)){
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                        "errmgr:detector %d observing %d",
                        prrte_process_info.my_name.vpid, detector->hb_observing));
            prrte_propagate.prp(&temp_proc_name.jobid, NULL, &temp_proc_name,PRRTE_ERR_PROC_ABORTED );

            /* with every 8 failed nodes realloc 8 more slots to store the vpid of failed nodes */
            if( (detector->failed_node_count / 8) > 0 && (detector->failed_node_count % 8) == 0 )
                detector->daemons_state = realloc( detector->daemons_state, detector->failed_node_count+8);

            errmgr_set_daemon_status(temp_proc_name);
            /* increase the number of failed nodes */
            detector->failed_node_count++;
            fd_heartbeat_request(detector);
        }
    }
}

/*
 * send eager based heartbeats
 */
static int fd_heartbeat_send(prrte_errmgr_detector_t* detector) {

    double now = Wtime();
    if( 0. != detector->hb_sstamp
            && (now - detector->hb_sstamp) >= 2.*detector->hb_period ) {
        /* missed my send deadline find a verbose to use */
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector: daemon %s MISSED my deadline by %.1e, this could trigger a false suspicion for me",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    now-detector->hb_sstamp));
    }
    detector->hb_sstamp = now;

    prrte_buffer_t *buffer = NULL;
    int ret;

    buffer = PRRTE_NEW(prrte_buffer_t);
    prrte_process_name_t daemon;
    daemon.jobid = prrte_process_info.my_name.jobid;
    daemon.vpid = detector->hb_observer;
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &prrte_process_info.my_name.jobid, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(ret);
    }
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &prrte_process_info.my_name.vpid, 1, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(ret);
    }
    /* send the heartbeat with eager send */
    if (0 > (ret  = prrte_rml.send_buffer_nb(
                    &daemon,
                    buffer,PRRTE_RML_TAG_HEARTBEAT,
                    prrte_rml_send_callback, NULL))) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector:failed to send heartbeat to %d:%d",
                    daemon.jobid, daemon.vpid));
        PRRTE_ERROR_LOG(ret);
    }
    return PRRTE_SUCCESS;
}

static int fd_heartbeat_recv_cb(int status, prrte_process_name_t* sender,
        prrte_buffer_t *buffer,
        prrte_rml_tag_t tg, void *cbdata) {
    prrte_errmgr_detector_t* detector = &prrte_errmgr_world_detector;
    int rc;
    int32_t cnt;
    uint32_t vpid, jobid;

    if ( sender->vpid == prrte_process_info.my_name.vpid)
    {
        /* this is a quit msg from observed process, stop detector */
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector:%s %s Received heartbeat from %d, which is myself, quit msg to close detector",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),__func__, sender->vpid));
        detector->hb_observing = detector->hb_observer = PRRTE_VPID_INVALID;
        detector->hb_rstamp = INFINITY;
        detector->hb_period = INFINITY;
        return false;
    }

    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &jobid, &cnt, PRRTE_JOBID))){
        PRRTE_ERROR_LOG(rc);
    }
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &vpid, &cnt, PRRTE_VPID))){
        PRRTE_ERROR_LOG(rc);
    }

    if(vpid != detector->hb_observing ) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector: daemon %s receive heartbeat from vpid %d, but I am monitoring vpid %d ",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    vpid,
                    detector->hb_observing ));
    }
    else {
        double stamp = Wtime();
        double grace = detector->hb_timeout - (stamp - detector->hb_rstamp);
        PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                    "errmgr:detector: daemon %s receive heartbeat from vpid %d tag %d at timestamp %g (remained %.1e of %.1e before suspecting)",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    vpid,
                    tg,
                    stamp,
                    grace,
                    detector->hb_timeout));
        detector->hb_rstamp = stamp;
        if( grace < 0.0 ) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_errmgr_base_framework.framework_output,
                        "errmgr:detector: daemon %s  MISSED (%.1e)",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        grace));
        }
    }
    return false;
}
