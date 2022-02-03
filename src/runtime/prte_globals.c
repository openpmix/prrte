/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2009-2010 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/class/prte_hash_table.h"
#include "src/class/prte_pointer_array.h"
#include "src/class/prte_value_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/threads/threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/rml/rml.h"
#include "src/util/argv.h"
#include "src/util/name_fns.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/util/proc_info.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/runtime/runtime_internals.h"

/* State Machine */
prte_list_t prte_job_states = {{0}};
prte_list_t prte_proc_states = {{0}};

/* a clean output channel without prefix */
int prte_clean_output = -1;

/* globals used by RTE */
bool prte_debug_daemons_file_flag = false;
bool prte_leave_session_attached = false;
bool prte_coprocessors_detected = false;
prte_hash_table_t *prte_coprocessors = NULL;
char *prte_topo_signature = NULL;
char *prte_data_server_uri = NULL;
char *prte_tool_basename = NULL;
bool prte_dvm_ready = false;
prte_pointer_array_t *prte_cache = NULL;
bool prte_persistent = true;
bool prte_add_pid_to_session_dirname = false;

/* PRTE OOB port flags */
bool prte_static_ports = false;
char *prte_oob_static_ports = NULL;

bool prte_keep_fqdn_hostnames = false;
bool prte_have_fqdn_allocation = false;
bool prte_show_resolved_nodenames = false;
bool prte_do_not_resolve = false;
int prte_hostname_cutoff = 1000;

int prted_debug_failure = -1;
int prted_debug_failure_delay = -1;
bool prte_never_launched = false;
bool prte_devel_level_output = false;
bool prte_display_topo_with_map = false;

char **prte_launch_environ = NULL;

bool prte_hnp_is_allocated = false;
bool prte_allocation_required = false;
bool prte_managed_allocation = false;
char *prte_set_slots = NULL;
bool prte_nidmap_communicated = false;
bool prte_node_info_communicated = false;

/* launch agents */
char *prte_launch_agent = NULL;
char **prted_cmd_line = NULL;
char **prte_fork_agent = NULL;

/* exit flags */
int prte_exit_status = 0;
bool prte_abnormal_term_ordered = false;
bool prte_routing_is_enabled = true;
bool prte_job_term_ordered = false;
bool prte_prteds_term_ordered = false;
bool prte_allowed_exit_without_sync = false;

int prte_timeout_usec_per_proc = -1;
float prte_max_timeout = -1.0;
prte_timer_t *prte_mpiexec_timeout = NULL;

int prte_stack_trace_wait_timeout = 30;

/* global arrays for data storage */
prte_pointer_array_t *prte_job_data = NULL;
prte_pointer_array_t *prte_node_pool = NULL;
prte_pointer_array_t *prte_node_topologies = NULL;
prte_pointer_array_t *prte_local_children = NULL;
pmix_rank_t prte_total_procs = 0;
char *prte_base_compute_node_sig = NULL;
bool prte_hetero_nodes = false;

/* IOF controls */
/* generate new xterm windows to display output from specified ranks */
char *prte_xterm = NULL;

/* report launch progress */
bool prte_report_launch_progress = false;

/* allocation specification */
char *prte_default_hostfile = NULL;
bool prte_default_hostfile_given = false;
int prte_num_allocated_nodes = 0;
char *prte_default_dash_host = NULL;

/* tool communication controls */
bool prte_report_events = false;
char *prte_report_events_uri = NULL;

/* report bindings */
bool prte_report_bindings = false;

/* barrier control */
bool prte_do_not_barrier = false;

/* process recovery */
bool prte_enable_recovery = false;
int32_t prte_max_restarts = 0;

/* exit status reporting */
bool prte_report_child_jobs_separately = false;
struct timeval prte_child_time_to_exit = {0};
bool prte_abort_non_zero_exit = false;

/* length of stat history to keep */
int prte_stat_history_size = -1;

/* envars to forward */
char **prte_forwarded_envars = NULL;

/* map stddiag output to stderr so it isn't forwarded to mpirun */
bool prte_map_stddiag_to_stderr = false;
bool prte_map_stddiag_to_stdout = false;

/* maximum size of virtual machine - used to subdivide allocation */
int prte_max_vm_size = -1;

int prte_debug_output = -1;
bool prte_debug_daemons_flag = false;
char *prte_job_ident = NULL;
bool prte_execute_quiet = false;
bool prte_report_silent_errors = false;
bool prte_hwloc_shmem_available = false;

/* See comment in src/tools/prun/debuggers.c about this MCA
   param */
bool prte_in_parallel_debugger = false;

