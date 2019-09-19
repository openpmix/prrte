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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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
#include "src/runtime/runtime.h"
#include "src/class/prrte_pointer_array.h"
#include "src/runtime/prrte_progress_threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/odls/odls.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/state/state.h"
#include "src/mca/state/base/base.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/threads/threads.h"

#include "src/prted/prted.h"

/*
 * Globals
 */
static bool want_prefix_by_default = (bool) PRRTE_WANT_PRRTE_PREFIX_BY_DEFAULT;

/*
 * Globals
 */
static struct {
    bool help;
    bool version;
    char *prefix;
    bool run_as_root;
    bool set_sid;
    bool daemonize;
    bool system_server;
    char *report_uri;
    bool remote_connections;
} myglobals;

static prrte_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" options */
    { NULL, 'h', NULL, "help", 0,
      &myglobals.help, PRRTE_CMD_LINE_TYPE_BOOL,
      "This help message" },
    { NULL, 'V', NULL, "version", 0,
      &myglobals.version, PRRTE_CMD_LINE_TYPE_BOOL,
      "Print version and exit" },

    { NULL, '\0', "prefix", "prefix", 1,
      &myglobals.prefix, PRRTE_CMD_LINE_TYPE_STRING,
      "Prefix to be used to look for PRRTE executables" },

    { "prrte_daemonize", '\0', NULL, "daemonize", 0,
      &myglobals.daemonize, PRRTE_CMD_LINE_TYPE_BOOL,
      "Daemonize the prrte-dvm into the background" },

    { NULL, '\0', NULL, "set-sid", 0,
      &myglobals.set_sid, PRRTE_CMD_LINE_TYPE_BOOL,
      "Direct the prrte-dvm to separate from the current session"},

    { "prrte_debug_daemons", '\0', "debug-daemons", "debug-daemons", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Debug daemons" },

    { "prrte_debug", 'd', "debug-devel", "debug-devel", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE" },

    { NULL, '\0', "allow-run-as-root", "allow-run-as-root", 0,
      &myglobals.run_as_root, PRRTE_CMD_LINE_TYPE_BOOL,
      "Allow execution as root (STRONGLY DISCOURAGED)" },

    /* Specify the launch agent to be used */
    { "prrte_launch_agent", '\0', "launch-agent", "launch-agent", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Command used to start processes on remote nodes (default: orted)" },

    /* maximum size of VM - typically used to subdivide an allocation */
    { "prrte_max_vm_size", '\0', "max-vm-size", "max-vm-size", 1,
      NULL, PRRTE_CMD_LINE_TYPE_INT,
      "Maximum size of VM" },

    /* Set a hostfile */
    { NULL, '\0', "hostfile", "hostfile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile" },
    { NULL, '\0', "machinefile", "machinefile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile" },
    { "prrte_default_hostfile", '\0', "default-hostfile", "default-hostfile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a default hostfile" },

    { NULL, 'H', "host", "host", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "List of hosts to invoke processes on" },

    { NULL, '\0', "system-server", "system-server", 0,
      &myglobals.system_server, PRRTE_CMD_LINE_TYPE_BOOL,
      "Provide a system-level server connection point - only one allowed per node" },

    { NULL, '\0', "report-uri", "report-uri", 1,
      &myglobals.report_uri, PRRTE_CMD_LINE_TYPE_STRING,
      "Printout URI on stdout [-], stderr [+], or a file [anything else]",
      PRRTE_CMD_LINE_OTYPE_DEBUG },

    { NULL, '\0', "remote-tools", "remote-tools", 0,
      &myglobals.remote_connections, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable connections from remote tools" },

    /* End of list */
    { NULL, '\0', NULL, NULL, 0,
      NULL, PRRTE_CMD_LINE_TYPE_NULL, NULL }
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

static char *make_version_str(int major, int minor, int release,
                              const char *greek,
                              const char *repo)
{
    char *str = NULL, *tmp;
    char temp[BUFSIZ];

    temp[BUFSIZ - 1] = '\0';
    snprintf(temp, BUFSIZ - 1, "%d.%d", major, minor);
    str = strdup(temp);
    if (release > 0) {
        snprintf(temp, BUFSIZ - 1, ".%d", release);
        prrte_asprintf(&tmp, "%s%s", str, temp);
        free(str);
        str = tmp;
    }
    if (NULL != greek) {
        prrte_asprintf(&tmp, "%s%s", str, greek);
        free(str);
        str = tmp;
    }
    if (NULL != repo) {
        prrte_asprintf(&tmp, "%s%s", str, repo);
        free(str);
        str = tmp;
    }

    if (NULL == str) {
        str = strdup(temp);
    }

    return str;
}

int main(int argc, char *argv[])
{
    int rc, i, j;
    prrte_cmd_line_t cmd_line;
    char *param, *value;
    prrte_job_t *jdata=NULL;
    prrte_app_context_t *app;

    /* Setup and parse the command line */
    memset(&myglobals, 0, sizeof(myglobals));
    /* find our basename (the name of the executable) so that we can
       use it in pretty-print error messages */
    prrte_tool_basename = prrte_basename(argv[0]);

    prrte_cmd_line_create(&cmd_line, cmd_line_init);
    prrte_mca_base_cmd_line_setup(&cmd_line);
    if (PRRTE_SUCCESS != (rc = prrte_cmd_line_parse(&cmd_line, true, false,
                                                  argc, argv)) ) {
        if (PRRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(rc));
        }
        return rc;
    }

    /* print version if requested.  Do this before check for help so
       that --version --help works as one might expect. */
    if (myglobals.version) {
        char *str;
        str = make_version_str(PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                               PRRTE_RELEASE_VERSION,
                               PRRTE_GREEK_VERSION,
                               PRRTE_REPO_REV);
        if (NULL != str) {
            fprintf(stdout, "%s %s\n\nReport bugs to %s\n",
                    prrte_tool_basename, str, PACKAGE_BUGREPORT);
            free(str);
        }
        exit(0);
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid() && !myglobals.run_as_root) {
        /* show_help is not yet available, so print an error manually */
        fprintf(stderr, "--------------------------------------------------------------------------\n");
        if (myglobals.help) {
            fprintf(stderr, "%s cannot provide the help message when run as root.\n\n", prrte_tool_basename);
        } else {
            fprintf(stderr, "%s has detected an attempt to run as root.\n\n", prrte_tool_basename);
        }

        fprintf(stderr, "Running at root is *strongly* discouraged as any mistake (e.g., in\n");
        fprintf(stderr, "defining TMPDIR) or bug can result in catastrophic damage to the OS\n");
        fprintf(stderr, "file system, leaving your system in an unusable state.\n\n");

        fprintf(stderr, "We strongly suggest that you run %s as a non-root user.\n\n", prrte_tool_basename);

        fprintf(stderr, "You can override this protection by adding the --allow-run-as-root\n");
        fprintf(stderr, "option to your command line.  However, we reiterate our strong advice\n");
        fprintf(stderr, "against doing so - please do so at your own risk.\n");
        fprintf(stderr, "--------------------------------------------------------------------------\n");
        exit(1);
    }

    /*
     * Since this process can now handle MCA/GMCA parameters, make sure to
     * process them.
     * NOTE: It is "safe" to call mca_base_cmd_line_process_args() before
     *  prrte_init_util() since mca_base_cmd_line_process_args() does *not*
     *  depend upon prrte_init_util() functionality.
     */
    if (PRRTE_SUCCESS != prrte_mca_base_cmd_line_process_args(&cmd_line, &environ, &environ)) {
        exit(1);
    }

    /* setup basic infrastructure */
    if (PRRTE_SUCCESS != (rc = prrte_init_util())) {
        /* error message will have already been output */
        return rc;
    }

    /* Check for help request */
    if (myglobals.help) {
        char *str, *args = NULL;
        char *project_name = "PMIx Reference RTE";
        args = prrte_cmd_line_get_usage_msg(&cmd_line);
        str = prrte_show_help_string("help-prrterun.txt", "prrterun:usage", false,
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

    if (myglobals.system_server) {
        /* we should act as system-level PMIx server */
        prrte_setenv(PRRTE_MCA_PREFIX"pmix_system_server", "1", true, &environ);
    }
    /* always act as session-level PMIx server */
    prrte_setenv(PRRTE_MCA_PREFIX"pmix_session_server", "1", true, &environ);
    /* if we were asked to report a uri, set the MCA param to do so */
    if (NULL != myglobals.report_uri) {
        prrte_setenv("PMIX_MCA_ptl_tcp_report_uri", myglobals.report_uri, true, &environ);
    }
    if (myglobals.remote_connections) {
        prrte_setenv("PMIX_MCA_ptl_tcp_remote_connections", "1", true, &environ);
    }
    /* don't aggregate help messages as that will apply job-to-job */
    prrte_setenv(PRRTE_MCA_PREFIX"prrte_base_help_aggregate", 0, true, &environ);

    /* Setup MCA params */
    prrte_register_params();

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars were set in the enviro prior to calling
     * prrterun
     */
    prrte_launch_environ = prrte_argv_copy(environ);

#if defined(HAVE_SETSID)
    /* see if we were directed to separate from current session */
    if (myglobals.set_sid) {
        setsid();
    }
#endif

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if(!prrte_debug_flag &&
       !prrte_debug_daemons_flag &&
       myglobals.daemonize) {
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
        prrte_show_help("help-prrterun.txt", "bad-job-object", true,
                       prrte_tool_basename);
        exit(0);
    }
    /* also should have created a daemon "app" */
    if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0))) {
        prrte_show_help("help-prrterun.txt", "bad-app-object", true,
                       prrte_tool_basename);
        exit(0);
    }

    /* Did the user specify a prefix, or want prefix by default? */
    if (prrte_cmd_line_is_taken(&cmd_line, "prefix") || want_prefix_by_default) {
        size_t param_len;
        /* if both the prefix was given and we have a prefix
         * given above, check to see if they match
         */
        if (prrte_cmd_line_is_taken(&cmd_line, "prefix") &&
            NULL != myglobals.prefix) {
            /* if they don't match, then that merits a warning */
            param = strdup(prrte_cmd_line_get_param(&cmd_line, "prefix", 0, 0));
            /* ensure we strip any trailing '/' */
            if (0 == strcmp(PRRTE_PATH_SEP, &(param[strlen(param)-1]))) {
                param[strlen(param)-1] = '\0';
            }
            value = strdup(myglobals.prefix);
            if (0 == strcmp(PRRTE_PATH_SEP, &(value[strlen(value)-1]))) {
                value[strlen(value)-1] = '\0';
            }
            if (0 != strcmp(param, value)) {
                prrte_show_help("help-prrterun.txt", "prrterun:app-prefix-conflict",
                               true, prrte_tool_basename, value, param);
                /* let the global-level prefix take precedence since we
                 * know that one is being used
                 */
                free(param);
                param = strdup(myglobals.prefix);
            }
            free(value);
        } else if (NULL != myglobals.prefix) {
            param = myglobals.prefix;
        } else if (prrte_cmd_line_is_taken(&cmd_line, "prefix")){
            /* must be --prefix alone */
            param = strdup(prrte_cmd_line_get_param(&cmd_line, "prefix", 0, 0));
        } else {
            /* --enable-prrterun-prefix-default was given to prrterun */
            param = strdup(prrte_install_dirs.prefix);
        }

        if (NULL != param) {
            /* "Parse" the param, aka remove superfluous path_sep. */
            param_len = strlen(param);
            while (0 == strcmp (PRRTE_PATH_SEP, &(param[param_len-1]))) {
                param[param_len-1] = '\0';
                param_len--;
                if (0 == param_len) {
                    prrte_show_help("help-prrterun.txt", "prrterun:empty-prefix",
                                   true, prrte_tool_basename, prrte_tool_basename);
                    return PRRTE_ERR_FATAL;
                }
            }
            prrte_set_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, PRRTE_ATTR_GLOBAL, param, PRRTE_STRING);
            free(param);
        }
    }

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    if (0 < (j = prrte_cmd_line_get_ninsts(&cmd_line, "hostfile"))) {
        if(1 < j) {
            prrte_show_help("help-prrterun.txt", "prrterun:multiple-hostfiles",
                           true, prrte_tool_basename, NULL);
            return PRRTE_ERR_FATAL;
        } else {
            value = prrte_cmd_line_get_param(&cmd_line, "hostfile", 0, 0);
            prrte_set_attribute(&app->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, value, PRRTE_STRING);
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(&cmd_line, "machinefile"))) {
        if(1 < j || prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, NULL, PRRTE_STRING)) {
            prrte_show_help("help-prrterun.txt", "prrterun:multiple-hostfiles",
                           true, prrte_tool_basename, NULL);
            return PRRTE_ERR_FATAL;
        } else {
            value = prrte_cmd_line_get_param(&cmd_line, "machinefile", 0, 0);
            prrte_set_attribute(&app->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, value, PRRTE_STRING);
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = prrte_cmd_line_get_ninsts(&cmd_line, "host"))) {
        char **targ=NULL, *tval;
        for (i = 0; i < j; ++i) {
            value = prrte_cmd_line_get_param(&cmd_line, "host", i, 0);
            prrte_argv_append_nosize(&targ, value);
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
