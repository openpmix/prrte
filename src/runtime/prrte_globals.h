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
 * Copyright (c) 2007-2010 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2007-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Global params for PRRTE
 */
#ifndef PRRTE_RUNTIME_PRRTE_GLOBALS_H
#define PRRTE_RUNTIME_PRRTE_GLOBALS_H

#include "prrte_config.h"
#include "types.h"

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "src/class/prrte_hash_table.h"
#include "src/class/prrte_pointer_array.h"
#include "src/class/prrte_value_array.h"
#include "src/class/prrte_ring_buffer.h"
#include "src/threads/threads.h"
#include "src/event/event-internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/plm/plm_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/util/attr.h"
#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/util/error_strings.h"
#include "src/runtime/runtime.h"


BEGIN_C_DECLS

PRRTE_EXPORT extern int prrte_debug_verbosity;  /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern char *prrte_prohibited_session_dirs;  /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern bool prrte_xml_output;  /* instantiated in src/runtime/prrte_globals.c */
PRRTE_EXPORT extern FILE *prrte_xml_fp;   /* instantiated in src/runtime/prrte_globals.c */
PRRTE_EXPORT extern bool prrte_help_want_aggregate;  /* instantiated in src/util/show_help.c */
PRRTE_EXPORT extern char *prrte_job_ident;  /* instantiated in src/runtime/prrte_globals.c */
PRRTE_EXPORT extern bool prrte_create_session_dirs;  /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern bool prrte_execute_quiet;  /* instantiated in src/runtime/prrte_globals.c */
PRRTE_EXPORT extern bool prrte_report_silent_errors;  /* instantiated in src/runtime/prrte_globals.c */
PRRTE_EXPORT extern prrte_event_base_t *prrte_event_base;  /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern bool prrte_event_base_active; /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern bool prrte_proc_is_bound;  /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern int prrte_progress_thread_debug;  /* instantiated in src/runtime/prrte_init.c */
PRRTE_EXPORT extern char *prrte_tool_basename;   // argv[0] of prun or one of its symlinks
/**
 * Global indicating where this process was bound to at launch (will
 * be NULL if !prrte_proc_is_bound)
 */
PRRTE_EXPORT extern hwloc_cpuset_t prrte_proc_applied_binding;  /* instantiated in src/runtime/prrte_init.c */


/* Shortcut for some commonly used names */
#define PRRTE_NAME_WILDCARD      (&prrte_name_wildcard)
PRRTE_EXPORT extern prrte_process_name_t prrte_name_wildcard;  /** instantiated in src/runtime/prrte_init.c */
#define PRRTE_NAME_INVALID       (&prrte_name_invalid)
PRRTE_EXPORT extern prrte_process_name_t prrte_name_invalid;  /** instantiated in src/runtime/prrte_init.c */

#define PRRTE_PROC_MY_NAME       (&prrte_process_info.my_name)

/* define a special name that point to my parent (aka the process that spawned me) */
#define PRRTE_PROC_MY_PARENT     (&prrte_process_info.my_parent)

/* define a special name that belongs to prte master */
#define PRRTE_PROC_MY_HNP        (&prrte_process_info.my_hnp)

PRRTE_EXPORT extern bool prrte_in_parallel_debugger;

/* error manager callback function */
typedef void (*prrte_err_cb_fn_t)(prrte_process_name_t *proc, prrte_proc_state_t state, void *cbdata);

/* define an object for timer events */
typedef struct {
    prrte_object_t super;
    struct timeval tv;
    prrte_event_t *ev;
    void *payload;
} prrte_timer_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_timer_t);

PRRTE_EXPORT extern int prrte_exit_status;

/* PRRTE event priorities - we define these
 * at levels that permit higher layers such as
 * OMPI to handle their events at higher priority,
 * with the exception of errors. Errors generally
 * require exception handling (e.g., ctrl-c termination)
 * that overrides the need to process MPI messages
 */
#define PRRTE_ERROR_PRI  PRRTE_EV_ERROR_PRI
#define PRRTE_MSG_PRI    PRRTE_EV_MSG_LO_PRI
#define PRRTE_SYS_PRI    PRRTE_EV_SYS_LO_PRI
#define PRRTE_INFO_PRI   PRRTE_EV_INFO_LO_PRI

