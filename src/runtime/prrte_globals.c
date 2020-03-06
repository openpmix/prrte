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
 * Copyright (c) 2007-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2009-2010 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/class/prrte_hash_table.h"
#include "src/class/prrte_pointer_array.h"
#include "src/class/prrte_value_array.h"
#include "src/dss/dss.h"
#include "src/threads/threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/util/proc_info.h"
#include "src/util/name_fns.h"

#include "src/runtime/runtime.h"
#include "src/runtime/runtime_internals.h"
#include "src/runtime/prrte_globals.h"

/* need the data type support functions here */
#include "src/runtime/data_type_support/prrte_dt_support.h"

/* State Machine */
prrte_list_t prrte_job_states = {{0}};
prrte_list_t prrte_proc_states = {{0}};

/* a clean output channel without prefix */
int prrte_clean_output = -1;

/* globals used by RTE */
bool prrte_debug_daemons_file_flag = false;
bool prrte_leave_session_attached = false;
bool prrte_do_not_launch = false;
char *prrte_local_cpu_type = NULL;
char *prrte_local_cpu_model = NULL;
bool prrte_coprocessors_detected = false;
prrte_hash_table_t *prrte_coprocessors = NULL;
char *prrte_topo_signature = NULL;
char *prrte_data_server_uri = NULL;
char *prrte_tool_basename = NULL;
bool prrte_dvm_ready = false;
prrte_pointer_array_t *prrte_cache = NULL;

/* PRRTE OOB port flags */
bool prrte_static_ports = false;
char *prrte_oob_static_ports = NULL;

bool prrte_keep_fqdn_hostnames = false;
bool prrte_have_fqdn_allocation = false;
bool prrte_show_resolved_nodenames = false;
int prrte_use_hostname_alias = -1;
int prrte_hostname_cutoff = 1000;

int prted_debug_failure = -1;
int prted_debug_failure_delay = -1;
bool prrte_never_launched = false;
bool prrte_devel_level_output = false;
bool prrte_display_topo_with_map = false;
bool prrte_display_diffable_output = false;

char **prrte_launch_environ = NULL;

bool prrte_hnp_is_allocated = false;
bool prrte_allocation_required = false;
bool prrte_managed_allocation = false;
char *prrte_set_slots = NULL;
bool prrte_soft_locations = false;
bool prrte_nidmap_communicated = false;
bool prrte_node_info_communicated = false;

/* launch agents */
char *prrte_launch_agent = NULL;
char **prted_cmd_line=NULL;
char **prrte_fork_agent=NULL;

/* exit flags */
int prrte_exit_status = 0;
bool prrte_abnormal_term_ordered = false;
bool prrte_routing_is_enabled = true;
bool prrte_job_term_ordered = false;
bool prrte_prteds_term_ordered = false;
bool prrte_allowed_exit_without_sync = false;

int prrte_startup_timeout = -1;
int prrte_timeout_usec_per_proc = -1;
float prrte_max_timeout = -1.0;
prrte_timer_t *prrte_mpiexec_timeout = NULL;

int prrte_stack_trace_wait_timeout = 30;

/* global arrays for data storage */
prrte_hash_table_t *prrte_job_data = NULL;
prrte_pointer_array_t *prrte_node_pool = NULL;
prrte_pointer_array_t *prrte_node_topologies = NULL;
prrte_pointer_array_t *prrte_local_children = NULL;
prrte_vpid_t prrte_total_procs = 0;

/* IOF controls */
bool prrte_tag_output = false;
bool prrte_timestamp_output = false;
/* generate new xterm windows to display output from specified ranks */
char *prrte_xterm = NULL;

/* report launch progress */
bool prrte_report_launch_progress = false;

/* allocation specification */
char *prrte_default_hostfile = NULL;
bool prrte_default_hostfile_given = false;
char *prrte_rankfile = NULL;
int prrte_num_allocated_nodes = 0;
char *prrte_default_dash_host = NULL;

/* tool communication controls */
bool prrte_report_events = false;
char *prrte_report_events_uri = NULL;

/* report bindings */
bool prrte_report_bindings = false;

/* barrier control */
bool prrte_do_not_barrier = false;

