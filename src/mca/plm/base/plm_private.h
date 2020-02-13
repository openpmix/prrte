/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
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

#ifndef MCA_PLM_PRIVATE_H
#define MCA_PLM_PRIVATE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "types.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */

#include "src/class/prrte_list.h"
#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss_types.h"
#include "src/mca/base/prrte_mca_base_framework.h"

#include "src/dss/dss_types.h"
#include "src/mca/plm/plm_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/odls/odls_types.h"
#include "src/runtime/prrte_globals.h"


BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_plm_base_framework;

/* globals for use solely within PLM framework */
typedef struct {
    /* next jobid */
    uint16_t next_jobid;
    /* time when daemons started launch */
    struct timeval daemonlaunchstart;
    /* tree spawn cmd */
    prrte_buffer_t tree_spawn_cmd;
    /* daemon nodes assigned at launch */
    bool daemon_nodes_assigned_at_launch;
    size_t node_regex_threshold;
} prrte_plm_globals_t;
/**
 * Global instance of PLM framework data
 */
PRRTE_EXPORT extern prrte_plm_globals_t prrte_plm_globals;


/**
 * Utility routine to set progress engine schedule
 */
PRRTE_EXPORT int prrte_plm_base_set_progress_sched(int sched);

/*
 * Launch support
 */
PRRTE_EXPORT void prrte_plm_base_daemon_callback(int status, prrte_process_name_t* sender,
                                                 prrte_buffer_t *buffer,
                                                 prrte_rml_tag_t tag, void *cbdata);
PRRTE_EXPORT void prrte_plm_base_daemon_failed(int status, prrte_process_name_t* sender,
                                               prrte_buffer_t *buffer,
                                               prrte_rml_tag_t tag, void *cbdata);
PRRTE_EXPORT void prrte_plm_base_daemon_topology(int status, prrte_process_name_t* sender,
                                                 prrte_buffer_t *buffer,
                                                 prrte_rml_tag_t tag, void *cbdata);

PRRTE_EXPORT int prrte_plm_base_create_jobid(prrte_job_t *jdata);
PRRTE_EXPORT int prrte_plm_base_set_hnp_name(void);
PRRTE_EXPORT void prrte_plm_base_reset_job(prrte_job_t *jdata);
PRRTE_EXPORT int prrte_plm_base_setup_prted_cmd(int *argc, char ***argv);
PRRTE_EXPORT void prrte_plm_base_check_all_complete(int fd, short args, void *cbdata);
PRRTE_EXPORT int prrte_plm_base_setup_virtual_machine(prrte_job_t *jdata);
PRRTE_EXPORT int prrte_plm_base_spawn_reponse(int32_t status, prrte_job_t *jdata);

/**
 * Utilities for plm components that use proxy daemons
 */
PRRTE_EXPORT int prrte_plm_base_prted_exit(prrte_daemon_cmd_flag_t command);
PRRTE_EXPORT int prrte_plm_base_prted_terminate_job(prrte_jobid_t jobid);
PRRTE_EXPORT int prrte_plm_base_prted_kill_local_procs(prrte_pointer_array_t *procs);
PRRTE_EXPORT int prrte_plm_base_prted_signal_local_procs(prrte_jobid_t job, int32_t signal);

/*
 * communications utilities
 */
PRRTE_EXPORT int prrte_plm_base_comm_start(void);
PRRTE_EXPORT int prrte_plm_base_comm_stop(void);
PRRTE_EXPORT void prrte_plm_base_recv(int status, prrte_process_name_t* sender,
                                      prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                                      void* cbdata);


/**
 * Construct basic PRRTE Daemon command line arguments
 */
PRRTE_EXPORT int prrte_plm_base_prted_append_basic_args(int *argc, char ***argv,
                                                        char *ess_module,
                                                        int *proc_vpid_index);

/*
 * Proxy functions for use by daemons and application procs
 * needing dynamic operations
 */
PRRTE_EXPORT int prrte_plm_proxy_init(void);
PRRTE_EXPORT int prrte_plm_proxy_spawn(prrte_job_t *jdata);
PRRTE_EXPORT int prrte_plm_proxy_finalize(void);

END_C_DECLS

#endif  /* MCA_PLS_PRIVATE_H */
