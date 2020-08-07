/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"

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

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/data_type_support/prte_dt_support.h"

#include "src/mca/propagate/propagate.h"
#include "src/mca/propagate/base/base.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/detector/errmgr_detector.h"

#include "propagate_prperror.h"

prte_list_t prte_error_procs = {{0}};

static int prte_propagate_error_cb_type = -1;

static int init(void);
static int finalize(void);
static int register_prp_callback(void);
static int prte_propagate_prperror(prte_jobid_t *job, prte_process_name_t *source,
        prte_process_name_t *errorproc, prte_proc_state_t state);

static int prte_propagate_prperror_recv(prte_buffer_t* buffer);

/* flag use to register callback for grpcomm rbcast forward */
int enable_callback_register_flag = 1;

prte_propagate_base_module_t prte_propagate_prperror_module ={
    init,
    finalize,
    prte_propagate_prperror,
    register_prp_callback
};


static void flush_error_list(size_t evhdlr_registration_id,
        pmix_status_t status,
        const pmix_proc_t *psource,
        pmix_info_t info[], size_t ninfo,
        pmix_info_t *results, size_t nresults,
        pmix_event_notification_cbfunc_fn_t cbfunc,
        void *cbdata)
{
    PRTE_OUTPUT_VERBOSE((2, prte_propagate_base_framework.framework_output,
                "Flush error process list"));
    PRTE_DESTRUCT(&prte_error_procs);
    PRTE_CONSTRUCT(&prte_error_procs, prte_list_t);
    if (NULL != cbfunc) {
        cbfunc(PRTE_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/*
 *Local functions
 */
static int init(void)
{
    PRTE_CONSTRUCT(&prte_error_procs, prte_list_t);
    pmix_status_t pcode1 = PMIX_ERR_JOB_TERMINATED;
    PMIx_Register_event_handler(&pcode1, 1, NULL, 0, flush_error_list, NULL, NULL);
    return PRTE_SUCCESS;
}

static int register_prp_callback(void)
{
    int ret;
    if(enable_callback_register_flag)
    {
        if(prte_grpcomm.register_cb!=NULL) {
            ret= prte_grpcomm.register_cb((prte_grpcomm_rbcast_cb_t)prte_propagate_prperror_recv);
            prte_propagate_error_cb_type = ret;
            PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                        "propagate: prperror: daemon register grpcomm callback %d at start",prte_propagate_error_cb_type));
            enable_callback_register_flag = 0;
        }
    }
    return PRTE_SUCCESS;
}

static int finalize(void)
{
    int ret=0;
    if ( -1 == prte_propagate_error_cb_type){
        return PRTE_SUCCESS;
    }
    /* do we need unregister ? cause all module is unloading, those memory maybe collected may have illegal access */
    /* ret = prte_grpcomm.unregister_cb(prte_propagate_error_cb_type); */
    prte_propagate_error_cb_type = -1;
    PRTE_DESTRUCT(&prte_error_procs);
    return ret;
}

/*
 * uplevel call from the error handler to initiate a failure_propagator
 */
static int prte_propagate_prperror(prte_jobid_t *job, prte_process_name_t *source,
        prte_process_name_t *errorproc, prte_proc_state_t state) {

    int rc = PRTE_SUCCESS;
    /* don't need to check jobid because this can be different: daemon and process has different jobids */

    bool daemon_error_flag = false;

    /* namelist for tracking error procs */
    prte_namelist_t *nmcheck, *nm;
    nmcheck = PRTE_NEW(prte_namelist_t);

    PRTE_LIST_FOREACH(nmcheck, &prte_error_procs, prte_namelist_t){
        if ((nmcheck->name.jobid == errorproc->jobid) && (nmcheck->name.vpid == errorproc->vpid))
        {
            PRTE_OUTPUT_VERBOSE((10, prte_propagate_base_framework.framework_output,
                        "propagate: prperror: already propagated this msg: error proc is %s", PRTE_NAME_PRINT(errorproc) ));
            return rc;
        }
    }

    nm = PRTE_NEW(prte_namelist_t);
    nm->name.jobid = errorproc->jobid;
    nm->name.vpid = errorproc->vpid;
    prte_list_append(&prte_error_procs, &(nm->super));

    prte_grpcomm_signature_t *sig;
    int cnt=0;
    /* ---------------------------------------------------------
     * | cb_type | status | errorproc | nprocs afftected | procs|
     * --------------------------------------------------------*/
    prte_buffer_t prperror_buffer;

    /* set the status for pmix to use */
    prte_proc_state_t status;
    status = state;

    /* register callback for rbcast for forwarding */
    int ret;
    if(enable_callback_register_flag)
    {
        if(prte_grpcomm.register_cb!=NULL) {
            ret= prte_grpcomm.register_cb((prte_grpcomm_rbcast_cb_t)prte_propagate_prperror_recv);
            prte_propagate_error_cb_type = ret;
            PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                        "propagate: prperror: daemon register grpcomm callback %d",prte_propagate_error_cb_type));
            enable_callback_register_flag = 0;
        }
    }

    /* change the error daemon state*/
    PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                "propagate: prperror: daemon %s rbcast state %d of proc %s",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),state, PRTE_NAME_PRINT(errorproc)));

    PRTE_CONSTRUCT(&prperror_buffer, prte_buffer_t);
    /* pack the callback type */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(&prperror_buffer, &prte_propagate_error_cb_type, 1, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&prperror_buffer);
        return rc;
    }
    /* pack the status */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(&prperror_buffer, &status, 1, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&prperror_buffer);
        return rc;
    }
    /* pack dead proc first */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(&prperror_buffer, errorproc, 1, PRTE_NAME))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&prperror_buffer);
        return rc;
    }
    prte_node_t *node;
    /* check this is a daemon or not, if vpid is same cannot ensure this is daemon also need check jobid*/
    if (errorproc->vpid == prte_get_proc_daemon_vpid(errorproc) && (errorproc->jobid == PRTE_PROC_MY_NAME->jobid) ){
        /* Given a node name, return an array of processes within the specified jobid
         * on that node. If the specified node does not currently host any processes,
         * then the returned list will be empty.
         */
        PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                    "%s propagate:daemon prperror this is a daemon error on %s \n",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    prte_get_proc_hostname(errorproc)));

        node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, errorproc->vpid);
        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PRTE_DESTRUCT(&prperror_buffer);
            return PRTE_ERR_NOT_FOUND;
        }

        cnt=node->num_procs; //prte issue, num_procs value is not correct
        if (PRTE_SUCCESS != (rc = prte_dss.pack(&prperror_buffer, &cnt, 1, PRTE_INT))) {
            PRTE_ERROR_LOG(rc);
            PRTE_DESTRUCT(&prperror_buffer);
            return rc;
        }
        daemon_error_flag = true;
    }
    /* if process failure pack 0 affected for forwarding unpack*/
    else {
        if (PRTE_SUCCESS != (rc = prte_dss.pack(&prperror_buffer, &cnt, 1, PRTE_INT))) {
            PRTE_ERROR_LOG(rc);
            PRTE_DESTRUCT(&prperror_buffer);
            return rc;
        }
    }

    pmix_proc_t pname;
    pmix_info_t *pinfo;
    size_t pcnt = 1 + cnt;

    PMIX_INFO_CREATE(pinfo, pcnt);

    PRTE_PMIX_CONVERT_NAME(rc, &pname, errorproc);
    PMIX_INFO_LOAD(&pinfo[0], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );

    if(daemon_error_flag) {
        prte_proc_t *pptr;
        prte_buffer_t *alert;
        prte_plm_cmd_flag_t cmd;

        int i;
        for (i=0; i < cnt; i++) {
            if (NULL != (pptr = (prte_proc_t*)prte_pointer_array_get_item(node->procs, i))) {
                PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                            " %d children are afftected  %s\n",cnt, PRTE_NAME_PRINT(&pptr->name)));

                if (PRTE_SUCCESS != (rc = prte_dss.pack(&prperror_buffer, &pptr->name, 1, PRTE_NAME))) {
                    PRTE_ERROR_LOG(rc);
                    PRTE_DESTRUCT(&prperror_buffer);
                    return rc;
                }
                PRTE_PMIX_CONVERT_NAME(rc, &pname, &pptr->name);
                PMIX_INFO_LOAD(&pinfo[i+1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );

                alert = PRTE_NEW(prte_buffer_t);
                /* pack update state command */
                cmd = PRTE_PLM_UPDATE_PROC_STATE;
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &cmd, 1, PRTE_PLM_CMD))) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_FREE(pinfo, pcnt);
                    return rc;
                }

                /* pack jobid */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &(pptr->name.jobid), 1, PRTE_JOBID))) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_FREE(pinfo, pcnt);
                    return rc;
                }

                /* proc state now is PRTE_PROC_STATE_ABORTED_BY_SIG, cause odls set state to this; code is 128+9 */
                pptr->state = PRTE_PROC_STATE_ABORTED_BY_SIG;
                /* pack the child's vpid */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &(pptr->name.vpid), 1, PRTE_VPID))) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_FREE(pinfo, pcnt);
                    return rc;
                }
                /* pack the pid */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &pptr->pid, 1, PRTE_PID))) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_FREE(pinfo, pcnt);
                    return rc;
                }
                /* pack its state */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &pptr->state, 1, PRTE_PROC_STATE))) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_FREE(pinfo, pcnt);
                    return rc;
                }
                /* pack its exit code */
                if (PRTE_SUCCESS != (rc = prte_dss.pack(alert, &pptr->exit_code, 1, PRTE_EXIT_CODE))) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_FREE(pinfo, pcnt);
                    return rc;
                }

                /* send this process's info to hnp */
                if (0 > (rc = prte_rml.send_buffer_nb(
                                PRTE_PROC_MY_HNP, alert,
                                PRTE_RML_TAG_PLM,
                                prte_rml_send_callback, NULL))) {
                    PRTE_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                "%s errmgr:detector: send to hnp failed",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(alert);
                }
                if (PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_IOF_COMPLETE) &&
                        PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_WAITPID) &&
                        !PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_RECORDED)) {
                    PRTE_ACTIVATE_PROC_STATE(&(pptr->name), PRTE_PROC_STATE_TERMINATED);
                }
            }
        }
    }

    /* goes to all daemons */
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    sig->signature = (prte_process_name_t*)malloc(sizeof(prte_process_name_t));
    sig->signature[0].jobid = PRTE_PROC_MY_NAME->jobid;
    /* all daemons hosting this jobid are participating */
    sig->signature[0].vpid = PRTE_VPID_WILDCARD;
    if (PRTE_SUCCESS != (rc = prte_grpcomm.rbcast(sig, PRTE_RML_TAG_PROPAGATE, &prperror_buffer))) {
        PRTE_ERROR_LOG(rc);
    }
    /* notify this error locally, only from rbcast dont have a source id */
    if( source==NULL ) {
        if (PRTE_SUCCESS != PMIx_Notify_event(prte_pmix_convert_rc(state), NULL,
                    PMIX_RANGE_LOCAL, pinfo, cnt+1,
                    NULL,NULL )) {
            PMIX_INFO_FREE(pinfo, pcnt);
        }
    }
    //PRTE_DESTRUCT(&prperror_buffer);
    PMIX_INFO_FREE(pinfo, pcnt);
    PRTE_RELEASE(sig);
    /* we're done! */
    return PRTE_SUCCESS;
}

