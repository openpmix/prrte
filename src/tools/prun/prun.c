/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/include/constants.h"
#include "src/include/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_POLL_H
#    include <poll.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/threads/pmix_mutex.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"

#include "prun.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/state.h"
#include "src/prted/prted.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"
typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

int prun(int argc, char *argv[])
{
    int rc = 1, i;
    pmix_list_t apps;
    char **pargv;
    int pargc;
    prte_schizo_base_module_t *schizo;
    char hostname[PRTE_PATH_MAX];
    char *personality;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;
    FILE *fp;
    char *mypidfile = NULL;
    bool first;
    char **split;
    char *param;
    int n;

    /* init the globals */
    PMIX_CONSTRUCT(&apps, pmix_list_t);
    prte_tool_basename = pmix_basename(argv[0]);
    prte_tool_actual = "prun";
    pargc = argc;
    pargv = pmix_argv_copy_strip(argv);  // strip any quoted arguments
    gethostname(hostname, sizeof(hostname));

    rc = prte_init_minimum();
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* because we have to use the schizo framework and init our hostname
     * prior to parsing the incoming argv for cmd line options, do a hacky
     * search to support passing of impacted options (e.g., verbosity for schizo) */
    rc = prte_schizo_base_parse_prte(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    rc = prte_schizo_base_parse_pmix(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* init the tiny part of PRTE we use */
    prte_init_util(PRTE_PROC_TYPE_NONE);

    /* setup an event base */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "Unable to initialize event library\n");
        exit(1);
    }

    /* open the SCHIZO framework */
    rc = pmix_mca_base_framework_open(&prte_schizo_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* look for any personality specification and do a quick sanity check */
    personality = NULL;
    bool rankby_found = false;
    bool bindto_found = false;
    for (i = 0; NULL != pargv[i]; i++) {
        if (0 == strcmp(pargv[i], "--personality")) {
            personality = pargv[i + 1];
            continue;
        }
        if (0 == strcmp(pargv[i], "--map-by")) {
            free(pargv[i]);
            pargv[i] = strdup("--mapby");
            continue;
        }
        if (0 == strcmp(pargv[i], "--rank-by") ||
            0 == strcmp(pargv[i], "--rankby")) {
            if (rankby_found) {
                pmix_show_help("help-schizo-base.txt", "multi-instances", true, pargv[i]);
                return PRTE_ERR_BAD_PARAM;
            }
            rankby_found = true;
            if (0 == strcmp(pargv[i], "--rank-by")) {
                free(pargv[i]);
                pargv[i] = strdup("--rankby");
            }
            continue;
        }
        if (0 == strcmp(pargv[i], "--bind-to") ||
            0 == strcmp(pargv[i], "--bindto")) {
            if (bindto_found) {
                pmix_show_help("help-schizo-base.txt", "multi-instances", true, "bind-to");
                return PRTE_ERR_BAD_PARAM;
            }
            bindto_found = true;
            if (0 == strcmp(pargv[i], "--bind-to")) {
                free(pargv[i]);
                pargv[i] = strdup("--bindto");
            }
            continue;
        }
        if (0 == strcmp(pargv[i], "--runtime-options")) {
            free(pargv[i]);
            pargv[i] = strdup("--rtos");
            continue;
        }
    }

    /* detect if we are running as a proxy and select the active
     * schizo module for this tool */
    schizo = prte_schizo_base_detect_proxy(personality);
    if (NULL == schizo) {
        pmix_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, personality);
        return 1;
    }
    if (NULL == personality) {
        personality = schizo->name;
    }

    /* Register all global MCA Params */
    if (PRTE_SUCCESS != (rc = prte_register_params())) {
        if (PRTE_ERR_SILENT != rc) {
            pmix_show_help("help-prte-runtime", "prte_init:startup:internal-failure", true,
                           "prte register params",
                           PRTE_ERROR_NAME(rc), rc);
        }
        return 1;
    }

    /* parse the input argv to get values, including everyone's MCA params */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    // check for special case of executable immediately following tool
    if ('-' != pargv[1][0]) {
        results.tail = PMIx_Argv_copy(&pargv[1]);
    } else {
        rc = schizo->parse_cli(pargv, &results, PMIX_CLI_WARN);
        if (PRTE_SUCCESS != rc) {
            PMIX_DESTRUCT(&results);
            if (PRTE_OPERATION_SUCCEEDED == rc) {
                return PRTE_SUCCESS;
            }
            if (PRTE_ERR_SILENT != rc) {
                fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(rc));
            }
            return rc;
        }
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning message
     */
    if (0 == geteuid()) {
        schizo->allow_run_as_root(&results); // will exit us if not allowed
    }

    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_REPORT_PID);
    if (NULL != opt) {
        /* if the string is a "-", then output to stdout */
        if (0 == strcmp(opt->values[0], "-")) {
            fprintf(stdout, "%lu\n", (unsigned long) getpid());
        } else if (0 == strcmp(opt->values[0], "+")) {
            /* output to stderr */
            fprintf(stderr, "%lu\n", (unsigned long) getpid());
        } else {
            char *leftover;
            int outpipe;
            /* see if it is an integer pipe */
            leftover = NULL;
            outpipe = strtol(opt->values[0], &leftover, 10);
            if (NULL == leftover || 0 == strlen(leftover)) {
                /* stitch together the var names and URI */
                pmix_asprintf(&leftover, "%lu", (unsigned long) getpid());
                /* output to the pipe */
                pmix_fd_write(outpipe, strlen(leftover) + 1, leftover);
                free(leftover);
                close(outpipe);
            } else {
                /* must be a file */
                fp = fopen(opt->values[0], "w");
                if (NULL == fp) {
                    pmix_output(0, "Impossible to open the file %s in write mode\n", opt->values[0]);
                    PRTE_UPDATE_EXIT_STATUS(1);
                    goto DONE;
                }
                /* output my PID */
                fprintf(fp, "%lu\n", (unsigned long) getpid());
                fclose(fp);
                mypidfile = strdup(opt->values[0]);
            }
        }
    }

    /* if we were asked to report a uri, set the MCA param to do so */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_REPORT_URI);
    if (NULL != opt) {
        prte_pmix_server_globals.report_uri = strdup(opt->values[0]);
    }

    // check for an appfile
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_APPFILE);
    if (NULL != opt) {
        // parse the file and add its context to the argv array
        fp = fopen(opt->values[0], "r");
        if (NULL == fp) {
            pmix_show_help("help-prun.txt", "appfile-failure", true, opt->values[0]);
            if (NULL != mypidfile) {
                free(mypidfile);
            }
            return 1;
        }
        first = true;
        while (NULL != (param = pmix_getline(fp))) {
            if (!first) {
                // add a colon delimiter
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&pargv, ":");
                ++pargc;
            }
            // break the line down into parts
            split = PMIX_ARGV_SPLIT_COMPAT(param, ' ');
            for (n=0; NULL != split[n]; n++) {
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&pargv, split[n]);
                ++pargc;
            }
            PMIX_ARGV_FREE_COMPAT(split);
            first = false;
        }
        fclose(fp);
    }

    // open the ess framework so it can init the signal forwarding
    // list - we don't actually need the components
    rc = pmix_mca_base_framework_open(&prte_ess_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto DONE;
    }


    rc = prun_common(&results, schizo, pargc, pargv);

DONE:
    // cleanup and leave
    if (NULL != mypidfile) {
        unlink(mypidfile);
    }
    (void) pmix_mca_base_framework_close(&prte_ess_base_framework);

    exit(prte_exit_status);
}
