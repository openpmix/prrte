/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2017 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#    include <syslog.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "constants.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/base/prte_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/runtime/runtime.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/proc_info.h"
#include "src/util/prte_environ.h"

/*
 * Public variables
 */
char *prte_mca_base_component_path = NULL;
int prte_mca_base_opened = 0;
char *prte_mca_base_system_default_path = NULL;
char *prte_mca_base_user_default_path = NULL;
bool prte_mca_base_component_show_load_errors = (bool) PRTE_SHOW_LOAD_ERRORS_DEFAULT;
bool prte_mca_base_component_track_load_errors = false;
bool prte_mca_base_component_disable_dlopen = false;

static char *prte_mca_base_verbose = NULL;

/*
 * Private functions
 */
static void set_defaults(prte_output_stream_t *lds);
static void parse_verbose(char *e, prte_output_stream_t *lds);

/*
 * Main MCA initialization.
 */
int prte_mca_base_open(void)
{
    char *value;
    prte_output_stream_t lds;

    if (prte_mca_base_opened++) {
        return PRTE_SUCCESS;
    }

    if (PRTE_PROC_IS_MASTER) {
        /* define the system and user default paths */
#if PRTE_WANT_HOME_CONFIG_FILES
        prte_mca_base_system_default_path = strdup(prte_install_dirs.prtelibdir);
        value = (char *) prte_home_directory();
        if (NULL == value) {
            prte_output(0, "Error: Unable to get the user home directory\n");
            return PRTE_ERROR;
        }
        prte_asprintf(&prte_mca_base_user_default_path,
                      "%s" PRTE_PATH_SEP ".prte" PRTE_PATH_SEP "components", value);
#else
        prte_asprintf(&prte_mca_base_system_default_path, "%s", prte_install_dirs.prtelibdir);
#endif

        /* see if the user wants to override the defaults */
        if (NULL == prte_mca_base_user_default_path) {
            value = strdup(prte_mca_base_system_default_path);
        } else {
            prte_asprintf(&value, "%s%c%s", prte_mca_base_system_default_path, PRTE_ENV_SEP,
                          prte_mca_base_user_default_path);
        }
    } else {
        prte_asprintf(&prte_mca_base_system_default_path, "%s", prte_install_dirs.prtelibdir);
        value = strdup(prte_mca_base_system_default_path);
    }

    prte_mca_base_component_path = value;
    prte_mca_base_var_register("prte", "mca", "base", "component_path",
                               "Path where to look for additional components",
                               PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_mca_base_component_path);
    free(value);

    prte_mca_base_component_show_load_errors = (bool) PRTE_SHOW_LOAD_ERRORS_DEFAULT;
    prte_mca_base_var_register("prte", "mca", "base", "component_show_load_errors",
                               "Whether to show errors for components that failed to load or not",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_mca_base_component_show_load_errors);

    prte_mca_base_component_track_load_errors = false;
    prte_mca_base_var_register("prte", "mca", "base", "component_track_load_errors",
                               "Whether to track errors for components that failed to load or not",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_mca_base_component_track_load_errors);

    prte_mca_base_component_disable_dlopen = false;
    prte_mca_base_var_register("prte", "mca", "base", "component_disable_dlopen",
                               "Whether to attempt to disable opening dynamic components or not",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_mca_base_component_disable_dlopen);

    /* What verbosity level do we want for the default 0 stream? */
    char *str = getenv("PRTE_OUTPUT_INTERNAL_TO_STDOUT");
    if (NULL != str && str[0] == '1') {
        prte_mca_base_verbose = "stdout";
    } else {
        prte_mca_base_verbose = "stderr";
    }
    prte_mca_base_var_register(
        "prte", "mca", "base", "verbose",
        "Specifies where the default error output stream goes (this is separate from distinct help "
        "messages).  Accepts a comma-delimited list of: stderr, stdout, syslog, "
        "syslogpri:<notice|info|debug>, syslogid:<str> (where str is the prefix string for all "
        "syslog notices), file[:filename] (if filename is not specified, a default filename is "
        "used), fileappend (if not specified, the file is opened for truncation), level[:N] (if "
        "specified, integer verbose level; otherwise, 0 is implied)",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_mca_base_verbose);

    memset(&lds, 0, sizeof(lds));
    if (NULL != prte_mca_base_verbose) {
        parse_verbose(prte_mca_base_verbose, &lds);
    } else {
        set_defaults(&lds);
    }
    prte_asprintf(&lds.lds_prefix, "[%s:%05d] ", prte_process_info.nodename, getpid());
    prte_output_reopen(0, &lds);
    prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, 0, "mca: base: opening components");
    free(lds.lds_prefix);

    /* Open up the component repository */
    return prte_mca_base_component_repository_init();
}