/* process recovery */
bool prrte_enable_recovery = false;
int32_t prrte_max_restarts = 0;

/* exit status reporting */
bool prrte_report_child_jobs_separately = false;
struct timeval prrte_child_time_to_exit = {0};
bool prrte_abort_non_zero_exit = false;

/* length of stat history to keep */
int prrte_stat_history_size = -1;

/* envars to forward */
char **prrte_forwarded_envars = NULL;

/* map stddiag output to stderr so it isn't forwarded to mpirun */
bool prrte_map_stddiag_to_stderr = false;
bool prrte_map_stddiag_to_stdout = false;

/* maximum size of virtual machine - used to subdivide allocation */
int prrte_max_vm_size = -1;

int prrte_debug_output = -1;
bool prrte_debug_daemons_flag = false;
bool prrte_xml_output = false;
FILE *prrte_xml_fp = NULL;
char *prrte_job_ident = NULL;
bool prrte_execute_quiet = false;
bool prrte_report_silent_errors = false;
bool prrte_hwloc_shmem_available = false;

/* See comment in src/tools/prun/debuggers.c about this MCA
   param */
bool prrte_in_parallel_debugger = false;

char *prrte_daemon_cores = NULL;

int prrte_dt_init(void)
{
    int rc;
    prrte_data_type_t tmp;

    /* set default output */
    prrte_debug_output = prrte_output_open(NULL);

    /* open up the verbose output for PRRTE debugging */
    if (prrte_debug_flag || 0 < prrte_debug_verbosity ||
        (prrte_debug_daemons_flag && (PRRTE_PROC_IS_DAEMON || PRRTE_PROC_IS_MASTER))) {
        if (0 < prrte_debug_verbosity) {
            prrte_output_set_verbosity(prrte_debug_output, prrte_debug_verbosity);
        } else {
            prrte_output_set_verbosity(prrte_debug_output, 1);
        }
    }

    /** register the base system types with the DSS */
    tmp = PRRTE_STD_CNTR;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_std_cntr,
                                                     prrte_dt_unpack_std_cntr,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_std_cntr,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_std_cntr,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_STD_CNTR", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_JOB;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_job,
                                                     prrte_dt_unpack_job,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_job,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_job,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_job,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_JOB", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_NODE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_node,
                                                     prrte_dt_unpack_node,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_node,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_node,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_node,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_NODE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_PROC;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_proc,
                                                     prrte_dt_unpack_proc,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_proc,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_proc,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_proc,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_PROC", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_APP_CONTEXT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_app_context,
                                                     prrte_dt_unpack_app_context,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_app_context,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_app_context,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_app_context,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_APP_CONTEXT", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_NODE_STATE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_node_state,
                                                     prrte_dt_unpack_node_state,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_node_state,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_node_state,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_NODE_STATE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_PROC_STATE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_proc_state,
                                                     prrte_dt_unpack_proc_state,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_proc_state,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_proc_state,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_PROC_STATE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_JOB_STATE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_job_state,
                                                     prrte_dt_unpack_job_state,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_job_state,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_job_state,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_JOB_STATE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_EXIT_CODE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_exit_code,
                                                     prrte_dt_unpack_exit_code,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_exit_code,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_exit_code,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_EXIT_CODE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_JOB_MAP;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_map,
                                                     prrte_dt_unpack_map,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_map,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_map,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_map,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_JOB_MAP", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_RML_TAG;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_tag,
                                                      prrte_dt_unpack_tag,
                                                      (prrte_dss_copy_fn_t)prrte_dt_copy_tag,
                                                      (prrte_dss_compare_fn_t)prrte_dt_compare_tags,
                                                      (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                      PRRTE_DSS_UNSTRUCTURED,
                                                      "PRRTE_RML_TAG", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_DAEMON_CMD;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_daemon_cmd,
                                                     prrte_dt_unpack_daemon_cmd,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_daemon_cmd,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_daemon_cmd,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_DAEMON_CMD", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_IOF_TAG;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_iof_tag,
                                                     prrte_dt_unpack_iof_tag,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_iof_tag,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_iof_tag,
                                                     (prrte_dss_print_fn_t)prrte_dt_std_print,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_IOF_TAG", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_ATTRIBUTE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_attr,
                                                     prrte_dt_unpack_attr,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_attr,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_attr,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_attr,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_ATTRIBUTE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    tmp = PRRTE_SIGNATURE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dt_pack_sig,
                                                     prrte_dt_unpack_sig,
                                                     (prrte_dss_copy_fn_t)prrte_dt_copy_sig,
                                                     (prrte_dss_compare_fn_t)prrte_dt_compare_sig,
                                                     (prrte_dss_print_fn_t)prrte_dt_print_sig,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_SIGNATURE", &tmp))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}

prrte_job_t* prrte_get_job_data_object(prrte_jobid_t job)
{
    prrte_job_t *jdata;

    /* if the job data wasn't setup, we cannot provide the data */
    if (NULL == prrte_job_data) {
        return NULL;
    }

    jdata = NULL;
    prrte_hash_table_get_value_uint32(prrte_job_data, job, (void**)&jdata);
    return jdata;
}

prrte_proc_t* prrte_get_proc_object(prrte_process_name_t *proc)
{
    prrte_job_t *jdata;
    prrte_proc_t *proct;

    if (NULL == (jdata = prrte_get_job_data_object(proc->jobid))) {
        return NULL;
    }
    proct = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid);
    return proct;
}

prrte_vpid_t prrte_get_proc_daemon_vpid(prrte_process_name_t *proc)
{
    prrte_job_t *jdata;
    prrte_proc_t *proct;

    if (NULL == (jdata = prrte_get_job_data_object(proc->jobid))) {
        return PRRTE_VPID_INVALID;
    }
    if (NULL == (proct = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, proc->vpid))) {
        return PRRTE_VPID_INVALID;
    }
    if (NULL == proct->node || NULL == proct->node->daemon) {
        return PRRTE_VPID_INVALID;
    }
    return proct->node->daemon->name.vpid;
}