/* define some common keys used in PRRTE */
#define PRRTE_DB_DAEMON_VPID  "prrte.daemon.vpid"

/* State Machine lists */
PRRTE_EXPORT extern prrte_list_t prrte_job_states;
PRRTE_EXPORT extern prrte_list_t prrte_proc_states;

/* a clean output channel without prefix */
PRRTE_EXPORT extern int prrte_clean_output;

#define PRRTE_GLOBAL_ARRAY_BLOCK_SIZE    64
#define PRRTE_GLOBAL_ARRAY_MAX_SIZE      INT_MAX

/* define a default error return code for PRRTE */
#define PRRTE_ERROR_DEFAULT_EXIT_CODE    1

/**
 * Define a macro for updating the prrte_exit_status
 * The macro provides a convenient way of doing this
 * so that we can add thread locking at some point
 * since the prrte_exit_status is a global variable.
 *
 * Ensure that we do not overwrite the exit status if it has
 * already been set to some non-zero value. If we don't make
 * this check, then different parts of the code could overwrite
 * each other's exit status in the case of abnormal termination.
 *
 * For example, if a process aborts, we would record the initial
 * exit code from the aborted process. However, subsequent processes
 * will have been aborted by signal as we kill the job. We don't want
 * the subsequent processes to overwrite the original exit code so
 * we can tell the user the exit code from the process that caused
 * the whole thing to happen.
 */
#define PRRTE_UPDATE_EXIT_STATUS(newstatus)                                  \
    do {                                                                    \
        if (0 == prrte_exit_status && 0 != newstatus) {                      \
            PRRTE_OUTPUT_VERBOSE((1, prrte_debug_output,                      \
                                 "%s:%s(%d) updating exit status to %d",    \
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),        \
                                 __FILE__, __LINE__, newstatus));           \
            prrte_exit_status = newstatus;                                   \
        }                                                                   \
    } while(0);

/* sometimes we need to reset the exit status - for example, when we
 * are restarting a failed process
 */
#define PRRTE_RESET_EXIT_STATUS()                                \
    do {                                                        \
        PRRTE_OUTPUT_VERBOSE((1, prrte_debug_output,              \
                            "%s:%s(%d) reseting exit status",   \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), \
                            __FILE__, __LINE__));               \
        prrte_exit_status = 0;                                   \
    } while(0);


/* define a set of flags to control the launch of a job */
typedef uint16_t prrte_job_controls_t;
#define PRRTE_JOB_CONTROL    PRRTE_UINT16


/* global type definitions used by RTE - instanced in prrte_globals.c */

/************
* Declare this to allow us to use it before fully
* defining it - resolves potential circular definition
*/
struct prrte_proc_t;
struct prrte_job_map_t;
/************/

/* define an object for storing node topologies */
typedef struct {
    prrte_object_t super;
    int index;
    hwloc_topology_t topo;
    char *sig;
} prrte_topology_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_topology_t);


/**
* Information about a specific application to be launched in the RTE.
 */
typedef struct {
    /** Parent object */
    prrte_object_t super;
    /** Unique index when multiple apps per job */
    prrte_app_idx_t idx;
    /** Absolute pathname of argv[0] */
    char   *app;
    /** Number of copies of this process that are to be launched */
    prrte_std_cntr_t num_procs;
    /** Array of pointers to the proc objects for procs of this app_context
     * NOTE - not always used
     */
    prrte_pointer_array_t procs;
    /** State of the app_context */
    prrte_app_state_t state;
    /** First MPI rank of this app_context in the job */
    prrte_vpid_t first_rank;
    /** Standard argv-style array, including a final NULL pointer */
    char  **argv;
    /** Standard environ-style array, including a final NULL pointer */
    char  **env;
    /** Current working directory for this app */
    char   *cwd;
    /* flags */
    prrte_app_context_flags_t flags;
    /* provide a list of attributes for this app_context in place
     * of having a continually-expanding list of fixed-use values.
     * This is a list of prrte_value_t's, with the intent of providing
     * flexibility without constantly expanding the memory footprint
     * every time we want some new (rarely used) option
     */
    prrte_list_t attributes;
} prrte_app_context_t;

PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_app_context_t);