char *prte_daemon_cores = NULL;

/* enable/disable ft */
bool prte_enable_ft = false;

int prte_dt_init(void)
{
    /* set default output */
    prte_debug_output = prte_output_open(NULL);

    /* open up the verbose output for PRTE debugging */
    if (prte_debug_flag || 0 < prte_debug_verbosity
        || (prte_debug_daemons_flag && (PRTE_PROC_IS_DAEMON || PRTE_PROC_IS_MASTER))) {
        if (0 < prte_debug_verbosity) {
            prte_output_set_verbosity(prte_debug_output, prte_debug_verbosity);
        } else {
            prte_output_set_verbosity(prte_debug_output, 1);
        }
    }

    return PRTE_SUCCESS;
}

prte_job_t *prte_get_job_data_object(const pmix_nspace_t job)
{
    prte_job_t *jptr;
    int i;

    /* if the job data wasn't setup, we cannot provide the data */
    if (NULL == prte_job_data) {
        return NULL;
    }
    /* if the nspace is invalid, then reject it */
    if (PMIX_NSPACE_INVALID(job)) {
        return NULL;
    }
    for (i = 0; i < prte_job_data->size; i++) {
        if (NULL == (jptr = (prte_job_t *) prte_pointer_array_get_item(prte_job_data, i))) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(jptr->nspace, job)) {
            return jptr;
        }
    }
    return NULL;
}

int prte_set_job_data_object(prte_job_t *jdata)
{
    prte_job_t *jptr;
    int i, save = -1;

    /* if the job data wasn't setup, we cannot set the data */
    if (NULL == prte_job_data) {
        return PRTE_ERROR;
    }
    /* if the nspace is invalid, then that's an error */
    if (PMIX_NSPACE_INVALID(jdata->nspace)) {
        return PRTE_ERROR;
    }
    /* verify that we don't already have this object */
    for (i = 0; i < prte_job_data->size; i++) {
        if (NULL == (jptr = (prte_job_t *) prte_pointer_array_get_item(prte_job_data, i))) {
            if (0 > save) {
                save = i;
            }
            continue;
        }
        if (PMIX_CHECK_NSPACE(jptr->nspace, jdata->nspace)) {
            return PRTE_EXISTS;
        }
    }

    if (-1 == save) {
        jdata->index = prte_pointer_array_add(prte_job_data, jdata);
    } else {
        jdata->index = save;
        prte_pointer_array_set_item(prte_job_data, save, jdata);
    }
    if (0 > jdata->index) {
        return PRTE_ERROR;
    }
    return PRTE_SUCCESS;
}

prte_proc_t *prte_get_proc_object(const pmix_proc_t *proc)
{
    prte_job_t *jdata;
    prte_proc_t *proct;

    if (NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        return NULL;
    }
    proct = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, proc->rank);
    return proct;
}

pmix_rank_t prte_get_proc_daemon_vpid(const pmix_proc_t *proc)
{
    prte_job_t *jdata;
    prte_proc_t *proct;

    if (NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        return PMIX_RANK_INVALID;
    }
    if (NULL == (proct = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, proc->rank))) {
        return PMIX_RANK_INVALID;
    }
    if (NULL == proct->node || NULL == proct->node->daemon) {
        return PMIX_RANK_INVALID;
    }
    return proct->node->daemon->name.rank;
}

char *prte_get_proc_hostname(const pmix_proc_t *proc)
{
    prte_proc_t *proct;

    /* don't bother error logging any not-found situations
     * as the layer above us will have something to say
     * about it */

    /* look it up on our arrays */
    if (NULL == (proct = prte_get_proc_object(proc))) {
        return NULL;
    }
    if (NULL == proct->node || NULL == proct->node->name) {
        return NULL;
    }
    return proct->node->name;
}

prte_node_rank_t prte_get_proc_node_rank(const pmix_proc_t *proc)
{
    prte_proc_t *proct;

    /* look it up on our arrays */
    if (NULL == (proct = prte_get_proc_object(proc))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_NODE_RANK_INVALID;
    }
    return proct->node_rank;
}

prte_node_t* prte_node_match(prte_list_t *nodes, const char *name)
{
    int m;
    prte_node_t *nptr;
    char *nm;

    /* does the name refer to me? */
    if (prte_check_host_is_local(name)) {
        nm = prte_process_info.nodename;
    } else {
        nm = (char*)name;
    }

    PRTE_LIST_FOREACH(nptr, nodes, prte_node_t) {
        /* start with the simple check */
        if (0 == strcmp(nptr->name, nm)) {
            return nptr;
        }
    }

    /* see if it is an alias for something already on the list */
    PRTE_LIST_FOREACH(nptr, nodes, prte_node_t) {
        if (NULL == nptr->aliases) {
            continue;
        }
        /* no choice but an exhaustive search - fortunately, these lists are short! */
        for (m = 0; NULL != nptr->aliases[m]; m++) {
            if (0 == strcmp(name, nptr->aliases[m])) {
                /* this is the node! */
                return nptr;
            }
        }
    }

    return NULL;
}

