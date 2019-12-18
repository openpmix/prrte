/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif


#include "src/dss/dss.h"
#include "src/event/event-internal.h"

#include "src/mca/odls/odls_types.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_wait.h"
#include "src/prted/prted.h"

#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"

#if 0
static void failed_cmd(int fd, short event, void *cbdata)
{
    prrte_timer_t *tm = (prrte_timer_t*)cbdata;

    /* we get called if an abnormal term
     * don't complete in time - just force exit
     */
    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:orted_cmd command timed out",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
    PRRTE_RELEASE(tm);
/*
    PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
*/
}
#endif

int prrte_plm_base_prted_exit(prrte_daemon_cmd_flag_t command)
{
    int rc;
    prrte_buffer_t *cmd;
    prrte_daemon_cmd_flag_t cmmnd;
    prrte_grpcomm_signature_t *sig;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:orted_cmd sending orted_exit commands",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* flag that orteds are being terminated */
    prrte_prteds_term_ordered = true;
    cmmnd = command;

    /* if we are terminating before launch, or abnormally
     * terminating, then the daemons may not be wired up
     * and therefore cannot depend on detecting their
     * routed children to determine termination
     */
    if (prrte_abnormal_term_ordered ||
        prrte_never_launched ||
        !prrte_routing_is_enabled) {
        cmmnd = PRRTE_DAEMON_HALT_VM_CMD;
    }

    /* send it express delivery! */
    cmd = PRRTE_NEW(prrte_buffer_t);
    /* pack the command */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &cmmnd, 1, PRRTE_DAEMON_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cmd);
        return rc;
    }
    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, cmd))) {
        PRRTE_ERROR_LOG(rc);
    }
    PRRTE_RELEASE(cmd);
    PRRTE_RELEASE(sig);

#if 0
    /* if we are abnormally ordering the termination, then
     * set a timeout in case it never finishes
     */
    if (prrte_abnormal_term_ordered) {
        PRRTE_DETECT_TIMEOUT(prrte_process_info.num_procs, 100, 3, failed_cmd, NULL);
    }
#endif
    return rc;
}


int prrte_plm_base_prted_terminate_job(prrte_jobid_t jobid)
{
    prrte_pointer_array_t procs;
    prrte_proc_t proc;
    int rc;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:prted_terminate job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jobid)));

    PRRTE_CONSTRUCT(&procs, prrte_pointer_array_t);
    prrte_pointer_array_init(&procs, 1, 1, 1);
    PRRTE_CONSTRUCT(&proc, prrte_proc_t);
    proc.name.jobid = jobid;
    proc.name.vpid = PRRTE_VPID_WILDCARD;
    prrte_pointer_array_add(&procs, &proc);
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_prted_kill_local_procs(&procs))) {
        PRRTE_ERROR_LOG(rc);
    }
    PRRTE_DESTRUCT(&procs);
    PRRTE_DESTRUCT(&proc);
    return rc;
}

int prrte_plm_base_prted_kill_local_procs(prrte_pointer_array_t *procs)
{
    int rc;
    prrte_buffer_t *cmd;
    prrte_daemon_cmd_flag_t command=PRRTE_DAEMON_KILL_LOCAL_PROCS;
    int v;
    prrte_proc_t *proc;
    prrte_grpcomm_signature_t *sig;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:orted_cmd sending kill_local_procs cmds",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    cmd = PRRTE_NEW(prrte_buffer_t);
    /* pack the command */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &command, 1, PRRTE_DAEMON_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(cmd);
        return rc;
    }

    /* pack the proc names */
    if (NULL != procs) {
        for (v=0; v < procs->size; v++) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(procs, v))) {
                continue;
            }
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(cmd, &(proc->name), 1, PRRTE_NAME))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(cmd);
                return rc;
            }
        }
    }
    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, cmd))) {
        PRRTE_ERROR_LOG(rc);
    }
    PRRTE_RELEASE(cmd);
    PRRTE_RELEASE(sig);

    /* we're done! */
    return rc;
}


int prrte_plm_base_prted_signal_local_procs(prrte_jobid_t job, int32_t signal)
{
    int rc;
    prrte_buffer_t cmd;
    prrte_daemon_cmd_flag_t command=PRRTE_DAEMON_SIGNAL_LOCAL_PROCS;
    prrte_grpcomm_signature_t *sig;

    PRRTE_OUTPUT_VERBOSE((5, prrte_plm_base_framework.framework_output,
                         "%s plm:base:prted_cmd sending signal_local_procs cmds",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    PRRTE_CONSTRUCT(&cmd, prrte_buffer_t);

    /* pack the command */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&cmd, &command, 1, PRRTE_DAEMON_CMD))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&cmd);
        return rc;
    }

    /* pack the jobid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&cmd, &job, 1, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&cmd);
        return rc;
    }

    /* pack the signal */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&cmd, &signal, 1, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&cmd);
        return rc;
    }

    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_DAEMON, &cmd))) {
        PRRTE_ERROR_LOG(rc);
    }
    PRRTE_DESTRUCT(&cmd);
    PRRTE_RELEASE(sig);

    /* we're done! */
    return PRRTE_SUCCESS;
}