static int _prte_propagate_prperror(prte_jobid_t *job, prte_process_name_t *source,
                prte_process_name_t *errorproc, prte_proc_state_t state, prte_buffer_t* buffer,prte_buffer_t* rly) {

    int rc = PRTE_SUCCESS;
    /* don't need to check jobid because this can be different: daemon and process has different jobids */

    /* namelist for tracking error procs */
    prte_namelist_t *nmcheck, *nm;
    nmcheck = PRTE_NEW(prte_namelist_t);

    PRTE_LIST_FOREACH(nmcheck, &prte_error_procs, prte_namelist_t){
        if ((nmcheck->name.jobid == errorproc->jobid) && (nmcheck->name.vpid == errorproc->vpid))
        {
            PRTE_OUTPUT_VERBOSE((10, prte_propagate_base_framework.framework_output,
                        "propagate: prperror: already propagated this msg: error proc is %s", PRTE_NAME_PRINT(errorproc) ));
            return rc;
        }
    }
    PRTE_OUTPUT_VERBOSE((10, prte_propagate_base_framework.framework_output,
                "propagate: prperror: interal forward: error proc is %s", PRTE_NAME_PRINT(errorproc) ));

    nm = PRTE_NEW(prte_namelist_t);
    nm->name.jobid = errorproc->jobid;
    nm->name.vpid = errorproc->vpid;
    prte_list_append(&prte_error_procs, &(nm->super));
    /* goes to all daemons */
    prte_grpcomm_signature_t *sig;
    sig = PRTE_NEW(prte_grpcomm_signature_t);
    sig->signature = (prte_process_name_t*)malloc(sizeof(prte_process_name_t));
    sig->signature[0].jobid = PRTE_PROC_MY_NAME->jobid;
    /* all daemons hosting this jobid are participating */
    sig->signature[0].vpid = PRTE_VPID_WILDCARD;
    if (PRTE_SUCCESS != (rc = prte_grpcomm.rbcast(sig, PRTE_RML_TAG_PROPAGATE, rly))) {
        PRTE_ERROR_LOG(rc);
    }

    pmix_proc_t pname;
    pmix_info_t *pinfo;
    int ret;
    int cnt=1;
    int num_affected = 0;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &num_affected, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(ret);
        return false;
    }
    PMIX_INFO_CREATE(pinfo, 1+num_affected);

    PRTE_PMIX_CONVERT_NAME(rc, &pname, errorproc);
    PMIX_INFO_LOAD(&pinfo[0], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );

    prte_process_name_t ename;
    int i=0;
    for (i =0; i <num_affected; i++)
    {
        if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &ename, &cnt, PRTE_NAME))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        PRTE_PMIX_CONVERT_NAME(rc, &pname, &ename);
        PMIX_INFO_LOAD(&pinfo[i+1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC );
    }
    /* notify this error locally, only from rbcast dont have a source id */
    if( source==NULL ) {
        if (PRTE_SUCCESS != PMIx_Notify_event(prte_pmix_convert_rc(state), NULL,
                    PMIX_RANGE_LOCAL, pinfo, num_affected+1,
                    NULL,NULL )) {
            PRTE_RELEASE(pinfo);
        }
    }
    return rc;
}


static int prte_propagate_prperror_recv(prte_buffer_t* buffer)
{
    int ret, cnt, state;
    prte_process_name_t errorproc;
    int cbtype;

    prte_buffer_t rly;
    PRTE_CONSTRUCT(&rly, prte_buffer_t);

    prte_dss.copy_payload(&rly, buffer);
    /* get the cbtype */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &cbtype, &cnt,PRTE_INT ))) {
        PRTE_ERROR_LOG(ret);
        return false;
    }
    cnt = 1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &state, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(ret);
        return false;
    }
    /* for propagate, only one major errorproc is affected per call */
    cnt = 1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &errorproc, &cnt, PRTE_NAME))) {
        PRTE_ERROR_LOG(ret);
        return false;
    }

    PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                "%s propagete: prperror: daemon received %s gone forwarding with status %d",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&errorproc), state));

    _prte_propagate_prperror(&prte_process_info.my_name.jobid, NULL, &errorproc, state, buffer , &rly);
    return false;
}
