/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2018 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2008 Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/error.h"
#include "src/util/error_strings.h"
#include "src/util/keyval_parse.h"
#include "src/util/malloc.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_show_help.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/util/stacktrace.h"
#include "src/util/sys_limits.h"

#include "src/hwloc/hwloc-internal.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/threads/pmix_threads.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/mca/prtebacktrace/base/base.h"
#include "src/mca/prteinstalldirs/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/rtc/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"

#include "src/runtime/pmix_rte.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/runtime.h"

/*
 * Whether we have completed prte_init or we are in prte_finalize
 */
bool prte_initialized = false;
bool prte_finalizing = false;
bool prte_debug_flag = false;
int prte_debug_verbosity = -1;
char *prte_prohibited_session_dirs = NULL;
bool prte_create_session_dirs = true;
prte_event_base_t *prte_event_base = {0};
bool prte_event_base_active = true;
bool prte_proc_is_bound = false;
int prte_progress_thread_debug = -1;
hwloc_cpuset_t prte_proc_applied_binding = NULL;
int prte_cache_line_size = 128;

pmix_proc_t prte_name_wildcard = {{0}, PMIX_RANK_WILDCARD};

pmix_proc_t prte_name_invalid = {{0}, PMIX_RANK_INVALID};

pmix_nspace_t prte_nspace_wildcard = {0};

static bool util_initialized = false;

#if PRTE_CC_USE_PRAGMA_IDENT
#    pragma ident PRTE_IDENT_STRING
#elif PRTE_CC_USE_IDENT
#    ident PRTE_IDENT_STRING
#endif
const char prte_version_string[] = PRTE_IDENT_STRING;