/*
 * Set sane default values for the lds
 */
static void set_defaults(prte_output_stream_t *lds)
{

    /* Load up defaults */

    PRTE_CONSTRUCT(lds, prte_output_stream_t);
#if defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H)
    lds->lds_syslog_priority = LOG_INFO;
    lds->lds_syslog_ident = "prte";
#endif
    lds->lds_want_stderr = true;
}

/*
 * Parse the value of an environment variable describing verbosity
 */
static void parse_verbose(char *e, prte_output_stream_t *lds)
{
    char *edup;
    char *ptr, *next;
    bool have_output = false;

    if (NULL == e) {
        return;
    }

    edup = strdup(e);
    ptr = edup;

    /* Now parse the environment variable */

    while (NULL != ptr && strlen(ptr) > 0) {
        next = strchr(ptr, ',');
        if (NULL != next) {
            *next = '\0';
        }

        if (0 == strcasecmp(ptr, "syslog")) {
#if defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H)
            lds->lds_want_syslog = true;
            have_output = true;
#else
            prte_output(0, "syslog support requested but not available on this system");
#endif /* defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H) */
        } else if (strncasecmp(ptr, "syslogpri:", 10) == 0) {
#if defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H)
            lds->lds_want_syslog = true;
            have_output = true;
            if (strcasecmp(ptr + 10, "notice") == 0)
                lds->lds_syslog_priority = LOG_NOTICE;
            else if (strcasecmp(ptr + 10, "INFO") == 0)
                lds->lds_syslog_priority = LOG_INFO;
            else if (strcasecmp(ptr + 10, "DEBUG") == 0)
                lds->lds_syslog_priority = LOG_DEBUG;
#else
            prte_output(0, "syslog support requested but not available on this system");
#endif /* defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H) */
        } else if (strncasecmp(ptr, "syslogid:", 9) == 0) {
#if defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H)
            lds->lds_want_syslog = true;
            lds->lds_syslog_ident = ptr + 9;
#else
            prte_output(0, "syslog support requested but not available on this system");
#endif /* defined(HAVE_SYSLOG) && defined(HAVE_SYSLOG_H) */
        }

        else if (strcasecmp(ptr, "stdout") == 0) {
            lds->lds_want_stdout = true;
            have_output = true;
        } else if (strcasecmp(ptr, "stderr") == 0) {
            lds->lds_want_stderr = true;
            have_output = true;
        }

        else if (strcasecmp(ptr, "file") == 0 || strcasecmp(ptr, "file:") == 0) {
            lds->lds_want_file = true;
            have_output = true;
        } else if (strncasecmp(ptr, "file:", 5) == 0) {
            lds->lds_want_file = true;
            lds->lds_file_suffix = strdup(ptr + 5);
            have_output = true;
        } else if (strcasecmp(ptr, "fileappend") == 0) {
            lds->lds_want_file = true;
            lds->lds_want_file_append = 1;
            have_output = true;
        }

        else if (strncasecmp(ptr, "level", 5) == 0) {
            lds->lds_verbose_level = 0;
            if (ptr[5] == PRTE_ENV_SEP)
                lds->lds_verbose_level = atoi(ptr + 6);
        }

        if (NULL == next) {
            break;
        }
        ptr = next + 1;
    }

    /* If we didn't get an output, default to stderr */

    if (!have_output) {
        lds->lds_want_stderr = true;
    }

    /* All done */

    free(edup);
}
