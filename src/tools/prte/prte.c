/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif  /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif  /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/installdirs/installdirs.h"
#include "src/mca/base/base.h"

#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/cmd_line.h"
#include "src/util/daemon_init.h"
#include "src/util/fd.h"
#include "src/util/output.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/prrte_environ.h"
#include "src/util/prrte_getcwd.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"

#include "src/include/version.h"
#include "src/class/prrte_pointer_array.h"
#include "src/runtime/prrte_progress_threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/odls/odls.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/base/base.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/threads/threads.h"

#include "src/prted/prted.h"

/*
 * Globals
 */
static bool want_prefix_by_default = (bool) PRRTE_WANT_PRRTE_PREFIX_BY_DEFAULT;

/* prte-specific command line options */
static prrte_cmd_line_init_t cmd_line_init[] = {
    /* DVM-specific options */

    /* uri of PMIx publish/lookup server, or at least where to get it */
    { '\0', "prrte-server", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the publish/lookup server, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* fwd mpirun port */
    { '\0', "fwd-mpirun-port", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Forward mpirun port to compute node daemons so all will use it",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "system-server", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Act as system server",
      PRRTE_CMD_LINE_OTYPE_DVM },


    /* Debug options */
    { '\0', "debug", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Debug PRRTE",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "debug-daemons", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Debug daemons",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { 'd', "debug-devel", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of PRRTE",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "debug-daemons-file", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of any PRRTE daemons used by this application, storing output in files",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "leave-session-attached", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not discard stdout/stderr of remote PRTE daemons",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0',  "test-suicide", 1, PRRTE_CMD_LINE_TYPE_BOOL,
      "Suicide instead of clean abort after delay",
      PRRTE_CMD_LINE_OTYPE_DEBUG },



    { '\0', "do-not-resolve", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not attempt to resolve interfaces",
      PRRTE_CMD_LINE_OTYPE_DEVEL },


    { '\0', "remote-tools", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable connections from remote tools",
      PRRTE_CMD_LINE_OTYPE_DVM },

    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static int wait_pipe[2];

static int wait_dvm(pid_t pid) {
    char reply;
    int rc;
    int status;

    close(wait_pipe[1]);
    do {
        rc = read(wait_pipe[0], &reply, 1);
    } while (0 > rc && EINTR == errno);

    if (1 == rc && 'K' == reply) {
        return 0;
    } else if (0 == rc) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
    }
    return 255;
}

int main(int argc, char *argv[])
{
    int rc, i, j;
    ssize_t param_len;
    prrte_cmd_line_t cmd_line;
    char *param;
    prrte_job_t *jdata=NULL;
    prrte_app_context_t *app;
    prrte_value_t *pval;

    /* init the tiny part of PRRTE we use */
    prrte_init_util();

    /* find our basename (the name of the executable) so that we can
       use it in pretty-print error messages */
    prrte_tool_basename = prrte_basename(argv[0]);

    rc = prrte_cmd_line_create(&cmd_line, cmd_line_init);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* open the SCHIZO framework */
    if (PRRTE_SUCCESS != (rc = prrte_mca_base_framework_open(&prrte_schizo_base_framework, 0))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRRTE_SUCCESS != (rc = prrte_schizo_base_select())) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    /* scan for personalities */
    prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "prrte", false);
    prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "pmix", false);
    for (i=0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, argv[i+1], false);
        }
    }

    /* setup the rest of the cmd line only once */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.define_cli(&cmd_line))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_parse(&cmd_line, true, false,
                                                  argc, argv)) ) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(rc));
        }
        return rc;
    }

    /* now let the schizo components take a pass at it to get the MCA params */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.parse_cli(argc, 0, argv, NULL, NULL))) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(rc));
        }
         PRRTE_ERROR_LOG(rc);
       return rc;
    }

    /* print version if requested.  Do this before check for help so
       that --version --help works as one might expect. */
     if (prrte_cmd_line_is_taken(&cmd_line, "version")) {
        fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n",
                prrte_tool_basename, "PMIx Reference RunTime Environment",
                PRRTE_VERSION, PACKAGE_BUGREPORT);
        exit(0);
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid()) {
        prrte_schizo.allow_run_as_root(&cmd_line);  // will exit us if not allowed
    }

    /* Check for help request */
     if (prrte_cmd_line_is_taken(&cmd_line, "help")) {
        char *str, *args = NULL;
        char *project_name = "PMIx Reference RTE";
        args = prrte_cmd_line_get_usage_msg(&cmd_line, false);
        str = prrte_show_help_string("help-prun.txt", "prun:usage", false,
                                    prrte_tool_basename, project_name, PRRTE_VERSION,
                                    prrte_tool_basename, args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    if (prrte_cmd_line_is_taken(&cmd_line, "system-server")) {
        /* we should act as system-level PMIx server */
        prrte_setenv("PRRTE_MCA_pmix_system_server", "1", true, &environ);
    }
    /* always act as session-level PMIx server */
    prrte_setenv("PRRTE_MCA_pmix_session_server", "1", true, &environ);
    /* if we were asked to report a uri, set the MCA param to do so */
     if (NULL != (pval = prrte_cmd_line_get_param(&cmd_line, "report-uri", 0, 0))) {
        prrte_setenv("PMIX_MCA_ptl_tcp_report_uri", pval->data.string, true, &environ);
    }
    if (prrte_cmd_line_is_taken(&cmd_line, "remote-tools")) {
        prrte_setenv("PMIX_MCA_ptl_tcp_remote_connections", "1", true, &environ);
    }
    /* don't aggregate help messages as that will apply job-to-job */
    prrte_setenv("PRRTE_MCA_prrte_base_help_aggregate", 0, true, &environ);

    /* Setup MCA params */
    prrte_register_params();

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars were set in the enviro prior to calling
     * prun
     */
    prrte_launch_environ = prrte_argv_copy(environ);

#if defined(HAVE_SETSID)
    /* see if we were directed to separate from current session */
    if (prrte_cmd_line_is_taken(&cmd_line, "set-sid")) {
        setsid();
    }
#endif

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    prrte_debug_flag = prrte_cmd_line_is_taken(&cmd_line, "debug");
    prrte_debug_daemons_flag = prrte_cmd_line_is_taken(&cmd_line, "debug-daemons");
    if (!prrte_debug_flag &&
        !prrte_debug_daemons_flag &&
        prrte_cmd_line_is_taken(&cmd_line, "daemonize")) {
        pipe(wait_pipe);
        prrte_state_base_parent_fd = wait_pipe[1];
        prrte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
    }

    /* Intialize our environment */
    if (PRRTE_SUCCESS != (rc = prrte_init(&argc, &argv, PRRTE_PROC_MASTER))) {
        /* cannot call PRRTE_ERROR_LOG as it could be the errmgr
         * never got loaded!
         */
        return rc;
    }

     /* get the daemon job object - was created by ess/hnp component */
    if (NULL == (jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid))) {
        prrte_show_help("help-prun.txt", "bad-job-object", true,
                       prrte_tool_basename);
        exit(0);
    }
    /* also should have created a daemon "app" */
    if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0))) {
        prrte_show_help("help-prun.txt", "bad-app-object", true,
                       prrte_tool_basename);
        exit(0);
    }

    /* Did the user specify a prefix, or want prefix by default? */
    if (NULL != (pval = prrte_cmd_line_get_param(&cmd_line, "prefix", 0, 0)) || want_prefix_by_default) {
        if (NULL != pval) {
            param = strdup(pval->data.string);
        } else {
            /* --enable-prun-prefix-default was given to prun */
            param = strdup(prrte_install_dirs.prefix);
        }
        /* "Parse" the param, aka remove superfluous path_sep. */
        param_len = strlen(param);
        while (0 == strcmp (PRRTE_PATH_SEP, &(param[param_len-1]))) {
            param[param_len-1] = '\0';
            param_len--;
            if (0 == param_len) {
                prrte_show_help("help-prun.txt", "prun:empty-prefix",
                               true, prrte_tool_basename, prrte_tool_basename);
                return PRRTE_ERR_FATAL;
            }
        }
        prrte_set_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, PRRTE_ATTR_GLOBAL, param, PRRTE_STRING);
        free(param);
    }

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    if (0 < (j = prrte_cmd_line_get_ninsts(&cmd_line, "hostfile"))) {
        if(1 < j) {
            prrte_show_help("help-prun.txt", "prun:multiple-hostfiles",
                           true, prrte_tool_basename, NULL);
            return PRRTE_ERR_FATAL;
        } else {
            pval = prrte_cmd_line_get_param(&cmd_line, "hostfile", 0, 0);
            prrte_set_attribute(&app->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, pval->data.string, PRRTE_STRING);
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(&cmd_line, "machinefile"))) {
        if(1 < j || prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, NULL, PRRTE_STRING)) {
            prrte_show_help("help-prun.txt", "prun:multiple-hostfiles",
                           true, prrte_tool_basename, NULL);
            return PRRTE_ERR_FATAL;
        } else {
            pval = prrte_cmd_line_get_param(&cmd_line, "machinefile", 0, 0);
            prrte_set_attribute(&app->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, pval->data.string, PRRTE_STRING);
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = prrte_cmd_line_get_ninsts(&cmd_line, "host"))) {
        char **targ=NULL, *tval;
        for (i = 0; i < j; ++i) {
            pval = prrte_cmd_line_get_param(&cmd_line, "host", i, 0);
            prrte_argv_append_nosize(&targ, pval->data.string);
        }
        tval = prrte_argv_join(targ, ',');
        prrte_set_attribute(&app->attributes, PRRTE_APP_DASH_HOST, PRRTE_ATTR_LOCAL, tval, PRRTE_STRING);
        prrte_argv_free(targ);
        free(tval);
    }
    PRRTE_DESTRUCT(&cmd_line);

    /* setup to listen for commands sent specifically to me, even though I would probably
     * be the one sending them! Unfortunately, since I am a participating daemon,
     * there are times I need to send a command to "all daemons", and that means *I* have
     * to receive it too
     */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DAEMON,
                            PRRTE_RML_PERSISTENT, prrte_daemon_recv, NULL);

    /* spawn the DVM - we skip the initial steps as this
     * isn't a user-level application */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOCATE);

    /* loop the event lib until an exit event is detected */
    while (prrte_event_base_active) {
        prrte_event_loop(prrte_event_base, PRRTE_EVLOOP_ONCE);
    }
    PRRTE_ACQUIRE_OBJECT(prrte_event_base_active);

    prrte_finalize();

    if (prrte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prrte_exit_status);
    }
    exit(prrte_exit_status);

}
