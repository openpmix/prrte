/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_STATE_PRIVATE_H
#define PRRTE_MCA_STATE_PRIVATE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */

#include "src/mca/plm/plm_types.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/state/state.h"

BEGIN_C_DECLS

PRRTE_EXPORT extern bool prrte_state_base_run_fdcheck;
/*
 * Base functions
 */
PRRTE_EXPORT void prrte_state_base_activate_job_state(prrte_job_t *jdata,
                                                      prrte_job_state_t state);

PRRTE_EXPORT int prrte_state_base_add_job_state(prrte_job_state_t state,
                                                prrte_state_cbfunc_t cbfunc,
                                                int priority);

PRRTE_EXPORT int prrte_state_base_set_job_state_callback(prrte_job_state_t state,
                                                         prrte_state_cbfunc_t cbfunc);

PRRTE_EXPORT int prrte_state_base_set_job_state_priority(prrte_job_state_t state,
                                                         int priority);

PRRTE_EXPORT int prrte_state_base_remove_job_state(prrte_job_state_t state);

PRRTE_EXPORT void prrte_util_print_job_state_machine(void);


PRRTE_EXPORT void prrte_state_base_activate_proc_state(prrte_process_name_t *proc,
                                                       prrte_proc_state_t state);

PRRTE_EXPORT int prrte_state_base_add_proc_state(prrte_proc_state_t state,
                                                 prrte_state_cbfunc_t cbfunc,
                                                 int priority);

PRRTE_EXPORT int prrte_state_base_set_proc_state_callback(prrte_proc_state_t state,
                                                          prrte_state_cbfunc_t cbfunc);

PRRTE_EXPORT int prrte_state_base_set_proc_state_priority(prrte_proc_state_t state,
                                                          int priority);

PRRTE_EXPORT int prrte_state_base_remove_proc_state(prrte_proc_state_t state);

PRRTE_EXPORT void prrte_util_print_proc_state_machine(void);

/* common state processing functions */
PRRTE_EXPORT void prrte_state_base_local_launch_complete(int fd, short argc, void *cbdata);
PRRTE_EXPORT void prrte_state_base_cleanup_job(int fd, short argc, void *cbdata);
PRRTE_EXPORT void prrte_state_base_report_progress(int fd, short argc, void *cbdata);
PRRTE_EXPORT void prrte_state_base_track_procs(int fd, short argc, void *cbdata);
PRRTE_EXPORT void prrte_state_base_check_all_complete(int fd, short args, void *cbdata);
PRRTE_EXPORT void prrte_state_base_check_fds(prrte_job_t *jdata);
PRRTE_EXPORT void prrte_state_base_notify_data_server(prrte_process_name_t *target);

END_C_DECLS
#endif
