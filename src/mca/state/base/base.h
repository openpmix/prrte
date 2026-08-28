/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_MCA_STATE_BASE_H
#define PRTE_MCA_STATE_BASE_H

/*
 * includes
 */
#include "prte_config.h"
#include "constants.h"

#include <stdio.h>

#include "src/class/pmix_list.h"
#include "src/util/pmix_printf.h"

#include "src/mca/mca.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/state.h"

BEGIN_C_DECLS

/**
 * Struct to hold data global to the state framework
 */
typedef struct {
    int parent_fd;
    bool ready_msg;
    bool run_fdcheck;
    bool recoverable;
    int max_restarts;
    bool continuous;
    bool error_non_zero_exit;
    bool show_launch_progress;
    bool notifyerrors;
    bool autorestart;
    /* DVM state logging - see state_base_log.c.  log_path is owned by the
     * MCA variable system; log_file and log_fp are ours. */
    bool log_jobstate;
    bool log_procstate;
    char *log_path;
    char *log_file;
    FILE *log_fp;
} prte_state_base_t;
PRTE_EXPORT extern prte_state_base_t prte_state_base;

/* select a component */
PRTE_EXPORT int prte_state_base_select(void);

/*
 * DVM state logging (state_base_log.c)
 */
PRTE_EXPORT int prte_state_base_log_resolve_dir(const char *path, const char *base, char **dir);
PRTE_EXPORT void prte_state_base_log_open(void);
PRTE_EXPORT void prte_state_base_log_close(void);
PRTE_EXPORT void prte_state_base_log_job(prte_job_t *jdata, prte_job_state_t state);
PRTE_EXPORT void prte_state_base_log_proc(const pmix_proc_t *proc, prte_proc_state_t state);

/* debug tools */
PRTE_EXPORT void prte_state_base_print_job_state_machine(void);

PRTE_EXPORT void prte_state_base_print_proc_state_machine(void);

PRTE_EXPORT int prte_state_base_set_runtime_options(prte_job_t *jdata, char *spec);
PRTE_EXPORT bool prte_state_base_report_child_sep(const char *spec);

/*
 * Base functions
 */
PRTE_EXPORT void prte_state_base_activate_job_state(prte_job_t *jdata, prte_job_state_t state);

PRTE_EXPORT int prte_state_base_add_job_state(prte_job_state_t state, prte_state_cbfunc_t cbfunc);

PRTE_EXPORT int prte_state_base_set_job_state_callback(prte_job_state_t state,
                                                       prte_state_cbfunc_t cbfunc);

PRTE_EXPORT int prte_state_base_remove_job_state(prte_job_state_t state);

PRTE_EXPORT void prte_state_base_activate_proc_state(pmix_proc_t *proc, prte_proc_state_t state);

PRTE_EXPORT int prte_state_base_add_proc_state(prte_proc_state_t state, prte_state_cbfunc_t cbfunc);

PRTE_EXPORT int prte_state_base_set_proc_state_callback(prte_proc_state_t state,
                                                        prte_state_cbfunc_t cbfunc);

PRTE_EXPORT int prte_state_base_remove_proc_state(prte_proc_state_t state);

/* common state processing functions */
PRTE_EXPORT void prte_state_base_local_launch_complete(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_report_progress(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_track_procs(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_check_fds(prte_job_t *jdata);
PRTE_EXPORT void prte_state_base_notify_data_server(pmix_proc_t *target);

/* Purge only OUR OWN store - the one holding this daemon's local-range
 * data - without touching the DVM's global store.  What a daemon knows
 * when its share of a job is done is that its own procs of that job have
 * gone; the job itself may still be running on other nodes, and the
 * global store is not its to act on. */
PRTE_EXPORT void prte_state_base_notify_local_data_server(pmix_proc_t *target);

/* A proc reported a state and we hold no job object to account it against.
 * Both track_procs implementations call this instead of dropping the
 * report on the floor - see the comment on the definition. */
PRTE_EXPORT void prte_state_base_orphaned_proc(pmix_proc_t *proc, prte_proc_state_t state);

// resource recovery
PRTE_EXPORT void prte_state_base_recover_resources(prte_job_t *jdata, prte_proc_t *pptr);

END_C_DECLS

#endif
