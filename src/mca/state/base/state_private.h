/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_MCA_STATE_PRIVATE_H
#define PRTE_MCA_STATE_PRIVATE_H

/*
 * includes
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/state/state.h"

BEGIN_C_DECLS

PRTE_EXPORT extern bool prte_state_base_run_fdcheck;
/*
 * Base functions
 */
PRTE_EXPORT void prte_state_base_activate_job_state(prte_job_t *jdata, prte_job_state_t state);

PRTE_EXPORT int prte_state_base_add_job_state(prte_job_state_t state, prte_state_cbfunc_t cbfunc,
                                              int priority);

PRTE_EXPORT int prte_state_base_set_job_state_callback(prte_job_state_t state,
                                                       prte_state_cbfunc_t cbfunc);

PRTE_EXPORT int prte_state_base_set_job_state_priority(prte_job_state_t state, int priority);

PRTE_EXPORT int prte_state_base_remove_job_state(prte_job_state_t state);

PRTE_EXPORT void prte_util_print_job_state_machine(void);

PRTE_EXPORT void prte_state_base_activate_proc_state(pmix_proc_t *proc, prte_proc_state_t state);

PRTE_EXPORT int prte_state_base_add_proc_state(prte_proc_state_t state, prte_state_cbfunc_t cbfunc,
                                               int priority);

PRTE_EXPORT int prte_state_base_set_proc_state_callback(prte_proc_state_t state,
                                                        prte_state_cbfunc_t cbfunc);

PRTE_EXPORT int prte_state_base_set_proc_state_priority(prte_proc_state_t state, int priority);

PRTE_EXPORT int prte_state_base_remove_proc_state(prte_proc_state_t state);

PRTE_EXPORT void prte_util_print_proc_state_machine(void);

/* common state processing functions */
PRTE_EXPORT void prte_state_base_local_launch_complete(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_cleanup_job(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_report_progress(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_track_procs(int fd, short argc, void *cbdata);
PRTE_EXPORT void prte_state_base_check_all_complete(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_state_base_check_fds(prte_job_t *jdata);
PRTE_EXPORT void prte_state_base_notify_data_server(pmix_proc_t *target);

END_C_DECLS
#endif