char* prrte_get_proc_hostname(prrte_process_name_t *proc)
{
    prrte_proc_t *proct;

    /* don't bother error logging any not-found situations
     * as the layer above us will have something to say
     * about it */

    /* look it up on our arrays */
    if (NULL == (proct = prrte_get_proc_object(proc))) {
        return NULL;
    }
    if (NULL == proct->node || NULL == proct->node->name) {
        return NULL;
    }
    return proct->node->name;
}

prrte_node_rank_t prrte_get_proc_node_rank(prrte_process_name_t *proc)
{
    prrte_proc_t *proct;

    /* look it up on our arrays */
    if (NULL == (proct = prrte_get_proc_object(proc))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_NODE_RANK_INVALID;
    }
    return proct->node_rank;
}

bool prrte_node_match(prrte_node_t *n1, char *name)
{
    char **n2names = NULL;
    char *n2alias = NULL;
    char **n1names = NULL;
    char *n1alias = NULL;
    int i, m;
    prrte_node_t *nptr;

    /* start with the simple check */
    if (0 == strcmp(n1->name, name)) {
        return true;
    }

    /* get the aliases for n1 and check those against "name" */
    if (prrte_get_attribute(&n1->attributes, PRRTE_NODE_ALIAS, (void**)&n1alias, PRRTE_STRING)) {
        n1names = prrte_argv_split(n1alias, ',');
        free(n1alias);
    }
    if (NULL != n1names) {
        for (i=0; NULL != n1names[i]; i++) {
            if (0 == strcmp(name, n1names[i])) {
                prrte_argv_free(n1names);
                return true;
            }
        }
    }

    /* "name" itself might be an alias, so find the node object for this name */
    for (i=0; i < prrte_node_pool->size; i++) {
        if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
            continue;
        }
        if (prrte_get_attribute(&nptr->attributes, PRRTE_NODE_ALIAS, (void**)&n2alias, PRRTE_STRING)) {
            n2names = prrte_argv_split(n2alias, ',');
            free(n2alias);
        }
        if (NULL == n2names) {
            continue;
        }
        /* no choice but an exhaustive search - fortunately, these lists are short! */
        for (m=0; NULL != n2names[m]; m++) {
            if (0 == strcmp(name, n2names[m])) {
                /* this is the node! */
                goto complete;
            }
        }
        prrte_argv_free(n2names);
        n2names = NULL;
    }
    return false;

  complete:
    /* only get here is we found the node for "name" */
    if (NULL == n1names) {
        for (m=0; NULL != n2names[m]; m++) {
            if (0 == strcmp(n1->name, n2names[m])) {
                prrte_argv_free(n2names);
                return true;
            }
        }
    } else {
        for (i=0; NULL != n1names[i]; i++) {
            for (m=0; NULL != n2names[m]; m++) {
                if (0 == strcmp(n1->name, n2names[m])) {
                    prrte_argv_free(n1names);
                    prrte_argv_free(n2names);
                    return true;
                }
            }
        }
    }
    if (NULL != n1names) {
        prrte_argv_free(n1names);
    }
    if (NULL != n2names) {
        prrte_argv_free(n2names);
    }
    return false;
}

