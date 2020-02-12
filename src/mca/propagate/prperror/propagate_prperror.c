/*
 * Copyright (c) 2017-2018 The University of Tennessee and The University
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
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/pmix/pmix_server.h"
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

#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/propagate/propagate.h"
#include "src/mca/propagate/base/base.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/detector/errmgr_detector.h"

#include "propagate_prperror.h"

prrte_list_t prrte_error_procs = {{0}};

static int prrte_propagate_error_cb_type = -1;

static int init(void);
static int finalize(void);
static int register_prp_callback(void);
static int prrte_propagate_prperror(prrte_jobid_t *job, prrte_process_name_t *source,
        prrte_process_name_t *errorproc, prrte_proc_state_t state);

int prrte_propagate_prperror_recv(prrte_buffer_t* buffer);

/* flag use to register callback for grpcomm rbcast forward */
int enable_callback_register_flag = 1;

prrte_propagate_base_module_t prrte_propagate_prperror_module ={
    init,
    finalize,
    prrte_propagate_prperror,
    register_prp_callback
};

double RTE_Wtime_test(void)
{
    double wtime;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    wtime = tv.tv_sec;
    wtime += (double)tv.tv_usec / 1000000.0;
    return wtime;
}

static void flush_error_list(size_t evhdlr_registration_id,
        pmix_status_t status,
        const pmix_proc_t *psource,
        pmix_info_t info[], size_t ninfo,
        pmix_info_t *results, size_t nresults,
        pmix_event_notification_cbfunc_fn_t cbfunc,
        void *cbdata)
{
    PRRTE_OUTPUT_VERBOSE((2, prrte_propagate_base_framework.framework_output,
                "Flush error process list"));
    PRRTE_DESTRUCT(&prrte_error_procs);
    PRRTE_CONSTRUCT(&prrte_error_procs, prrte_list_t);
    if (NULL != cbfunc) {
        cbfunc(PRRTE_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/*
 *Local functions
 */
static int init(void)
{
    PRRTE_CONSTRUCT(&prrte_error_procs, prrte_list_t);
    pmix_status_t pcode1 = PMIX_ERR_JOB_TERMINATED;
    PMIx_Register_event_handler(&pcode1, 1, NULL, 0, flush_error_list, NULL, NULL);
    return PRRTE_SUCCESS;
}

static int register_prp_callback(void)
{
    int ret;
    if(enable_callback_register_flag)
    {
        if(prrte_grpcomm.register_cb!=NULL)
            ret= prrte_grpcomm.register_cb((prrte_grpcomm_rbcast_cb_t)prrte_propagate_prperror_recv);
            prrte_propagate_error_cb_type = ret;
            PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                        "propagate: prperror: daemon register grpcomm callback %d at start",prrte_propagate_error_cb_type));
            enable_callback_register_flag = 0;
    }
    return PRRTE_SUCCESS;
}

static int finalize(void)
{
    int ret=0;
    if ( -1 == prrte_propagate_error_cb_type){
        return PRRTE_SUCCESS;
    }
    /* do we need unregister ? cause all module is unloading, those memory maybe collecred may have illegal access */
    /* ret = prrte_grpcomm.unregister_cb(prrte_propagate_error_cb_type); */
    prrte_propagate_error_cb_type = -1;
    PRRTE_DESTRUCT(&prrte_error_procs);
    return ret;
}

/*
 * uplevel call from the error handler to initiate a failure_propagator
 */
static int prrte_propagate_prperror(prrte_jobid_t *job, prrte_process_name_t *source,
        prrte_process_name_t *errorproc, prrte_proc_state_t state) {

    int rc = PRRTE_SUCCESS;
    /* don't need to check jobid because this can be different: daemon and process has different jobids */

    bool daemon_error_flag = false;

    /* namelist for tracking error procs */
    prrte_namelist_t *nmcheck, *nm;
    nmcheck = PRRTE_NEW(prrte_namelist_t);

    PRRTE_LIST_FOREACH(nmcheck, &prrte_error_procs, prrte_namelist_t){
        if ((nmcheck->name.jobid == errorproc->jobid) && (nmcheck->name.vpid == errorproc->vpid))
        {
            PRRTE_OUTPUT_VERBOSE((10, prrte_propagate_base_framework.framework_output,
                        "propagate: prperror: already propagated this msg: error proc is %s", PRRTE_NAME_PRINT(errorproc) ));
            return rc;
        }
    }

    nm = PRRTE_NEW(prrte_namelist_t);
    nm->name.jobid = errorproc->jobid;
    nm->name.vpid = errorproc->vpid;
    prrte_list_append(&prrte_error_procs, &(nm->super));

    prrte_grpcomm_signature_t *sig;
    int cnt=0;
    /* ---------------------------------------------------------
     * | cb_type | status | errorproc | nprocs afftected | procs|
     * --------------------------------------------------------*/
    prrte_buffer_t prperror_buffer;

    /* set the status for pmix to use */
    prrte_proc_state_t status;
    status = state;

    /* register callback for rbcast for forwarding */
    int ret;
    if(enable_callback_register_flag)
    {
        if(prrte_grpcomm.register_cb!=NULL)
            ret= prrte_grpcomm.register_cb((prrte_grpcomm_rbcast_cb_t)prrte_propagate_prperror_recv);
            prrte_propagate_error_cb_type = ret;
            PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                        "propagate: prperror: daemon register grpcomm callback %d",prrte_propagate_error_cb_type));
            enable_callback_register_flag = 0;
    }

    /* change the error daemon state*/
    PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                "propagate: prperror: daemon %s rbcast state %d of proc %s",
                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),state, PRRTE_NAME_PRINT(errorproc)));

    PRRTE_CONSTRUCT(&prperror_buffer, prrte_buffer_t);
    /* pack the callback type */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&prperror_buffer, &prrte_propagate_error_cb_type, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&prperror_buffer);
        return rc;
    }
    /* pack the status */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&prperror_buffer, &status, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&prperror_buffer);
        return rc;
    }
    /* pack dead proc first */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&prperror_buffer, errorproc, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&prperror_buffer);
        return rc;
    }
    prrte_node_t *node;
    /* check this is a daemon or not, if vpid is same cannot ensure this is daemon also need check jobid*/
    if (errorproc->vpid == prrte_get_proc_daemon_vpid(errorproc) && (errorproc->jobid == PRRTE_PROC_MY_NAME->jobid) ){
        /* Given a node name, return an array of processes within the specified jobid
         * on that node. If the specified node does not currently host any processes,
         * then the returned list will be empty.
         */
        PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                    "%s propagate:daemon prperror this is a daemon error on %s \n",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    prrte_get_proc_hostname(errorproc)));

        node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, errorproc->vpid);

        cnt=node->num_procs;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&prperror_buffer, &cnt, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&prperror_buffer);
            return rc;
        }
        daemon_error_flag = true;
    }
    /* if process failure pack 0 affected for forwarding unpack*/
    else {
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&prperror_buffer, &cnt, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&prperror_buffer);
            return rc;
        }
    }

    pmix_proc_t pname;
    pmix_info_t *pinfo;
    PMIX_INFO_CREATE(pinfo, 1+cnt);

    PRRTE_PMIX_CONVERT_NAME(&pname, errorproc);
    PMIX_INFO_LOAD(&pinfo[0], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );

    if(daemon_error_flag) {
        prrte_proc_t *pptr;
        prrte_buffer_t *alert;
        prrte_plm_cmd_flag_t cmd;

        int i;
        for (i=0; i < cnt; i++) {
            if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                            " %d children are afftected  %s\n",cnt, PRRTE_NAME_PRINT(&pptr->name)));

                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&prperror_buffer, &pptr->name, 1, PRRTE_NAME))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_DESTRUCT(&prperror_buffer);
                    return rc;
                }
                PRRTE_PMIX_CONVERT_NAME(&pname, &pptr->name);
                PMIX_INFO_LOAD(&pinfo[i+1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );

                alert = PRRTE_NEW(prrte_buffer_t);
                /* pack update state command */
                cmd = PRRTE_PLM_UPDATE_PROC_STATE;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &cmd, 1, PRRTE_PLM_CMD))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }

                /* pack jobid */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &(pptr->name.jobid), 1, PRRTE_JOBID))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }

                /* proc state now is PRRTE_PROC_STATE_ABORTED_BY_SIG, cause odls set state to this; code is 128+9 */
                pptr->state = PRRTE_PROC_STATE_ABORTED_BY_SIG;
                /* pack the child's vpid */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &(pptr->name.vpid), 1, PRRTE_VPID))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
                /* pack the pid */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &pptr->pid, 1, PRRTE_PID))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
                /* pack its state */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &pptr->state, 1, PRRTE_PROC_STATE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
                /* pack its exit code */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(alert, &pptr->exit_code, 1, PRRTE_EXIT_CODE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
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
                if (PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
                        PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_WAITPID) &&
                        !PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_RECORDED)) {
                    PRRTE_ACTIVATE_PROC_STATE(&(pptr->name), PRRTE_PROC_STATE_TERMINATED);
                }
            }
        }
    }

    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    /* all daemons hosting this jobid are participating */
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.rbcast(sig, PRRTE_RML_TAG_PROPAGATE, &prperror_buffer))) {
        PRRTE_ERROR_LOG(rc);
    }
    /* notify this error locally, only from rbcast dont have a source id */
    if( source==NULL ) {
        if (PRRTE_SUCCESS != PMIx_Notify_event(prrte_pmix_convert_rc(state), NULL,
                    PMIX_RANGE_LOCAL, pinfo, cnt+1,
                    NULL,NULL )) {
            PRRTE_RELEASE(pinfo);
        }
    }
    //PRRTE_DESTRUCT(&prperror_buffer);
    PRRTE_RELEASE(sig);
    /* we're done! */
    return PRRTE_SUCCESS;
}