typedef struct {
    /** Base object so this can be put on a list */
    prrte_list_item_t super;
    /* index of this node object in global array */
    prrte_std_cntr_t index;
    /** String node name */
    char *name;
    /* daemon on this node */
    struct prrte_proc_t *daemon;
    /** number of procs on this node */
    prrte_vpid_t num_procs;
    /* array of pointers to procs on this node */
    prrte_pointer_array_t *procs;
    /* next node rank on this node */
    prrte_node_rank_t next_node_rank;
    /** State of this node */
    prrte_node_state_t state;
    /** A "soft" limit on the number of slots available on the node.
        This will typically correspond to the number of physical CPUs
        that we have been allocated on this note and would be the
        "ideal" number of processes for us to launch. */
    prrte_std_cntr_t slots;
    /** How many processes have already been launched, used by one or
        more jobs on this node. */
    prrte_std_cntr_t slots_inuse;
    /** A "hard" limit (if set -- a value of 0 implies no hard limit)
        on the number of slots that can be allocated on a given
        node. This is for some environments (e.g. grid) there may be
        fixed limits on the number of slots that can be used.

        This value also could have been a boolean - but we may want to
        allow the hard limit be different than the soft limit - in
        other words allow the node to be oversubscribed up to a
        specified limit.  For example, if we have two processors, we
        may want to allow up to four processes but no more. */
    prrte_std_cntr_t slots_max;
    /* system topology for this node */
    prrte_topology_t *topology;
    /* flags */
    prrte_node_flags_t flags;
    /* list of prrte_attribute_t */
    prrte_list_t attributes;
} prrte_node_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_node_t);

typedef struct {
    /** Base object so this can be put on a list */
    prrte_list_item_t super;
    /* record the exit status for this job */
    int exit_code;
    /* personality for this job */
    char **personality;
    /* jobid for this job */
    prrte_jobid_t jobid;
    pmix_nspace_t nspace;
    /* offset to the total number of procs so shared memory
     * components can potentially connect to any spawned jobs*/
    prrte_vpid_t offset;
    /* app_context array for this job */
    prrte_pointer_array_t *apps;
    /* number of app_contexts in the array */
    prrte_app_idx_t num_apps;
    /* rank desiring stdin - for now, either one rank, all ranks
     * (wildcard), or none (invalid)
     */
    prrte_vpid_t stdin_target;
    /* total slots allocated to this job */
    prrte_std_cntr_t total_slots_alloc;
    /* number of procs in this job */
    prrte_vpid_t num_procs;
    /* array of pointers to procs in this job */
    prrte_pointer_array_t *procs;
    /* map of the job */
    struct prrte_job_map_t *map;
    /* bookmark for where we are in mapping - this
     * indicates the node where we stopped
     */
    prrte_node_t *bookmark;
    /* if we are binding, bookmark the index of the
     * last object we bound to */
    unsigned int bkmark_obj;
    /* state of the overall job */
    prrte_job_state_t state;
    /* number of procs mapped */
    prrte_vpid_t num_mapped;
    /* number of procs launched */
    prrte_vpid_t num_launched;
    /* number of procs reporting contact info */
    prrte_vpid_t num_reported;
    /* number of procs terminated */
    prrte_vpid_t num_terminated;
    /* number of daemons reported launched so we can track progress */
    prrte_vpid_t num_daemons_reported;
    /* originator of a dynamic spawn */
    prrte_process_name_t originator;
    /* number of local procs */
    prrte_vpid_t num_local_procs;
    /* flags */
    prrte_job_flags_t flags;
    /* attributes */
    prrte_list_t attributes;
    /* launch msg buffer */
    prrte_buffer_t launch_msg;
    /* track children of this job */
    prrte_list_t children;
    /* track the launcher of these jobs */
    prrte_jobid_t launcher;
} prrte_job_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_job_t);