/*
 * CONSTRUCTORS, DESTRUCTORS, AND CLASS INSTANTIATIONS
 * FOR PRRTE CLASSES
 */

static void prrte_app_context_construct(prrte_app_context_t* app_context)
{
    app_context->idx=0;
    app_context->app=NULL;
    app_context->num_procs=0;
    PRRTE_CONSTRUCT(&app_context->procs, prrte_pointer_array_t);
    prrte_pointer_array_init(&app_context->procs,
                            1,
                            PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                            16);
    app_context->state = PRRTE_APP_STATE_UNDEF;
    app_context->first_rank = 0;
    app_context->argv=NULL;
    app_context->env=NULL;
    app_context->cwd=NULL;
    app_context->flags = 0;
    PRRTE_CONSTRUCT(&app_context->attributes, prrte_list_t);
}

static void prrte_app_context_destructor(prrte_app_context_t* app_context)
{
    int i;
    prrte_proc_t *proc;

    if (NULL != app_context->app) {
        free (app_context->app);
        app_context->app = NULL;
    }

    for (i=0; i < app_context->procs.size; i++) {
        if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(&app_context->procs, i))) {
            PRRTE_RELEASE(proc);
        }
    }
    PRRTE_DESTRUCT(&app_context->procs);

    /* argv and env lists created by util/argv copy functions */
    if (NULL != app_context->argv) {
        prrte_argv_free(app_context->argv);
        app_context->argv = NULL;
    }

    if (NULL != app_context->env) {
        prrte_argv_free(app_context->env);
        app_context->env = NULL;
    }

    if (NULL != app_context->cwd) {
        free (app_context->cwd);
        app_context->cwd = NULL;
    }

    PRRTE_LIST_DESTRUCT(&app_context->attributes);
}

PRRTE_CLASS_INSTANCE(prrte_app_context_t,
                   prrte_object_t,
                   prrte_app_context_construct,
                   prrte_app_context_destructor);

