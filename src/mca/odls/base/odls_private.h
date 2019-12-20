/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_ODLS_PRIVATE_H
#define MCA_ODLS_PRIVATE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "types.h"

#include "src/class/prrte_list.h"
#include "src/class/prrte_pointer_array.h"
#include "src/class/prrte_bitmap.h"
#include "src/dss/dss_types.h"

#include "src/mca/iof/base/iof_base_setup.h"
#include "src/mca/rml/rml_types.h"
#include "src/runtime/prrte_globals.h"
#include "src/threads/threads.h"
#include "src/mca/odls/odls_types.h"

BEGIN_C_DECLS

/*
 * General ODLS types
 */

typedef struct {
    /** Verbose/debug output stream */
    int output;
    /** Time to allow process to forcibly die */
    int timeout_before_sigkill;
    /* list of ranks to be displayed on separate xterms */
    prrte_list_t xterm_ranks;
    /* the xterm cmd to be used */
    char **xtermcmd;
    /* thread pool */
    int max_threads;
    int num_threads;
    int cutoff;
    prrte_event_base_t **ev_bases;   // event base array for progress threads
    char** ev_threads;              // event progress thread names
    int next_base;                  // counter to load-level thread use
    bool signal_direct_children_only;
    prrte_lock_t lock;
} prrte_odls_globals_t;

PRRTE_EXPORT extern prrte_odls_globals_t prrte_odls_globals;

/*
 * Default functions that are common to most environments - can
 * be overridden by specific environments if they need something
 * different (e.g., bproc)
 */
PRRTE_EXPORT int
prrte_odls_base_default_get_add_procs_data(prrte_buffer_t *data,
                                          prrte_jobid_t job);

PRRTE_EXPORT int
prrte_odls_base_default_construct_child_list(prrte_buffer_t *data,
                                            prrte_jobid_t *job);

PRRTE_EXPORT void prrte_odls_base_spawn_proc(int fd, short sd, void *cbdata);

/* define a function that will fork a local proc */
typedef int (*prrte_odls_base_fork_local_proc_fn_t)(void *cd);

/* define an object for fork/exec the local proc */
typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    char *cmd;
    char *wdir;
    char **argv;
    char **env;
    prrte_job_t *jdata;
    prrte_app_context_t *app;
    prrte_proc_t *child;
    bool index_argv;
    prrte_iof_base_io_conf_t opts;
    prrte_odls_base_fork_local_proc_fn_t fork_local;
} prrte_odls_spawn_caddy_t;
PRRTE_CLASS_DECLARATION(prrte_odls_spawn_caddy_t);

/* define an object for starting local launch */
typedef struct {
    prrte_object_t object;
    prrte_event_t *ev;
    prrte_jobid_t job;
    prrte_odls_base_fork_local_proc_fn_t fork_local;
    int retries;
} prrte_odls_launch_local_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_odls_launch_local_t);

#define PRRTE_ACTIVATE_LOCAL_LAUNCH(j, f)                                \
    do {                                                                \
        prrte_odls_launch_local_t *ll;                                   \
        ll = PRRTE_NEW(prrte_odls_launch_local_t);                         \
        ll->job = (j);                                                  \
        ll->fork_local = (f);                                           \
        prrte_event_set(prrte_event_base, ll->ev, -1, PRRTE_EV_WRITE,      \
                       prrte_odls_base_default_launch_local, ll);        \
        prrte_event_set_priority(ll->ev, PRRTE_SYS_PRI);                  \
        prrte_event_active(ll->ev, PRRTE_EV_WRITE, 1);                    \
    } while(0);

PRRTE_EXPORT void prrte_odls_base_default_launch_local(int fd, short sd, void *cbdata);

PRRTE_EXPORT void prrte_odls_base_default_wait_local_proc(int fd, short sd, void *cbdata);

/* define a function type to signal a local proc */
typedef int (*prrte_odls_base_signal_local_fn_t)(pid_t pid, int signum);

PRRTE_EXPORT int
prrte_odls_base_default_signal_local_procs(const prrte_process_name_t *proc, int32_t signal,
                                          prrte_odls_base_signal_local_fn_t signal_local);

/* define a function type for killing a local proc */
typedef int (*prrte_odls_base_kill_local_fn_t)(pid_t pid, int signum);

/* define a function type to detect that a child died */
typedef bool (*prrte_odls_base_child_died_fn_t)(prrte_proc_t *child);

PRRTE_EXPORT int
prrte_odls_base_default_kill_local_procs(prrte_pointer_array_t *procs,
                                        prrte_odls_base_kill_local_fn_t kill_local);

PRRTE_EXPORT int prrte_odls_base_default_restart_proc(prrte_proc_t *child,
                                                      prrte_odls_base_fork_local_proc_fn_t fork_local);

/*
 * Preload binary/files functions
 */
PRRTE_EXPORT int prrte_odls_base_preload_files_app_context(prrte_app_context_t* context);

/*
 * Obtain process stats on a child proc
 */
PRRTE_EXPORT int prrte_odls_base_get_proc_stats(prrte_buffer_t *answer, prrte_process_name_t *proc);

END_C_DECLS

#endif
