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
 * Copyright (c) 2007-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007-2008 Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/util/arch.h"
#include "src/util/error.h"
#include "src/util/error_strings.h"
#include "src/util/keyval_parse.h"
#include "src/util/listener.h"
#include "src/util/malloc.h"
#include "src/util/name_fns.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/util/stacktrace.h"
#include "src/util/sys_limits.h"

#include "src/hwloc/hwloc-internal.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/threads/threads.h"

#include "src/mca/backtrace/base/base.h"
#include "src/mca/prtecompress/base/base.h"
#include "src/mca/base/base.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/prteif/base/base.h"
#include "src/mca/prteinstalldirs/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/pstat/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/rtc/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"

/*
 * Whether we have completed prrte_init or we are in prrte_finalize
 */
int prrte_initialized = 0;
bool prrte_finalizing = false;
bool prrte_debug_flag = false;
int prrte_debug_verbosity = -1;
char *prrte_prohibited_session_dirs = NULL;
bool prrte_create_session_dirs = true;
prrte_event_base_t *prrte_event_base = {0};
bool prrte_event_base_active = true;
bool prrte_proc_is_bound = false;
int prrte_progress_thread_debug = -1;
hwloc_cpuset_t prrte_proc_applied_binding = NULL;
int prrte_cache_line_size = 128;

prrte_process_name_t prrte_name_wildcard = {PRRTE_JOBID_WILDCARD, PRRTE_VPID_WILDCARD};

prrte_process_name_t prrte_name_invalid = {PRRTE_JOBID_INVALID, PRRTE_VPID_INVALID};


#if PRRTE_CC_USE_PRAGMA_IDENT
#pragma ident PRRTE_IDENT_STRING
#elif PRRTE_CC_USE_IDENT
#ident PRRTE_IDENT_STRING
#endif
const char prrte_version_string[] = PRRTE_IDENT_STRING;

int prrte_init_util(void)
{
    int ret;
    char *error = NULL;

    if (0 < prrte_initialized) {
        /* track number of times we have been called */
        prrte_initialized++;
        return PRRTE_SUCCESS;
    }
    prrte_initialized++;

    /* set the nodename right away so anyone who needs it has it */
    prrte_setup_hostname();

    /* initialize the memory allocator */
    prrte_malloc_init();

    /* initialize the output system */
    prrte_output_init();

    /* initialize install dirs code */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_prteinstalldirs_base_framework, 0))) {
        fprintf(stderr, "prrte_prteinstalldirs_base_open() failed -- process will likely abort (%s:%d, returned %d instead of PRRTE_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }

    /* initialize the help system */
    prrte_show_help_init();

    /* keyval lex-based parser */
    if (PRRTE_SUCCESS != (ret = prrte_util_keyval_parse_init())) {
        error = "prrte_util_keyval_parse_init";
        goto error;
    }

    /* Setup the parameter system */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_var_init())) {
        error = "mca_base_var_init";
        goto error;
    }

    /* Register all MCA Params */
    if (PRRTE_SUCCESS != (ret = prrte_register_params())) {
        error = "prrte_register_params";
        goto error;
    }

    if (PRRTE_SUCCESS != (ret = prrte_hwloc_base_register())) {
        error = "prrte_hwloc_base_register";
        goto error;
    }


    if (PRRTE_SUCCESS != (ret = prrte_net_init())) {
        error = "prrte_net_init";
        goto error;
    }

    /* pretty-print stack handlers */
    if (PRRTE_SUCCESS != (ret = prrte_util_register_stackhandlers())) {
        error = "prrte_util_register_stackhandlers";
        goto error;
    }

    /* set system resource limits - internally protected against
     * doing so twice in cases where the launch agent did it for us
     */
    if (PRRTE_SUCCESS != (ret = prrte_util_init_sys_limits(&error))) {
        prrte_show_help("help-prrte-runtime.txt",
                        "prrte_init:syslimit", false,
                        error);
        return PRRTE_ERR_SILENT;
    }

    /* initialize the arch string */
    if (PRRTE_SUCCESS != (ret = prrte_arch_init ())) {
        error = "prrte_arch_init";
        goto error;
    }

    /* Initialize the data storage service. */
    if (PRRTE_SUCCESS != (ret = prrte_dss_open())) {
        error = "prrte_dss_open";
        goto error;
    }

    /* initialize the mca */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_open())) {
        error = "mca_base_open";
        goto error;
    }

    /* initialize if framework */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_prteif_base_framework, 0))) {
        fprintf(stderr, "prrte_prteif_base_open() failed -- process will likely abort (%s:%d, returned %d instead of PRRTE_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }
    /* add network aliases to our list of alias hostnames */
    prrte_ifgetaliases(&prrte_process_info.aliases);

    /* open hwloc */
    prrte_hwloc_base_open();

    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_backtrace_base_framework, 0))) {
        error = "prrte_backtrace_base_open";
        goto error;
    }

    return PRRTE_SUCCESS;

  error:
    if (PRRTE_ERR_SILENT != ret) {
        prrte_show_help("help-prrte-runtime",
                       "prrte_init:startup:internal-failure",
                       true, error, PRRTE_ERROR_NAME(ret), ret);
    }

    return ret;
}