static int _prrte_propagate_prperror(prrte_jobid_t *job, prrte_process_name_t *source,
                prrte_process_name_t *errorproc, prrte_proc_state_t state, prrte_buffer_t* buffer,prrte_buffer_t* rly) {

    int rc = PRRTE_SUCCESS;
    /* don't need to check jobid because this can be different: daemon and process has different jobids */

    /* namelist for tracking error procs */
    prrte_namelist_t *nmcheck, *nm;
    nmcheck = PRRTE_NEW(prrte_namelist_t);

    PRRTE_LIST_FOREACH(nmcheck, &prrte_error_procs, prrte_namelist_t){
        if ((nmcheck->name.jobid == errorproc->jobid) && (nmcheck->name.vpid == errorproc->vpid))
        {
            PRRTE_OUTPUT_VERBOSE((10, prrte_propagate_base_framework.framework_output,
                        "propagate: prperror: already propagated this msg: error proc is %s", PRRTE_NAME_PRINT(errorproc) ));
            return rc;
        }
    }
    PRRTE_OUTPUT_VERBOSE((10, prrte_propagate_base_framework.framework_output,
                "propagate: prperror: interal forward: error proc is %s", PRRTE_NAME_PRINT(errorproc) ));

    nm = PRRTE_NEW(prrte_namelist_t);
    nm->name.jobid = errorproc->jobid;
    nm->name.vpid = errorproc->vpid;
    prrte_list_append(&prrte_error_procs, &(nm->super));
    /* goes to all daemons */
    prrte_grpcomm_signature_t *sig;
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    /* all daemons hosting this jobid are participating */
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.rbcast(sig, PRRTE_RML_TAG_PROPAGATE, rly))) {
        PRRTE_ERROR_LOG(rc);
    }

    pmix_proc_t pname;
    pmix_info_t *pinfo;
    int ret;
    int cnt=1;
    int num_affected = 0;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &num_affected, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(ret);
        return false;
    }
    PMIX_INFO_CREATE(pinfo, 1+num_affected);

    PRRTE_PMIX_CONVERT_NAME(&pname, errorproc);
    PMIX_INFO_LOAD(&pinfo[0], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );

    prrte_process_name_t ename;
    int i=0;
    for (i =0; i <num_affected; i++)
    {
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &ename, &cnt, PRRTE_NAME))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        PRRTE_PMIX_CONVERT_NAME(&pname, &ename);
        PMIX_INFO_LOAD(&pinfo[i+1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );
    }
    /* notify this error locally, only from rbcast dont have a source id */
    if( source==NULL ) {
        if (PRRTE_SUCCESS != PMIx_Notify_event(prrte_pmix_convert_rc(state), NULL,
                    PMIX_RANGE_LOCAL, pinfo, num_affected+1,
                    NULL,NULL )) {
            PRRTE_RELEASE(pinfo);
        }
    }
}


int prrte_propagate_prperror_recv(prrte_buffer_t* buffer)
{
    int ret, cnt, state;
    prrte_process_name_t errorproc;
    int cbtype;

    prrte_buffer_t rly;
    PRRTE_CONSTRUCT(&rly, prrte_buffer_t);

    prrte_dss.copy_payload(&rly, buffer);
    /* get the cbtype */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &cbtype, &cnt,PRRTE_INT ))) {
        PRRTE_ERROR_LOG(ret);
        return false;
    }
    cnt = 1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &state, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(ret);
        return false;
    }
    /* for propagate, only one major errorproc is affected per call */
    cnt = 1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &errorproc, &cnt, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(ret);
        return false;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                "%s propagete: prperror: daemon received %s gone forwarding with status %d",
                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_NAME_PRINT(&errorproc), state));

    _prrte_propagate_prperror(&prrte_process_info.my_name.jobid, NULL, &errorproc, state, buffer , &rly);
    return false;
}