struct prrte_proc_t {
    /** Base object so this can be put on a list */
    prrte_list_item_t super;
    /* process name */
    prrte_process_name_t name;
    prrte_job_t *job;
    pmix_rank_t rank;
    /* the vpid of my parent - the daemon vpid for an app
     * or the vpid of the parent in the routing tree of
     * a daemon */
    prrte_vpid_t parent;
    /* pid */
    pid_t pid;
    /* local rank amongst my peers on the node
     * where this is running - this value is
     * needed by MPI procs so that the lowest
     * rank on a node can perform certain fns -
     * e.g., open an sm backing file
     */
    prrte_local_rank_t local_rank;
    /* local rank on the node across all procs
     * and jobs known to this HNP - this is
     * needed so that procs can do things like
     * know which static IP port to use
     */
    prrte_node_rank_t node_rank;
    /* rank of this proc within its app context - this
     * will just equal its vpid for single app_context
     * applications
     */
    int32_t app_rank;
    /* Last state used to trigger the errmgr for this proc */
    prrte_proc_state_t last_errmgr_state;
    /* process state */
    prrte_proc_state_t state;
    /* exit code */
    prrte_exit_code_t exit_code;
    /* the app_context that generated this proc */
    prrte_app_idx_t app_idx;
    /* pointer to the node where this proc is executing */
    prrte_node_t *node;
    /* RML contact info */
    char *rml_uri;
    /* some boolean flags */
    prrte_proc_flags_t flags;
    /* list of prrte_value_t attributes */
    prrte_list_t attributes;
};
typedef struct prrte_proc_t prrte_proc_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_proc_t);

/**
 * Get a job data object
 * We cannot just reference a job data object with its jobid as
 * the jobid is no longer an index into the array. This change
 * was necessitated by modification of the jobid to include
 * an mpirun-unique qualifer to eliminate any global name
 * service
 */
PRRTE_EXPORT   prrte_job_t* prrte_get_job_data_object(prrte_jobid_t job);

/**
 * Get a proc data object
 */
PRRTE_EXPORT prrte_proc_t* prrte_get_proc_object(prrte_process_name_t *proc);

/**
 * Get the daemon vpid hosting a given proc
 */
PRRTE_EXPORT prrte_vpid_t prrte_get_proc_daemon_vpid(prrte_process_name_t *proc);

/* Get the hostname of a proc */
PRRTE_EXPORT char* prrte_get_proc_hostname(prrte_process_name_t *proc);

/* get the node rank of a proc */
PRRTE_EXPORT prrte_node_rank_t prrte_get_proc_node_rank(prrte_process_name_t *proc);

/* check to see if two nodes match */
PRRTE_EXPORT bool prrte_node_match(prrte_node_t *n1, char *name);

/* global variables used by RTE - instanced in prrte_globals.c */
PRRTE_EXPORT extern bool prrte_debug_daemons_flag;
PRRTE_EXPORT extern bool prrte_debug_daemons_file_flag;
PRRTE_EXPORT extern bool prrte_leave_session_attached;
PRRTE_EXPORT extern bool prrte_do_not_launch;
PRRTE_EXPORT extern bool prrte_coprocessors_detected;
PRRTE_EXPORT extern prrte_hash_table_t *prrte_coprocessors;
PRRTE_EXPORT extern char *prrte_topo_signature;
PRRTE_EXPORT extern char *prrte_data_server_uri;
PRRTE_EXPORT extern bool prrte_dvm_ready;
PRRTE_EXPORT extern prrte_pointer_array_t *prrte_cache;

/* PRRTE OOB port flags */
PRRTE_EXPORT extern bool prrte_static_ports;
PRRTE_EXPORT extern char *prrte_oob_static_ports;

/* nodename flags */
PRRTE_EXPORT extern bool prrte_keep_fqdn_hostnames;
PRRTE_EXPORT extern bool prrte_have_fqdn_allocation;
PRRTE_EXPORT extern bool prrte_show_resolved_nodenames;
PRRTE_EXPORT extern int prrte_use_hostname_alias;
PRRTE_EXPORT extern int prrte_hostname_cutoff;

/* debug flags */
PRRTE_EXPORT extern int prted_debug_failure;
PRRTE_EXPORT extern int prted_debug_failure_delay;

PRRTE_EXPORT extern bool prrte_never_launched;
PRRTE_EXPORT extern bool prrte_devel_level_output;
PRRTE_EXPORT extern bool prrte_display_topo_with_map;
PRRTE_EXPORT extern bool prrte_display_diffable_output;

PRRTE_EXPORT extern char **prrte_launch_environ;