static void prrte_job_construct(prrte_job_t* job)
{
    job->exit_code = 0;
    job->personality = NULL;
    job->jobid = PRRTE_JOBID_INVALID;
    PMIX_LOAD_NSPACE(job->nspace, NULL);
    job->offset = 0;
    job->apps = PRRTE_NEW(prrte_pointer_array_t);
    prrte_pointer_array_init(job->apps,
                            1,
                            PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                            2);
    job->num_apps = 0;
    job->stdin_target = 0;
    job->total_slots_alloc = 0;
    job->num_procs = 0;
    job->procs = PRRTE_NEW(prrte_pointer_array_t);
    prrte_pointer_array_init(job->procs,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                            PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    job->map = NULL;
    job->bookmark = NULL;
    job->bkmark_obj = 0;
    job->state = PRRTE_JOB_STATE_UNDEF;

    job->num_mapped = 0;
    job->num_launched = 0;
    job->num_reported = 0;
    job->num_terminated = 0;
    job->num_daemons_reported = 0;

    job->originator.jobid = PRRTE_JOBID_INVALID;
    job->originator.vpid = PRRTE_VPID_INVALID;
    job->num_local_procs = 0;

    job->flags = 0;
    PRRTE_FLAG_SET(job, PRRTE_JOB_FLAG_FORWARD_OUTPUT);

    PRRTE_CONSTRUCT(&job->attributes, prrte_list_t);
    PRRTE_CONSTRUCT(&job->launch_msg, prrte_buffer_t);
    PRRTE_CONSTRUCT(&job->children, prrte_list_t);
    job->launcher = PRRTE_JOBID_INVALID;
}

static void prrte_job_destruct(prrte_job_t* job)
{
    prrte_proc_t *proc;
    prrte_app_context_t *app;
    int n;
    prrte_timer_t *evtimer;

    if (NULL == job) {
        /* probably just a race condition - just return */
        return;
    }

    if (prrte_debug_flag) {
        prrte_output(0, "%s Releasing job data for %s",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_JOBID_PRINT(job->jobid));
    }

    if (NULL != job->personality) {
        prrte_argv_free(job->personality);
    }
    for (n=0; n < job->apps->size; n++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(job->apps, n))) {
            continue;
        }
        PRRTE_RELEASE(app);
    }
    PRRTE_RELEASE(job->apps);

    /* release any pointers in the attributes */
    evtimer = NULL;
    if (prrte_get_attribute(&job->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT,
                           (void**)&evtimer, PRRTE_PTR)) {
        prrte_remove_attribute(&job->attributes, PRRTE_JOB_FAILURE_TIMER_EVENT);
        /* the timer is a pointer to prrte_timer_t */
        PRRTE_RELEASE(evtimer);
    }
    proc = NULL;
    if (prrte_get_attribute(&job->attributes, PRRTE_JOB_ABORTED_PROC,
                           (void**)&proc, PRRTE_PTR)) {
        prrte_remove_attribute(&job->attributes, PRRTE_JOB_ABORTED_PROC);
        /* points to an prrte_proc_t */
        PRRTE_RELEASE(proc);
    }

    if (NULL != job->map) {
        PRRTE_RELEASE(job->map);
        job->map = NULL;
    }

    for (n=0; n < job->procs->size; n++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(job->procs, n))) {
            continue;
        }
        PRRTE_RELEASE(proc);
    }
    PRRTE_RELEASE(job->procs);

    /* release the attributes */
    PRRTE_LIST_DESTRUCT(&job->attributes);

    PRRTE_DESTRUCT(&job->launch_msg);

    PRRTE_LIST_DESTRUCT(&job->children);

    if (NULL != prrte_job_data && PRRTE_JOBID_INVALID != job->jobid) {
        /* remove the job from the global array */
        prrte_hash_table_remove_value_uint32(prrte_job_data, job->jobid);
    }

}

PRRTE_CLASS_INSTANCE(prrte_job_t,
                   prrte_list_item_t,
                   prrte_job_construct,
                   prrte_job_destruct);


static void prrte_node_construct(prrte_node_t* node)
{
    node->index = -1;
    node->name = NULL;
    node->daemon = NULL;

    node->num_procs = 0;
    node->procs = PRRTE_NEW(prrte_pointer_array_t);
    prrte_pointer_array_init(node->procs,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                            PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    node->next_node_rank = 0;

    node->state = PRRTE_NODE_STATE_UNKNOWN;
    node->slots = 0;
    node->slots_inuse = 0;
    node->slots_max = 0;
    node->topology = NULL;

    node->flags = 0;
    PRRTE_CONSTRUCT(&node->attributes, prrte_list_t);
}

static void prrte_node_destruct(prrte_node_t* node)
{
    int i;
    prrte_proc_t *proc;

    if (NULL != node->name) {
        free(node->name);
        node->name = NULL;
    }

    if (NULL != node->daemon) {
        node->daemon->node = NULL;
        PRRTE_RELEASE(node->daemon);
        node->daemon = NULL;
    }

    for (i=0; i < node->procs->size; i++) {
        if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
            prrte_pointer_array_set_item(node->procs, i, NULL);
            PRRTE_RELEASE(proc);
        }
    }
    PRRTE_RELEASE(node->procs);

    /* do NOT destroy the topology */

    /* release the attributes */
    PRRTE_LIST_DESTRUCT(&node->attributes);
}


PRRTE_CLASS_INSTANCE(prrte_node_t,
                   prrte_list_item_t,
                   prrte_node_construct,
                   prrte_node_destruct);