int prrte_init(int* pargc, char*** pargv, prrte_proc_type_t flags)
{
    int ret;
    char *error = NULL;

   /*
     * Initialize the event library
     */
    if (PRRTE_SUCCESS != (ret = prrte_event_base_open())) {
        error = "prrte_event_base_open";
        goto error;
    }
    prrte_event_use_threads();

    /* ensure we know the type of proc for when we finalize */
    prrte_process_info.proc_type = flags;

    /* setup the locks */
    if (PRRTE_SUCCESS != (ret = prrte_locks_init())) {
        error = "prrte_locks_init";
        goto error;
    }

    /* setup the prrte_show_help system */
    if (PRRTE_SUCCESS != (ret = prrte_show_help_init())) {
        error = "prrte_output_init";
        goto error;
    }

    /* Ensure the rest of the process info structure is initialized */
    if (PRRTE_SUCCESS != (ret = prrte_proc_info())) {
        error = "prrte_proc_info";
        goto error;
    }
    prrte_process_info.proc_type = flags;

    /* let the pmix server register params */
    pmix_server_register_params();

    /* open the SCHIZO framework as everyone needs it, and the
     * ess will use it to help select its component */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_schizo_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_schizo_base_open";
        goto error;
    }

    if (PRRTE_SUCCESS != (ret = prrte_schizo_base_select())) {
        error = "prrte_schizo_base_select";
        goto error;
    }

    /* open the ESS and select the correct module for this environment */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_ess_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_ess_base_open";
        goto error;
    }

    if (PRRTE_SUCCESS != (ret = prrte_ess_base_select())) {
        error = "prrte_ess_base_select";
        goto error;
    }

    /* PRRTE tools "block" in their own loop over the event
     * base, so no progress thread is required */
    prrte_event_base = prrte_sync_event_base;

    /* initialize the RTE for this environment */
    if (PRRTE_SUCCESS != (ret = prrte_ess.init(*pargc, *pargv))) {
        error = "prrte_ess_init";
        goto error;
    }

    /* start listening - will be ignored if no listeners
     * were registered */
    if (PRRTE_SUCCESS != (ret = prrte_start_listening())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_start_listening";
        goto error;
    }

    /* All done */
    return PRRTE_SUCCESS;

 error:
    if (PRRTE_ERR_SILENT != ret) {
        prrte_show_help("help-prrte-runtime",
                       "prrte_init:startup:internal-failure",
                       true, error, PRRTE_ERROR_NAME(ret), ret);
    }

    return ret;
}