PRRTE_EXPORT extern bool prrte_hnp_is_allocated;
PRRTE_EXPORT extern bool prrte_allocation_required;
PRRTE_EXPORT extern bool prrte_managed_allocation;
PRRTE_EXPORT extern char *prrte_set_slots;
PRRTE_EXPORT extern bool prrte_soft_locations;
PRRTE_EXPORT extern bool prrte_hnp_connected;
PRRTE_EXPORT extern bool prrte_nidmap_communicated;
PRRTE_EXPORT extern bool prrte_node_info_communicated;

/* launch agents */
PRRTE_EXPORT extern char *prrte_launch_agent;
PRRTE_EXPORT extern char **prted_cmd_line;
PRRTE_EXPORT extern char **prrte_fork_agent;

/* exit flags */
PRRTE_EXPORT extern bool prrte_abnormal_term_ordered;
PRRTE_EXPORT extern bool prrte_routing_is_enabled;
PRRTE_EXPORT extern bool prrte_job_term_ordered;
PRRTE_EXPORT extern bool prrte_prteds_term_ordered;
PRRTE_EXPORT extern bool prrte_allowed_exit_without_sync;
PRRTE_EXPORT extern int prrte_startup_timeout;

PRRTE_EXPORT extern int prrte_timeout_usec_per_proc;
PRRTE_EXPORT extern float prrte_max_timeout;
PRRTE_EXPORT extern prrte_timer_t *prrte_mpiexec_timeout;

/* global arrays for data storage */
PRRTE_EXPORT extern prrte_hash_table_t *prrte_job_data;
PRRTE_EXPORT extern prrte_pointer_array_t *prrte_node_pool;
PRRTE_EXPORT extern prrte_pointer_array_t *prrte_node_topologies;
PRRTE_EXPORT extern prrte_pointer_array_t *prrte_local_children;
PRRTE_EXPORT extern prrte_vpid_t prrte_total_procs;

/* IOF controls */
PRRTE_EXPORT extern bool prrte_tag_output;
PRRTE_EXPORT extern bool prrte_timestamp_output;
/* generate new xterm windows to display output from specified ranks */
PRRTE_EXPORT extern char *prrte_xterm;

/* whether or not to report launch progress */
PRRTE_EXPORT extern bool prrte_report_launch_progress;

/* allocation specification */
PRRTE_EXPORT extern char *prrte_default_hostfile;
PRRTE_EXPORT extern bool prrte_default_hostfile_given;
PRRTE_EXPORT extern char *prrte_rankfile;
PRRTE_EXPORT extern int prrte_num_allocated_nodes;
PRRTE_EXPORT extern char *prrte_default_dash_host;

/* tool communication controls */
PRRTE_EXPORT extern bool prrte_report_events;
PRRTE_EXPORT extern char *prrte_report_events_uri;

/* process recovery */
PRRTE_EXPORT extern bool prrte_enable_recovery;
PRRTE_EXPORT extern int32_t prrte_max_restarts;
/* barrier control */
PRRTE_EXPORT extern bool prrte_do_not_barrier;

/* exit status reporting */
PRRTE_EXPORT extern bool prrte_report_child_jobs_separately;
PRRTE_EXPORT extern struct timeval prrte_child_time_to_exit;
PRRTE_EXPORT extern bool prrte_abort_non_zero_exit;

/* length of stat history to keep */
PRRTE_EXPORT extern int prrte_stat_history_size;

/* envars to forward */
PRRTE_EXPORT extern char **prrte_forwarded_envars;

/* map stddiag output to stderr so it isn't forwarded to mpirun */
PRRTE_EXPORT extern bool prrte_map_stddiag_to_stderr;
PRRTE_EXPORT extern bool prrte_map_stddiag_to_stdout;

/* maximum size of virtual machine - used to subdivide allocation */
PRRTE_EXPORT extern int prrte_max_vm_size;

/* binding directives for daemons to restrict them
 * to certain cores
 */
PRRTE_EXPORT extern char *prrte_daemon_cores;

/* Max time to wait for stack straces to return */
PRRTE_EXPORT extern int prrte_stack_trace_wait_timeout;

/* whether or not hwloc shmem support is available */
PRRTE_EXPORT extern bool prrte_hwloc_shmem_available;

extern char *prrte_signal_string;
extern char *prrte_stacktrace_output_filename;
extern char *prrte_net_private_ipv4;
extern char *prrte_set_max_sys_limits;

END_C_DECLS

#endif /* PRRTE_RUNTIME_PRRTE_GLOBALS_H */