static void prrte_proc_construct(prrte_proc_t* proc)
{
    proc->name = *PRRTE_NAME_INVALID;
    proc->job = NULL;
    proc->rank = PMIX_RANK_INVALID;
    proc->pid = 0;
    proc->local_rank = PRRTE_LOCAL_RANK_INVALID;
    proc->node_rank = PRRTE_NODE_RANK_INVALID;
    proc->app_rank = -1;
    proc->last_errmgr_state = PRRTE_PROC_STATE_UNDEF;
    proc->state = PRRTE_PROC_STATE_UNDEF;
    proc->app_idx = 0;
    proc->node = NULL;
    proc->exit_code = 0;      /* Assume we won't fail unless otherwise notified */
    proc->rml_uri = NULL;
    proc->flags = 0;
    PRRTE_CONSTRUCT(&proc->attributes, prrte_list_t);
}

static void prrte_proc_destruct(prrte_proc_t* proc)
{
    if (NULL != proc->node) {
        PRRTE_RELEASE(proc->node);
        proc->node = NULL;
    }

    if (NULL != proc->rml_uri) {
        free(proc->rml_uri);
        proc->rml_uri = NULL;
    }

    PRRTE_LIST_DESTRUCT(&proc->attributes);
}

PRRTE_CLASS_INSTANCE(prrte_proc_t,
                   prrte_list_item_t,
                   prrte_proc_construct,
                   prrte_proc_destruct);

static void prrte_job_map_construct(prrte_job_map_t* map)
{
    map->req_mapper = NULL;
    map->last_mapper = NULL;
    map->mapping = 0;
    map->ranking = 0;
    map->binding = 0;
    map->ppr = NULL;
    map->cpus_per_rank = 0;
    map->display_map = false;
    map->num_new_daemons = 0;
    map->daemon_vpid_start = PRRTE_VPID_INVALID;
    map->num_nodes = 0;
    map->nodes = PRRTE_NEW(prrte_pointer_array_t);
    prrte_pointer_array_init(map->nodes,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                            PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE);
}

static void prrte_job_map_destruct(prrte_job_map_t* map)
{
    prrte_std_cntr_t i;
    prrte_node_t *node;

    if (NULL != map->req_mapper) {
        free(map->req_mapper);
    }
    if (NULL != map->last_mapper) {
        free(map->last_mapper);
    }
    if (NULL != map->ppr) {
        free(map->ppr);
    }
    for (i=0; i < map->nodes->size; i++) {
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            PRRTE_RELEASE(node);
            prrte_pointer_array_set_item(map->nodes, i, NULL);
        }
    }
    PRRTE_RELEASE(map->nodes);
}

PRRTE_CLASS_INSTANCE(prrte_job_map_t,
                   prrte_object_t,
                   prrte_job_map_construct,
                   prrte_job_map_destruct);

static void prrte_attr_cons(prrte_attribute_t* p)
{
    p->key = 0;
    p->local = true;  // default to local-only data
    memset(&p->data, 0, sizeof(p->data));
}
static void prrte_attr_des(prrte_attribute_t *p)
{
    if (PRRTE_BYTE_OBJECT == p->type) {
        if (NULL != p->data.bo.bytes) {
            free(p->data.bo.bytes);
        }
    } else if (PRRTE_BUFFER == p->type) {
        PRRTE_DESTRUCT(&p->data.buf);
    } else if (PRRTE_STRING == p->type) {
        free(p->data.string);
    } else if (PRRTE_ENVAR == p->type) {
        free(p->data.envar.envar);
        free(p->data.envar.value);
    }
}
PRRTE_CLASS_INSTANCE(prrte_attribute_t,
                   prrte_list_item_t,
                   prrte_attr_cons, prrte_attr_des);

static void tcon(prrte_topology_t *t)
{
    t->topo = NULL;
    t->sig = NULL;
}
static void tdes(prrte_topology_t *t)
{
    if (NULL != t->topo) {
        prrte_hwloc_base_free_topology(t->topo);
    }
    if (NULL != t->sig) {
        free(t->sig);
    }
}
PRRTE_CLASS_INSTANCE(prrte_topology_t,
                   prrte_object_t,
                   tcon, tdes);