bool prte_nptr_match(prte_node_t *n1, prte_node_t *n2)
{
    size_t i, m;

    /* start with the simple check */
    if (0 == strcmp(n1->name, n2->name)) {
        return true;
    }

    if (NULL != n1->aliases) {
        for (i = 0; NULL != n1->aliases[i]; i++) {
            if (0 == strcmp(n1->aliases[i], n2->name)) {
                return true;
            }
            if (NULL != n2->aliases) {
                for (m = 0; NULL != n2->aliases[m]; m++) {
                    if (0 == strcmp(n2->aliases[m], n1->name)) {
                        return true;
                    }
                    if (0 == strcmp(n1->aliases[i], n2->aliases[m])) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/*
 * CONSTRUCTORS, DESTRUCTORS, AND CLASS INSTANTIATIONS
 * FOR PRTE CLASSES
 */

static void prte_app_context_construct(prte_app_context_t *app_context)
{
    app_context->idx = 0;
    app_context->app = NULL;
    app_context->num_procs = 0;
    PRTE_CONSTRUCT(&app_context->procs, prte_pointer_array_t);
    prte_pointer_array_init(&app_context->procs, 1, PRTE_GLOBAL_ARRAY_MAX_SIZE, 16);
    app_context->state = PRTE_APP_STATE_UNDEF;
    app_context->first_rank = 0;
    app_context->argv = NULL;
    app_context->env = NULL;
    app_context->cwd = NULL;
    app_context->flags = 0;
    PRTE_CONSTRUCT(&app_context->attributes, prte_list_t);
}

static void prte_app_context_destructor(prte_app_context_t *app_context)
{
    int i;
    prte_proc_t *proc;

    if (NULL != app_context->app) {
        free(app_context->app);
        app_context->app = NULL;
    }

    for (i = 0; i < app_context->procs.size; i++) {
        if (NULL != (proc = (prte_proc_t *) prte_pointer_array_get_item(&app_context->procs, i))) {
            PRTE_RELEASE(proc);
        }
    }
    PRTE_DESTRUCT(&app_context->procs);

    /* argv and env lists created by util/argv copy functions */
    if (NULL != app_context->argv) {
        prte_argv_free(app_context->argv);
        app_context->argv = NULL;
    }

    if (NULL != app_context->env) {
        prte_argv_free(app_context->env);
        app_context->env = NULL;
    }

    if (NULL != app_context->cwd) {
        free(app_context->cwd);
        app_context->cwd = NULL;
    }

    PRTE_LIST_DESTRUCT(&app_context->attributes);
}

PRTE_CLASS_INSTANCE(prte_app_context_t, prte_object_t, prte_app_context_construct,
                    prte_app_context_destructor);

static void prte_job_construct(prte_job_t *job)
{
    job->exit_code = 0;
    job->personality = NULL;
    PMIX_LOAD_NSPACE(job->nspace, NULL);
    job->index = -1;
    job->offset = 0;
    job->apps = PRTE_NEW(prte_pointer_array_t);
    prte_pointer_array_init(job->apps, 1, PRTE_GLOBAL_ARRAY_MAX_SIZE, 2);
    job->num_apps = 0;
    job->stdin_target = 0;
    job->total_slots_alloc = 0;
    job->num_procs = 0;
    job->procs = PRTE_NEW(prte_pointer_array_t);
    prte_pointer_array_init(job->procs, PRTE_GLOBAL_ARRAY_BLOCK_SIZE, PRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    job->map = NULL;
    job->bookmark = NULL;
    job->bkmark_obj = UINT_MAX; // mark that we haven't assigned a bkmark yet
    job->state = PRTE_JOB_STATE_UNDEF;

    job->num_mapped = 0;
    job->num_launched = 0;
    job->num_reported = 0;
    job->num_terminated = 0;
    job->num_daemons_reported = 0;
    job->num_ready_for_debug = 0;

    PMIX_LOAD_PROCID(&job->originator, NULL, PMIX_RANK_INVALID);
    job->num_local_procs = 0;

    job->flags = 0;
    PRTE_FLAG_SET(job, PRTE_JOB_FLAG_FORWARD_OUTPUT);

    PRTE_CONSTRUCT(&job->attributes, prte_list_t);
    PMIX_DATA_BUFFER_CONSTRUCT(&job->launch_msg);
    PRTE_CONSTRUCT(&job->children, prte_list_t);
    PMIX_LOAD_NSPACE(job->launcher, NULL);
    job->ntraces = 0;
    job->traces = NULL;
}

static void prte_job_destruct(prte_job_t *job)
{
    prte_proc_t *proc;
    prte_app_context_t *app;
    int n;
    prte_timer_t *evtimer;
    prte_job_t *child_jdata = NULL;

    if (NULL == job) {
        /* probably just a race condition - just return */
        return;
    }

    if (prte_debug_flag) {
        prte_output(0, "%s Releasing job data for %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    PRTE_JOBID_PRINT(job->nspace));
    }

    if (NULL != job->personality) {
        prte_argv_free(job->personality);
    }
    for (n = 0; n < job->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(job->apps, n))) {
            continue;
        }
        PRTE_RELEASE(app);
    }
    PRTE_RELEASE(job->apps);

    /* release any pointers in the attributes */
    evtimer = NULL;
    if (prte_get_attribute(&job->attributes, PRTE_JOB_TIMEOUT_EVENT, (void **) &evtimer, PMIX_POINTER)) {
        prte_event_evtimer_del(evtimer->ev);
        prte_remove_attribute(&job->attributes, PRTE_JOB_TIMEOUT_EVENT);
        /* the timer is a pointer to prte_timer_t */
        PRTE_RELEASE(evtimer);
    }
    evtimer = NULL;
    if (prte_get_attribute(&job->attributes, PRTE_SPAWN_TIMEOUT_EVENT, (void **) &evtimer, PMIX_POINTER)) {
        prte_event_evtimer_del(evtimer->ev);
        prte_remove_attribute(&job->attributes, PRTE_SPAWN_TIMEOUT_EVENT);
        /* the timer is a pointer to prte_timer_t */
        PRTE_RELEASE(evtimer);
    }
    proc = NULL;
    if (prte_get_attribute(&job->attributes, PRTE_JOB_ABORTED_PROC, (void **) &proc, PMIX_POINTER)) {
        prte_remove_attribute(&job->attributes, PRTE_JOB_ABORTED_PROC);
        /* points to an prte_proc_t */
        PRTE_RELEASE(proc);
    }

    if (NULL != job->map) {
        PRTE_RELEASE(job->map);
        job->map = NULL;
    }

    for (n = 0; n < job->procs->size; n++) {
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(job->procs, n))) {
            continue;
        }
        PRTE_RELEASE(proc);
    }
    PRTE_RELEASE(job->procs);

    /* release the attributes */
    PRTE_LIST_DESTRUCT(&job->attributes);

    PMIX_DATA_BUFFER_DESTRUCT(&job->launch_msg);

    /* Clear the child list before destroying the list */
    PRTE_LIST_FOREACH(child_jdata, &job->children, prte_job_t)
    {
        prte_list_remove_item(&job->children, &child_jdata->super);
    }

    PRTE_LIST_DESTRUCT(&job->children);

    if (NULL != prte_job_data && 0 <= job->index) {
        /* remove the job from the global array */
        prte_pointer_array_set_item(prte_job_data, job->index, NULL);
    }
    if (NULL != job->traces) {
        prte_argv_free(job->traces);
    }
}

PRTE_CLASS_INSTANCE(prte_job_t,
                    prte_list_item_t,
                    prte_job_construct,
                    prte_job_destruct);

static void prte_node_construct(prte_node_t *node)
{
    node->index = -1;
    node->name = NULL;
    node->rawname = NULL;
    node->aliases = NULL;
    node->daemon = NULL;

    node->num_procs = 0;
    node->procs = PRTE_NEW(prte_pointer_array_t);
    prte_pointer_array_init(node->procs, PRTE_GLOBAL_ARRAY_BLOCK_SIZE, PRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    node->next_node_rank = 0;

    node->state = PRTE_NODE_STATE_UNKNOWN;
    node->slots = 0;
    node->slots_available = 0;
    node->slots_inuse = 0;
    node->slots_max = 0;
    node->topology = NULL;

    node->flags = 0;
    PRTE_CONSTRUCT(&node->attributes, prte_list_t);
}

static void prte_node_destruct(prte_node_t *node)
{
    int i;
    prte_proc_t *proc;

    if (NULL != node->name) {
        free(node->name);
        node->name = NULL;
    }
    if (NULL != node->rawname) {
        free(node->rawname);
        node->rawname = NULL;
    }
    if (NULL != node->aliases) {
        prte_argv_free(node->aliases);
        node->aliases = NULL;
    }
    if (NULL != node->daemon) {
        node->daemon->node = NULL;
        PRTE_RELEASE(node->daemon);
        node->daemon = NULL;
    }

    for (i = 0; i < node->procs->size; i++) {
        if (NULL != (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, i))) {
            prte_pointer_array_set_item(node->procs, i, NULL);
            PRTE_RELEASE(proc);
        }
    }
    PRTE_RELEASE(node->procs);

    /* do NOT destroy the topology */

    /* release the attributes */
    PRTE_LIST_DESTRUCT(&node->attributes);
}

PRTE_CLASS_INSTANCE(prte_node_t, prte_list_item_t, prte_node_construct, prte_node_destruct);

static void prte_proc_construct(prte_proc_t *proc)
{
    proc->name = *PRTE_NAME_INVALID;
    proc->job = NULL;
    proc->rank = PMIX_RANK_INVALID;
    proc->parent = PMIX_RANK_INVALID;
    proc->pid = 0;
    proc->local_rank = PRTE_LOCAL_RANK_INVALID;
    proc->node_rank = PRTE_NODE_RANK_INVALID;
    proc->numa_rank = PRTE_LOCAL_RANK_INVALID;
    proc->app_rank = -1;
    proc->last_errmgr_state = PRTE_PROC_STATE_UNDEF;
    proc->state = PRTE_PROC_STATE_UNDEF;
    proc->app_idx = 0;
    proc->node = NULL;
    proc->exit_code = 0; /* Assume we won't fail unless otherwise notified */
    proc->rml_uri = NULL;
    proc->flags = 0;
    PRTE_CONSTRUCT(&proc->attributes, prte_list_t);
}

static void prte_proc_destruct(prte_proc_t *proc)
{
    if (NULL != proc->node) {
        PRTE_RELEASE(proc->node);
        proc->node = NULL;
    }

    if (NULL != proc->rml_uri) {
        free(proc->rml_uri);
        proc->rml_uri = NULL;
    }

    PRTE_LIST_DESTRUCT(&proc->attributes);
}

PRTE_CLASS_INSTANCE(prte_proc_t, prte_list_item_t, prte_proc_construct, prte_proc_destruct);

static void prte_job_map_construct(prte_job_map_t *map)
{
    map->req_mapper = NULL;
    map->last_mapper = NULL;
    map->mapping = 0;
    map->ranking = 0;
    map->binding = 0;
    map->num_new_daemons = 0;
    map->daemon_vpid_start = PMIX_RANK_INVALID;
    map->num_nodes = 0;
    map->nodes = PRTE_NEW(prte_pointer_array_t);
    prte_pointer_array_init(map->nodes, PRTE_GLOBAL_ARRAY_BLOCK_SIZE, PRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
}

static void prte_job_map_destruct(prte_job_map_t *map)
{
    int32_t i;
    prte_node_t *node;

    if (NULL != map->req_mapper) {
        free(map->req_mapper);
    }
    if (NULL != map->last_mapper) {
        free(map->last_mapper);
    }
    for (i = 0; i < map->nodes->size; i++) {
        if (NULL != (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, i))) {
            PRTE_RELEASE(node);
            prte_pointer_array_set_item(map->nodes, i, NULL);
        }
    }
    PRTE_RELEASE(map->nodes);
}

PRTE_CLASS_INSTANCE(prte_job_map_t, prte_object_t, prte_job_map_construct, prte_job_map_destruct);

static void prte_attr_cons(prte_attribute_t *p)
{
    p->key = 0;
    p->local = true; // default to local-only data
    memset(&p->data, 0, sizeof(p->data));
}
static void prte_attr_des(prte_attribute_t *p)
{
    PMIX_VALUE_DESTRUCT(&p->data);
}
PRTE_CLASS_INSTANCE(prte_attribute_t, prte_list_item_t, prte_attr_cons, prte_attr_des);

static void tcon(prte_topology_t *t)
{
    t->topo = NULL;
    t->sig = NULL;
}
static void tdes(prte_topology_t *t)
{
    if (NULL != t->topo) {
        prte_hwloc_base_free_topology(t->topo);
    }
    if (NULL != t->sig) {
        free(t->sig);
    }
}
PRTE_CLASS_INSTANCE(prte_topology_t, prte_object_t, tcon, tdes);

#if PRTE_PICKY_COMPILERS
void prte_hide_unused_params(int x, ...)
{
    va_list ap;

    va_start(ap, x);
    va_end(ap);
}
#endif