int prte_init_util(prte_proc_type_t flags)
{
    int ret;
    char *error = NULL;
    char *tmp, *value;
    char **paths = NULL;

    if (util_initialized) {
        return PRTE_SUCCESS;
    }
    util_initialized = true;

    /* carry across the toolname */
    pmix_tool_basename = prte_tool_basename;

    /* ensure we know the type of proc for when we finalize */
    prte_process_info.proc_type = flags;

    /* initialize the memory allocator */
    prte_malloc_init();

    /* initialize the output system */
    pmix_output_init();
    prte_output_init();

    /* initialize install dirs code */
    ret = pmix_mca_base_framework_open(&pmix_pinstalldirs_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr,
                "pmix_pinstalldirs_base_open() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PMIX_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }
    if (PMIX_SUCCESS != (ret = pmix_pinstall_dirs_base_init(NULL, 0))) {
        fprintf(stderr,
                "pmix_pinstalldirs_base_init() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PMIX_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }
    ret = pmix_mca_base_framework_open(&prte_prteinstalldirs_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr,
                "prte_prteinstalldirs_base_open() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PRTE_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }

    /* initialize the help system */
    pmix_show_help_init();
    prte_show_help_init();

    /* keyval lex-based parser */
    if (PMIX_SUCCESS != (ret = pmix_util_keyval_parse_init())) {
        error = "pmix_util_keyval_parse_init";
        goto error;
    }
    if (PRTE_SUCCESS != (ret = prte_util_keyval_parse_init())) {
        error = "prte_util_keyval_parse_init";
        goto error;
    }

    /* Setup the parameter system */
    if (PRTE_SUCCESS != (ret = pmix_mca_base_var_init())) {
        error = "mca_base_var_init";
        goto error;
    }

    /* register params for pmix */
    if (PMIX_SUCCESS != (ret = pmix_register_params())) {
        error = "pmix_register_params";
        goto error;
    }

    /* set the nodename so anyone who needs it has it - this
     * must come AFTER we initialize the installdirs as it
     * causes the MCA var system to initialize */
    prte_setup_hostname();
    /* load the output verbose stream */
    prte_output_setup_stream_prefix();

    /* pretty-print stack handlers */
    if (PRTE_SUCCESS != (ret = prte_util_register_stackhandlers())) {
        error = "prte_util_register_stackhandlers";
        goto error;
    }

    /* set system resource limits - internally protected against
     * doing so twice in cases where the launch agent did it for us
     */
    if (PRTE_SUCCESS != (ret = prte_util_init_sys_limits(&error))) {
        prte_show_help("help-prte-runtime.txt", "prte_init:syslimit", false, error);
        return PRTE_ERR_SILENT;
    }

    /* setup the paths to the PRRTE component libraries */
    pmix_argv_append_nosize(&paths, prte_install_dirs.prtelibdir);
#if PRTE_WANT_HOME_CONFIG_FILES
    value = (char *) pmix_home_directory(geteuid());
    pmix_asprintf(&tmp,
                  "%s" PMIX_PATH_SEP ".prte" PMIX_PATH_SEP "components", value);
    if (PMIX_SUCCESS == pmix_os_dirpath_access(tmp, 0)) {
        pmix_argv_append_nosize(&paths, tmp);
    }
    free(tmp);
#endif
    value = pmix_argv_join(paths, PMIX_ENV_SEP);
    pmix_asprintf(&tmp, "prte@%s", value);
    free(value);

    /* Initialize the data storage service. */ /* initialize the mca */
    ret = pmix_mca_base_open(tmp);
    free(tmp);
    if (PMIX_SUCCESS != ret) {
        error = "mca_base_open";
        goto error;
    }

    /* Register all MCA Params */
    if (PRTE_SUCCESS != (ret = prte_register_params())) {
        error = "prte_register_params";
        goto error;
    }

    if (PRTE_SUCCESS
        != (ret = pmix_mca_base_framework_open(&prte_prtebacktrace_base_framework,
                                               PMIX_MCA_BASE_OPEN_DEFAULT))) {
        error = "prte_backtrace_base_open";
        goto error;
    }

    return PRTE_SUCCESS;

error:
    if (PRTE_ERR_SILENT != ret) {
        prte_show_help("help-prte-runtime", "prte_init:startup:internal-failure", true, error,
                       PRTE_ERROR_NAME(ret), ret);
    }

    return ret;
}

int prte_init(int *pargc, char ***pargv, prte_proc_type_t flags)
{
    int ret;
    char *error = NULL;

    PMIX_ACQUIRE_THREAD(&prte_init_lock);
    if (prte_initialized) {
        PMIX_RELEASE_THREAD(&prte_init_lock);
        return PRTE_SUCCESS;
    }
    PMIX_RELEASE_THREAD(&prte_init_lock);

    ret = prte_init_util(flags);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /*
     * Initialize the event library
     */
    if (PRTE_SUCCESS != (ret = prte_event_base_open())) {
        error = "prte_event_base_open";
        goto error;
    }

    /* ensure we know the type of proc for when we finalize */
    prte_process_info.proc_type = flags;

    /* setup the locks */
    if (PRTE_SUCCESS != (ret = prte_locks_init())) {
        error = "prte_locks_init";
        goto error;
    }

    /* setup the prte_show_help system */
    if (PRTE_SUCCESS != (ret = prte_show_help_init())) {
        error = "prte_output_init";
        goto error;
    }

    /* Ensure the rest of the process info structure is initialized */
    if (PRTE_SUCCESS != (ret = prte_proc_info())) {
        error = "prte_proc_info";
        goto error;
    }
    prte_process_info.proc_type = flags;

    if (PRTE_SUCCESS != (ret = prte_hwloc_base_register())) {
        error = "prte_hwloc_base_register";
        goto error;
    }

    /* let the pmix server register params */
    pmix_server_register_params();

    /* open hwloc */
    prte_hwloc_base_open();

    /* setup the global job and node arrays */
    prte_job_data = PMIX_NEW(pmix_pointer_array_t);
    if (PRTE_SUCCESS
        != (ret = pmix_pointer_array_init(prte_job_data, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                          PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                          PRTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        PRTE_ERROR_LOG(ret);
        error = "setup job array";
        goto error;
    }
    prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
    if (PRTE_SUCCESS
        != (ret = pmix_pointer_array_init(prte_node_pool, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                          PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                          PRTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        PRTE_ERROR_LOG(ret);
        error = "setup node array";
        goto error;
    }
    prte_node_topologies = PMIX_NEW(pmix_pointer_array_t);
    if (PRTE_SUCCESS
        != (ret = pmix_pointer_array_init(prte_node_topologies, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                          PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                          PRTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        PRTE_ERROR_LOG(ret);
        error = "setup node topologies array";
        goto error;
    }

    /* open the SCHIZO framework as everyone needs it, and the
     * ess will use it to help select its component */
    if (PRTE_SUCCESS
        != (ret = pmix_mca_base_framework_open(&prte_schizo_base_framework,
                                               PMIX_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_schizo_base_open";
        goto error;
    }

    if (PRTE_SUCCESS != (ret = prte_schizo_base_select())) {
        error = "prte_schizo_base_select";
        goto error;
    }

    /* open the ESS and select the correct module for this environment */
    if (PRTE_SUCCESS
        != (ret = pmix_mca_base_framework_open(&prte_ess_base_framework,
                                               PMIX_MCA_BASE_OPEN_DEFAULT))) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ess_base_open";
        goto error;
    }

    if (PRTE_SUCCESS != (ret = prte_ess_base_select())) {
        error = "prte_ess_base_select";
        goto error;
    }

    /* initialize the RTE for this environment */
    if (PRTE_SUCCESS != (ret = prte_ess.init(*pargc, *pargv))) {
        error = "prte_ess_init";
        goto error;
    }

    /* add network aliases to our list of alias hostnames */
    if (PRTE_SUCCESS != (ret = pmix_net_init())) {
        error = "pmix_net_init";
        goto error;
    }
    pmix_ifgetaliases(&prte_process_info.aliases);

    /* initialize the cache */
    prte_cache = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_cache, 1, INT_MAX, 1);

#if PRTE_ENABLE_FT
    if (PRTE_PROC_IS_MASTER || PRTE_PROC_IS_DAEMON) {
        if (NULL != prte_errmgr.enable_detector) {
            prte_errmgr.enable_detector(prte_enable_ft);
        }
    }
#endif

    /* All done */
    PMIX_ACQUIRE_THREAD(&prte_init_lock);
    prte_initialized = true;
    PMIX_RELEASE_THREAD(&prte_init_lock);
    return PRTE_SUCCESS;

error:
    if (PRTE_ERR_SILENT != ret) {
        prte_show_help("help-prte-runtime", "prte_init:startup:internal-failure", true, error,
                       PRTE_ERROR_NAME(ret), ret);
    }

    return ret;
}
