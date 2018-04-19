/*
 * Copyright (c) 2015-2018 Intel, Inc. All rights reserved.
 * Copyright (c) 2016      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef ORTED_SUBMIT_H
#define ORTED_SUBMIT_H

#include "orte_config.h"

#include "opal/util/cmd_line.h"

#include "orte/mca/plm/plm.h"
#include "orte/runtime/orte_globals.h"

BEGIN_C_DECLS


/**
 * Global struct for caching orte command line options.
 */
struct orte_cmd_options_t {
    char *help;
    bool version;
    bool verbose;
    char *report_pid;
    char *report_uri;
    bool terminate;
    bool debugger;
    int num_procs;
    char *appfile;
    char *wdir;
    bool set_cwd_to_session_dir;
    char *path;
    char *preload_files;
    bool sleep;
    char *stdin_target;
    char *prefix;
    char *path_to_mpirun;
#if OPAL_ENABLE_FT_CR == 1
    char *sstore_load;
#endif
    bool disable_recovery;
    bool preload_binaries;
    bool index_argv;
    bool run_as_root;
    char *personality;
    bool create_dvm;
    bool terminate_dvm;
    bool nolocal;
    bool no_oversubscribe;
    bool oversubscribe;
    int cpus_per_proc;
    bool pernode;
    int npernode;
    bool use_hwthreads_as_cpus;
    int npersocket;
    char *mapping_policy;
    char *ranking_policy;
    char *binding_policy;
    bool report_bindings;
    char *cpu_list;
    bool debug;
    bool tag_output;
    bool timestamp_output;
    char *output_filename;
    bool merge;
    bool continuous;
    char *hnp;
    bool staged_exec;
    int timeout;
    bool report_state_on_timeout;
    bool get_stack_traces;
};
typedef struct orte_cmd_options_t orte_cmd_options_t;
ORTE_DECLSPEC extern orte_cmd_options_t orte_cmd_options;
ORTE_DECLSPEC extern opal_cmd_line_t *orte_cmd_line;


END_C_DECLS

#endif /* ORTED_SUBMIT_H */
