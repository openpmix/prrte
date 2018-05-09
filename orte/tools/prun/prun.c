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
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2018 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

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

#include "opal/event/event-internal.h"
#include "opal/mca/installdirs/installdirs.h"
#include "opal/pmix/pmix-internal.h"
#include "opal/mca/base/base.h"
#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/util/basename.h"
#include "opal/util/cmd_line.h"
#include "opal/util/opal_environ.h"
#include "opal/util/opal_getcwd.h"
#include "opal/util/show_help.h"
#include "opal/util/fd.h"
#include "opal/sys/atomic.h"

#include "opal/version.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_info_support.h"
#include "opal/runtime/opal_progress_threads.h"
#include "opal/util/os_path.h"
#include "opal/util/path.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/dss/dss.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/schizo/base/base.h"
#include "orte/mca/state/state.h"
#include "orte/orted/orted_submit.h"

/* ensure I can behave like a daemon */
#include "prun.h"

typedef struct {
    opal_object_t super;
    opal_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} myinfo_t;
static void mcon(myinfo_t *p)
{
    OPAL_PMIX_CONSTRUCT_LOCK(&p->lock);
    p->info = NULL;
    p->ninfo = 0;
}
static void mdes(myinfo_t *p)
{
    OPAL_PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
static OBJ_CLASS_INSTANCE(myinfo_t, opal_object_t,
                          mcon, mdes);

typedef struct {
    opal_list_item_t super;
    pmix_app_t app;
    opal_list_t info;
} opal_pmix_app_t;
static void acon(opal_pmix_app_t *p)
{
    OBJ_CONSTRUCT(&p->info, opal_list_t);
}
static void ades(opal_pmix_app_t *p)
{
    OPAL_LIST_DESTRUCT(&p->info);
}
static OBJ_CLASS_INSTANCE(opal_pmix_app_t,
                          opal_list_item_t,
                          acon, ades);

static struct {
    bool terminate_dvm;
    bool system_server_first;
    bool system_server_only;
    int pid;
} myoptions;

typedef struct {
    opal_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static opal_list_t job_info;
static orte_jobid_t myjobid = ORTE_JOBID_INVALID;
static myinfo_t myinfo;

static int create_app(int argc, char* argv[],
                      opal_list_t *jdata,
                      opal_pmix_app_t **app,
                      bool *made_app, char ***app_env);
static int parse_locals(opal_list_t *jdata, int argc, char* argv[]);
static void set_classpath_jar_file(opal_pmix_app_t *app, int index, char *jarfile);
static size_t evid = INT_MAX;


static opal_cmd_line_init_t cmd_line_init[] = {
    /* tell the dvm to terminate */
    { NULL, '\0', "terminate", "terminate", 0,
      &myoptions.terminate_dvm, OPAL_CMD_LINE_TYPE_BOOL,
      "Terminate the DVM", OPAL_CMD_LINE_OTYPE_DVM },

    /* look first for a system server */
    { NULL, '\0', "system-server-first", "system-server-first", 0,
      &myoptions.system_server_first, OPAL_CMD_LINE_TYPE_BOOL,
      "First look for a system server and connect to it if found", OPAL_CMD_LINE_OTYPE_DVM },

    /* connect only to a system server */
    { NULL, '\0', "system-server-only", "system-server-only", 0,
      &myoptions.system_server_only, OPAL_CMD_LINE_TYPE_BOOL,
      "Connect only to a system-level server", OPAL_CMD_LINE_OTYPE_DVM },

    /* provide a connection PID */
    { NULL, '\0', "pid", "pid", 1,
      &myoptions.pid, OPAL_CMD_LINE_TYPE_INT,
      "PID of the session-level daemon to which we should connect",
      OPAL_CMD_LINE_OTYPE_DVM },

    /* End of list */
    { NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};


static void infocb(pmix_status_t status,
                   pmix_info_t *info, size_t ninfo,
                   void *cbdata,
                   pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;
    OPAL_ACQUIRE_OBJECT(lock);

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;
    OPAL_ACQUIRE_OBJECT(lock);
    evid = ref;
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;
    OPAL_ACQUIRE_OBJECT(lock);
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

static void evhandler(size_t evhdlr_registration_id,
                      pmix_status_t status,
                      const pmix_proc_t *source,
                      pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc,
                      void *cbdata)
{
    opal_pmix_lock_t *lock = NULL;
    int jobstatus=0, rc;
    orte_jobid_t jobid = ORTE_JOBID_INVALID;
    size_t n;

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = opal_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                OPAL_PMIX_CONVERT_NSPACE(rc, &jobid, info[n].value.data.proc->nspace);
                if (ORTE_SUCCESS != rc) {
                    ORTE_ERROR_LOG(rc);
                }
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (opal_pmix_lock_t*)info[n].value.data.ptr;
            }
        }
        if (orte_cmd_options.verbose && (myjobid != ORTE_JOBID_INVALID && jobid == myjobid)) {
            opal_output(0, "JOB %s COMPLETED WITH STATUS %d",
                        ORTE_JOBID_PRINT(jobid), jobstatus);
        }
    }
    /* release the lock */
    OPAL_PMIX_WAKEUP_THREAD(lock);

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

static void setupcbfunc(pmix_status_t status,
                        pmix_info_t info[], size_t ninfo,
                        void *provided_cbdata,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    mylock_t *mylock = (mylock_t*)provided_cbdata;
    size_t n;

    if (NULL != info) {
        mylock->ninfo = ninfo;
        PMIX_INFO_CREATE(mylock->info, mylock->ninfo);
        /* cycle across the provided info */
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&mylock->info[n], &info[n]);
        }
    }

    /* release the caller */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }

    OPAL_PMIX_WAKEUP_THREAD(&mylock->lock);
}

static void launchhandler(size_t evhdlr_registration_id,
                          pmix_status_t status,
                          const pmix_proc_t *source,
                          pmix_info_t info[], size_t ninfo,
                          pmix_info_t *results, size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc,
                          void *cbdata)
{
    size_t n;

    /* the info list will include the launch directives, so
     * transfer those to the myinfo_t for return to the main thread */
    if (NULL != info) {
        myinfo.ninfo = ninfo;
        PMIX_INFO_CREATE(myinfo.info, myinfo.ninfo);
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&myinfo.info[n], &info[n]);
        }
    }

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(OPAL_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }

    /* now release the thread */
    OPAL_PMIX_WAKEUP_THREAD(&myinfo.lock);
}

int prun(int argc, char *argv[])
{
    int rc, i;
    char *param, *ptr;
    opal_pmix_lock_t lock, rellock;
    opal_list_t apps;
    opal_pmix_app_t *app;
    opal_list_t codes;
    mylock_t mylock;
    pmix_info_t info, *iptr;
    pmix_proc_t pname;
    pmix_status_t ret;
    bool flag;
    opal_ds_info_t *ds;
    size_t m, n, ninfo;
    pmix_app_t *papps;
    size_t napps;
    char nspace[PMIX_MAX_NSLEN+1];

    /* init the globals */
    memset(&orte_cmd_options, 0, sizeof(orte_cmd_options));
    memset(&myoptions, 0, sizeof(myoptions));
    OBJ_CONSTRUCT(&job_info, opal_list_t);
    OBJ_CONSTRUCT(&apps, opal_list_t);

    /* search the argv for MCA params */
    for (i=0; NULL != argv[i]; i++) {
        if (':' == argv[i][0] ||
            NULL == argv[i+1] || NULL == argv[i+2]) {
            break;
        }
        if (0 == strncmp(argv[i], "-"OPAL_MCA_CMD_LINE_ID, strlen("-"OPAL_MCA_CMD_LINE_ID)) ||
            0 == strncmp(argv[i], "--"OPAL_MCA_CMD_LINE_ID, strlen("--"OPAL_MCA_CMD_LINE_ID)) ||
            0 == strncmp(argv[i], "-g"OPAL_MCA_CMD_LINE_ID, strlen("-g"OPAL_MCA_CMD_LINE_ID)) ||
            0 == strncmp(argv[i], "--g"OPAL_MCA_CMD_LINE_ID, strlen("--g"OPAL_MCA_CMD_LINE_ID))) {
            (void) mca_base_var_env_name (argv[i+1], &param);
            opal_setenv(param, argv[i+2], true, &environ);
            free(param);
        } else if (0 == strcmp(argv[i], "-am") ||
                   0 == strcmp(argv[i], "--am")) {
            (void)mca_base_var_env_name("mca_base_param_file_prefix", &param);
            opal_setenv(param, argv[i+1], true, &environ);
            free(param);
        } else if (0 == strcmp(argv[i], "-tune") ||
                   0 == strcmp(argv[i], "--tune")) {
            (void)mca_base_var_env_name("mca_base_envar_file_prefix", &param);
            opal_setenv(param, argv[i+1], true, &environ);
            free(param);
        }
    }

    /* init only the util portion of OPAL */
    if (OPAL_SUCCESS != (rc = opal_init_util(&argc, &argv))) {
        return rc;
    }

    /* set our proc type for schizo selection */
    orte_process_info.proc_type = ORTE_PROC_TOOL;

    /* open the SCHIZO framework so we can setup the command line */
    if (ORTE_SUCCESS != (rc = mca_base_framework_open(&orte_schizo_base_framework, 0))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    if (ORTE_SUCCESS != (rc = orte_schizo_base_select())) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    /* setup our cmd line */
    orte_cmd_line = OBJ_NEW(opal_cmd_line_t);
    if (OPAL_SUCCESS != (rc = opal_cmd_line_add(orte_cmd_line, cmd_line_init))) {
        return rc;
    }

    /* setup the rest of the cmd line only once */
    if (OPAL_SUCCESS != (rc = orte_schizo.define_cli(orte_cmd_line))) {
        return rc;
    }

    /* now that options have been defined, finish setup */
    mca_base_cmd_line_setup(orte_cmd_line);

    /* parse the result to get values */
    if (OPAL_SUCCESS != (rc = opal_cmd_line_parse(orte_cmd_line,
                                                  true, false, argc, argv)) ) {
        if (OPAL_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    opal_strerror(rc));
        }
        return rc;
    }

    /* see if print version is requested. Do this before
     * check for help so that --version --help works as
     * one might expect. */
     if (orte_cmd_options.version) {
        char *str;
        str = opal_info_make_version_str("all",
                                         OPAL_MAJOR_VERSION, OPAL_MINOR_VERSION,
                                         OPAL_RELEASE_VERSION,
                                         OPAL_GREEK_VERSION,
                                         OPAL_REPO_REV);
        if (NULL != str) {
            fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n",
                    "prun", "PMIx Reference Server", str, PACKAGE_BUGREPORT);
            free(str);
        }
        exit(0);
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid() && !orte_cmd_options.run_as_root) {
        /* show_help is not yet available, so print an error manually */
        fprintf(stderr, "--------------------------------------------------------------------------\n");
        if (orte_cmd_options.help) {
            fprintf(stderr, "prun cannot provide the help message when run as root.\n\n");
        } else {
            fprintf(stderr, "prun has detected an attempt to run as root.\n\n");
        }

        fprintf(stderr, "Running as root is *strongly* discouraged as any mistake (e.g., in\n");
        fprintf(stderr, "defining TMPDIR) or bug can result in catastrophic damage to the OS\n");
        fprintf(stderr, "file system, leaving your system in an unusable state.\n\n");

        fprintf(stderr, "We strongly suggest that you run prun as a non-root user.\n\n");

        fprintf(stderr, "You can override this protection by adding the --allow-run-as-root\n");
        fprintf(stderr, "option to your command line.  However, we reiterate our strong advice\n");
        fprintf(stderr, "against doing so - please do so at your own risk.\n");
        fprintf(stderr, "--------------------------------------------------------------------------\n");
        exit(1);
    }

    /* process any mca params */
    rc = mca_base_cmd_line_process_args(orte_cmd_line, &environ, &environ);
    if (ORTE_SUCCESS != rc) {
        return rc;
    }

    /* Check for help request */
    if (orte_cmd_options.help) {
        char *str, *args = NULL;
        args = opal_cmd_line_get_usage_msg(orte_cmd_line);
        str = opal_show_help_string("help-orterun.txt", "orterun:usage", false,
                                    "prun", "PSVR", OPAL_VERSION,
                                    "prun", args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /* ensure we ONLY take the ess/tool component */
    opal_setenv(OPAL_MCA_PREFIX"ess", "tool", true, &environ);
    /* tell the ess/tool component how we want to connect */
    if (myoptions.system_server_only) {
        opal_setenv(OPAL_MCA_PREFIX"ess_tool_system_server_only", "1", true, &environ);
    }
    if (myoptions.system_server_first) {
        opal_setenv(OPAL_MCA_PREFIX"ess_tool_system_server_first", "1", true, &environ);
    }
    /* if they specified the DVM's pid, then pass it along */
    if (0 != myoptions.pid) {
        asprintf(&param, "%d", myoptions.pid);
        opal_setenv(OPAL_MCA_PREFIX"ess_tool_server_pid", param, true, &environ);
        free(param);
    }
    /* if they specified the URI, then pass it along */
    if (NULL != orte_cmd_options.hnp) {
        opal_setenv("PMIX_MCA_ptl_tcp_server_uri", orte_cmd_options.hnp, true, &environ);
    }

    /* now initialize ORTE - we have to indicate we are a launcher so that we
     * will provide rendezvous points for tools to connect to us */
    if (OPAL_SUCCESS != (rc = orte_init(&argc, &argv, ORTE_PROC_LAUNCHER))) {
        OPAL_ERROR_LOG(rc);
        return rc;
    }

    /* if the user just wants us to terminate a DVM, then do so */
    if (myoptions.terminate_dvm) {
        flag = true;
        PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_TERMINATE, &flag, PMIX_BOOL);
        fprintf(stderr, "TERMINATING DVM...");
        OPAL_PMIX_CONSTRUCT_LOCK(&lock);
        PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void*)&lock);
        OPAL_PMIX_WAIT_THREAD(&lock);
        OPAL_PMIX_DESTRUCT_LOCK(&lock);
        fprintf(stderr, "DONE\n");
        goto DONE;
    }

    /* get here if they want to run an application, so let's parse
     * the cmd line to get it */

    if (OPAL_SUCCESS != (rc = parse_locals(&apps, argc, argv))) {
        OPAL_ERROR_LOG(rc);
        OPAL_LIST_DESTRUCT(&apps);
        goto DONE;
    }

    /* bozo check */
    if (0 == opal_list_get_size(&apps)) {
        opal_output(0, "No application specified!");
        goto DONE;
    }

    /* we want to be notified upon job completion */
    ds = OBJ_NEW(opal_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    flag = true;
    PMIX_INFO_LOAD(ds->info, PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);
    opal_list_append(&job_info, &ds->super);

    /* see if they specified the personality */
    if (NULL != orte_cmd_options.personality) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_PERSONALITY, orte_cmd_options.personality, PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    }

    /* check for stdout/err directives */
    /* if we were asked to tag output, mark it so */
    if (orte_cmd_options.tag_output) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TAG_OUTPUT, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    /* if we were asked to timestamp output, mark it so */
    if (orte_cmd_options.timestamp_output) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    /* if we were asked to output to files, pass it along */
    if (NULL != orte_cmd_options.output_filename) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        /* if the given filename isn't an absolute path, then
         * convert it to one so the name will be relative to
         * the directory where prun was given as that is what
         * the user will have seen */
        if (!opal_path_is_absolute(orte_cmd_options.output_filename)) {
            char cwd[OPAL_PATH_MAX];
            getcwd(cwd, sizeof(cwd));
            ptr = opal_os_path(false, cwd, orte_cmd_options.output_filename, NULL);
        } else {
            ptr = strdup(orte_cmd_options.output_filename);
        }
        PMIX_INFO_LOAD(ds->info, PMIX_OUTPUT_TO_FILE, ptr, PMIX_STRING);
        free(ptr);
        opal_list_append(&job_info, &ds->super);
    }
    /* if we were asked to merge stderr to stdout, mark it so */
    if (orte_cmd_options.merge) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }

    /* check what user wants us to do with stdin */
    if (NULL != orte_cmd_options.stdin_target) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_STDIN_TGT, orte_cmd_options.stdin_target, PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    }

    /* if we want the argv's indexed, indicate that */
    if (orte_cmd_options.index_argv) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_INDEX_ARGV, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }

    if (NULL != orte_cmd_options.mapping_policy) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_MAPBY, orte_cmd_options.mapping_policy, PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    } else if (orte_cmd_options.pernode) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_PPR, "1:node", PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    } else if (0 < orte_cmd_options.npernode) {
        /* define the ppr */
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        (void)asprintf(&ptr, "%d:node", orte_cmd_options.npernode);
        PMIX_INFO_LOAD(ds->info, PMIX_PPR, ptr, PMIX_STRING);
        free(ptr);
        opal_list_append(&job_info, &ds->super);
    } else if (0 < orte_cmd_options.npersocket) {
        /* define the ppr */
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        (void)asprintf(&ptr, "%d:socket", orte_cmd_options.npersocket);
        PMIX_INFO_LOAD(ds->info, PMIX_PPR, ptr, PMIX_STRING);
        free(ptr);
        opal_list_append(&job_info, &ds->super);
    }

    /* if the user specified cpus/rank, set it */
    if (0 < orte_cmd_options.cpus_per_proc) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CPUS_PER_PROC, &orte_cmd_options.cpus_per_proc, PMIX_UINT32);
        opal_list_append(&job_info, &ds->super);
    }

    /* if the user specified a ranking policy, then set it */
    if (NULL != orte_cmd_options.ranking_policy) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_RANKBY, orte_cmd_options.ranking_policy, PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    }

    /* if the user specified a binding policy, then set it */
    if (NULL != orte_cmd_options.binding_policy) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_BINDTO, orte_cmd_options.binding_policy, PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    }

    /* if they asked for nolocal, mark it so */
    if (orte_cmd_options.nolocal) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_NO_PROCS_ON_HEAD, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    if (orte_cmd_options.no_oversubscribe) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_NO_OVERSUBSCRIBE, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    if (orte_cmd_options.oversubscribe) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = false;
        PMIX_INFO_LOAD(ds->info, PMIX_NO_OVERSUBSCRIBE, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    if (orte_cmd_options.report_bindings) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_REPORT_BINDINGS, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    if (NULL != orte_cmd_options.cpu_list) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CPU_LIST, orte_cmd_options.cpu_list, PMIX_STRING);
        opal_list_append(&job_info, &ds->super);
    }

    /* mark if recovery was enabled on the cmd line */
    if (orte_enable_recovery) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_JOB_RECOVERABLE, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }
    /* record the max restarts */
    if (0 < orte_max_restarts) {
        OPAL_LIST_FOREACH(app, &apps, opal_pmix_app_t) {
            ds = OBJ_NEW(opal_ds_info_t);
            PMIX_INFO_CREATE(ds->info, 1);
            PMIX_INFO_LOAD(ds->info, PMIX_MAX_RESTARTS, &orte_max_restarts, PMIX_UINT32);
            opal_list_append(&app->info, &ds->super);
        }
    }
    /* if continuous operation was specified */
    if (orte_cmd_options.continuous) {
        /* mark this job as continuously operating */
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = true;
        PMIX_INFO_LOAD(ds->info, PMIX_JOB_CONTINUOUS, &flag, PMIX_BOOL);
        opal_list_append(&job_info, &ds->super);
    }

    /* pickup any relevant envars */
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    OPAL_PMIX_CONVERT_JOBID(pname.nspace, ORTE_PROC_MY_NAME->jobid);

    OPAL_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(pname.nspace, &info, 1, setupcbfunc, &mylock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        OPAL_PMIX_DESTRUCT_LOCK(&mylock.lock);
        goto DONE;
    }
    OPAL_PMIX_WAIT_THREAD(&mylock.lock);
    OPAL_PMIX_DESTRUCT_LOCK(&mylock.lock);
    /* transfer any returned ENVARS to the job_info */
    if (NULL != mylock.info) {
        for (n=0; n < mylock.ninfo; n++) {
            if (0 == strncmp(mylock.info[n].key, PMIX_SET_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_ADD_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_UNSET_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_PREPEND_ENVAR, PMIX_MAX_KEYLEN) ||
                0 == strncmp(mylock.info[n].key, PMIX_APPEND_ENVAR, PMIX_MAX_KEYLEN)) {
                ds = OBJ_NEW(opal_ds_info_t);
                PMIX_INFO_CREATE(ds->info, 1);
                PMIX_INFO_XFER(&ds->info[0], &mylock.info[n]);
                opal_list_append(&job_info, &ds->super);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }

    /* if we were launched by a tool wanting to direct our
     * operation, then we need to pause here and give it
     * a chance to tell us what we need to do */
    if (NULL != (param = getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL"))) {
        /* register for the PMIX_LAUNCH_DIRECTIVE event */
        OPAL_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIX_LAUNCH_DIRECTIVE;
        /* setup the myinfo object to capture the returned
         * values - must do so prior to registering in case
         * the event has already arrived */
        OBJ_CONSTRUCT(&myinfo, myinfo_t);
        /* go ahead and register */
        PMIx_Register_event_handler(&ret, 1, NULL, 0, launchhandler, regcbfunc, &lock);
        OPAL_PMIX_WAIT_THREAD(&lock);
        OPAL_PMIX_DESTRUCT_LOCK(&lock);
        OPAL_LIST_DESTRUCT(&codes);
        /* notify the tool that we are ready */
        ptr = strdup(param);
        param = strchr(ptr, ':');
        *param = '\0';
        ++param;
        (void)strncpy(pname.nspace, ptr, PMIX_MAX_NSLEN);
        pname.rank = strtoul(param, NULL, 10);
        PMIx_Notify_event(PMIX_LAUNCHER_READY, &pname, PMIX_RANGE_SESSION, NULL, 0, NULL, NULL);
        /* now wait for the launch directives to arrive */
        OPAL_PMIX_WAIT_THREAD(&myinfo.lock);
        /* process the returned directives */
        if (NULL != myinfo.info) {
            for (n=0; n < myinfo.ninfo; n++) {
                if (0 == strncmp(myinfo.info[n].key, PMIX_DEBUG_JOB_DIRECTIVES, PMIX_MAX_KEYLEN)) {
                    /* there will be a pmix_data_array containing the directives */
                    iptr = (pmix_info_t*)myinfo.info[n].value.data.darray->array;
                    ninfo = myinfo.info[n].value.data.darray->size;
                    for (m=0; m < ninfo; m++) {
                        ds = OBJ_NEW(opal_ds_info_t);
                        ds->info = &iptr[n];
                        opal_list_append(&job_info, &ds->super);
                    }
                    free(myinfo.info[n].value.data.darray);  // protect the info structs
                } else if (0 == strncmp(myinfo.info[n].key, PMIX_DEBUG_APP_DIRECTIVES, PMIX_MAX_KEYLEN)) {
                    /* there will be a pmix_data_array containing the directives */
                    iptr = (pmix_info_t*)myinfo.info[n].value.data.darray->array;
                    ninfo = myinfo.info[n].value.data.darray->size;
                    for (m=0; m < ninfo; m++) {
                        OPAL_LIST_FOREACH(app, &apps, opal_pmix_app_t) {
                            /* the value can only be on one list at a time, so replicate it */
                            ds = OBJ_NEW(opal_ds_info_t);
                            ds->info = &iptr[n];
                            opal_list_append(&app->info, &ds->super);
                        }
                    }
                    free(myinfo.info[n].value.data.darray);  // protect the info structs
                }
            }
        }
    }
    /* convert the job info into an array */
    ninfo = opal_list_get_size(&job_info);
    iptr = NULL;
    if (0 < ninfo) {
        PMIX_INFO_CREATE(iptr, ninfo);
        n=0;
        OPAL_LIST_FOREACH(ds, &job_info, opal_ds_info_t) {
            PMIX_INFO_XFER(&iptr[n], ds->info);
            ++n;
        }
    }
    /* convert the apps to an array */
    napps = opal_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);
    OPAL_LIST_FOREACH(app, &apps, opal_pmix_app_t) {
        papps[n] = app->app;
        ++n;
    }
    ret = PMIx_Spawn(iptr, ninfo, papps, napps, nspace);
    if (PMIX_SUCCESS != ret) {
        opal_output(0, "Job failed to spawn: %s", PMIx_Error_string(ret));
        goto DONE;
    }

    /* register to be notified when
     * our job completes */
    ret = PMIX_ERR_JOB_TERMINATED;
    /* setup the info */
    ninfo = 3;
    PMIX_INFO_CREATE(iptr, ninfo);
    /* give the handler a name */
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "JOB_TERMINATION_EVENT", PMIX_STRING);
    /* specify we only want to be notified when our
     * job terminates */
    (void)strncpy(pname.nspace, nspace, PMIX_MAX_NSLEN);
    pname.rank = PMIX_RANK_WILDCARD;
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    /* request that they return our lock object */
    OPAL_PMIX_CONSTRUCT_LOCK(&rellock);
    PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    /* do the registration */
    OPAL_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(&ret, 1, iptr, ninfo, evhandler, regcbfunc, &lock);
    OPAL_PMIX_WAIT_THREAD(&lock);
    OPAL_PMIX_DESTRUCT_LOCK(&lock);

    if (orte_cmd_options.verbose) {
        opal_output(0, "JOB %s EXECUTING", OPAL_JOBID_PRINT(myjobid));
    }

    OPAL_PMIX_WAIT_THREAD(&rellock);
    OPAL_PMIX_DESTRUCT_LOCK(&rellock);

    OPAL_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(evid, opcbfunc, &lock);
    OPAL_PMIX_WAIT_THREAD(&lock);
    OPAL_PMIX_DESTRUCT_LOCK(&lock);

  DONE:
    /* cleanup and leave */
    orte_finalize();
    return 0;
}

static int parse_locals(opal_list_t *jdata, int argc, char* argv[])
{
    int i, rc;
    int temp_argc;
    char **temp_argv, **env;
    opal_pmix_app_t *app;
    bool made_app;

    /* Make the apps */
    temp_argc = 0;
    temp_argv = NULL;
    opal_argv_append(&temp_argc, &temp_argv, argv[0]);

    /* NOTE: This bogus env variable is necessary in the calls to
       create_app(), below.  See comment immediately before the
       create_app() function for an explanation. */

    env = NULL;
    for (i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (opal_argv_count(temp_argv) > 1) {
                if (NULL != env) {
                    opal_argv_free(env);
                    env = NULL;
                }
                app = NULL;
                rc = create_app(temp_argc, temp_argv, jdata, &app, &made_app, &env);
                if (OPAL_SUCCESS != rc) {
                    /* Assume that the error message has already been
                       printed; no need to cleanup -- we can just
                       exit */
                    exit(1);
                }
                if (made_app) {
                    opal_list_append(jdata, &app->super);
                }

                /* Reset the temps */

                temp_argc = 0;
                temp_argv = NULL;
                opal_argv_append(&temp_argc, &temp_argv, argv[0]);
            }
        } else {
            opal_argv_append(&temp_argc, &temp_argv, argv[i]);
        }
    }

    if (opal_argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(temp_argc, temp_argv, jdata, &app, &made_app, &env);
        if (ORTE_SUCCESS != rc) {
            /* Assume that the error message has already been printed;
               no need to cleanup -- we can just exit */
            exit(1);
        }
        if (made_app) {
            opal_list_append(jdata, &app->super);
        }
    }
    if (NULL != env) {
        opal_argv_free(env);
    }
    opal_argv_free(temp_argv);

    /* All done */

    return ORTE_SUCCESS;
}


/*
 * This function takes a "char ***app_env" parameter to handle the
 * specific case:
 *
 *   orterun --mca foo bar -app appfile
 *
 * That is, we'll need to keep foo=bar, but the presence of the app
 * file will cause an invocation of parse_appfile(), which will cause
 * one or more recursive calls back to create_app().  Since the
 * foo=bar value applies globally to all apps in the appfile, we need
 * to pass in the "base" environment (that contains the foo=bar value)
 * when we parse each line in the appfile.
 *
 * This is really just a special case -- when we have a simple case like:
 *
 *   orterun --mca foo bar -np 4 hostname
 *
 * Then the upper-level function (parse_locals()) calls create_app()
 * with a NULL value for app_env, meaning that there is no "base"
 * environment that the app needs to be created from.
 */
static int create_app(int argc, char* argv[],
                      opal_list_t *jdata,
                      opal_pmix_app_t **app_ptr,
                      bool *made_app, char ***app_env)
{
    char cwd[OPAL_PATH_MAX];
    int i, j, count, rc;
    char *param, *value;
    opal_pmix_app_t *app = NULL;
    bool found = false;
    char *appname = NULL;
    opal_ds_info_t *val;

    *made_app = false;

    /* parse the cmd line - do this every time thru so we can
     * repopulate the globals */
    if (OPAL_SUCCESS != (rc = opal_cmd_line_parse(orte_cmd_line, true, false,
                                                  argc, argv)) ) {
        if (OPAL_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    opal_strerror(rc));
        }
        return rc;
    }

    /* Setup application context */
    app = OBJ_NEW(opal_pmix_app_t);
    opal_cmd_line_get_tail(orte_cmd_line, &count, &app->app.argv);

    /* See if we have anything left */
    if (0 == count) {
        opal_show_help("help-orterun.txt", "orterun:executable-not-specified",
                       true, "prun", "prun");
        rc = OPAL_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* Grab all MCA environment variables */
    app->app.env = opal_argv_copy(*app_env);
    for (i=0; NULL != environ[i]; i++) {
        if (0 == strncmp("PMIX_", environ[i], 5) ||
            0 == strncmp("OMPI_", environ[i], 5)) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(environ[i]);
            value = strchr(param, '=');
            *value = '\0';
            value++;
            opal_setenv(param, value, false, &app->app.env);
            free(param);
        }
    }

    /* set necessary env variables for external usage from tune conf file*/
    int set_from_file = 0;
    char **vars = NULL;
    if (OPAL_SUCCESS == mca_base_var_process_env_list_from_file(&vars) &&
            NULL != vars) {
        for (i=0; NULL != vars[i]; i++) {
            value = strchr(vars[i], '=');
            /* terminate the name of the param */
            *value = '\0';
            /* step over the equals */
            value++;
            /* overwrite any prior entry */
            opal_setenv(vars[i], value, true, &app->app.env);
            /* save it for any comm_spawn'd apps */
            opal_setenv(vars[i], value, true, &orte_forwarded_envars);
        }
        set_from_file = 1;
        opal_argv_free(vars);
    }
    /* Did the user request to export any environment variables on the cmd line? */
    char *env_set_flag;
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (opal_cmd_line_is_taken(orte_cmd_line, "x")) {
        if (NULL != env_set_flag) {
            opal_show_help("help-orterun.txt", "orterun:conflict-env-set", false);
            return ORTE_ERR_FATAL;
        }
        j = opal_cmd_line_get_ninsts(orte_cmd_line, "x");
        for (i = 0; i < j; ++i) {
            param = opal_cmd_line_get_param(orte_cmd_line, "x", i, 0);

            if (NULL != (value = strchr(param, '='))) {
                /* terminate the name of the param */
                *value = '\0';
                /* step over the equals */
                value++;
                /* overwrite any prior entry */
                opal_setenv(param, value, true, &app->app.env);
                /* save it for any comm_spawn'd apps */
                opal_setenv(param, value, true, &orte_forwarded_envars);
            } else {
                value = getenv(param);
                if (NULL != value) {
                    /* overwrite any prior entry */
                    opal_setenv(param, value, true, &app->app.env);
                    /* save it for any comm_spawn'd apps */
                    opal_setenv(param, value, true, &orte_forwarded_envars);
                } else {
                    opal_output(0, "Warning: could not find environment variable \"%s\"\n", param);
                }
            }
        }
    } else if (NULL != env_set_flag) {
        /* if mca_base_env_list was set, check if some of env vars were set via -x from a conf file.
         * If this is the case, error out.
         */
        if (!set_from_file) {
            /* set necessary env variables for external usage */
            vars = NULL;
            if (OPAL_SUCCESS == mca_base_var_process_env_list(env_set_flag, &vars) &&
                    NULL != vars) {
                for (i=0; NULL != vars[i]; i++) {
                    value = strchr(vars[i], '=');
                    /* terminate the name of the param */
                    *value = '\0';
                    /* step over the equals */
                    value++;
                    /* overwrite any prior entry */
                    opal_setenv(vars[i], value, true, &app->app.env);
                    /* save it for any comm_spawn'd apps */
                    opal_setenv(vars[i], value, true, &orte_forwarded_envars);
                }
                opal_argv_free(vars);
            }
        } else {
            opal_show_help("help-orterun.txt", "orterun:conflict-env-set", false);
            return ORTE_ERR_FATAL;
        }
    }

    /* Did the user request a specific wdir? */

    if (NULL != orte_cmd_options.wdir) {
        /* if this is a relative path, convert it to an absolute path */
        if (opal_path_is_absolute(orte_cmd_options.wdir)) {
            app->app.cwd = strdup(orte_cmd_options.wdir);
        } else {
            /* get the cwd */
            if (OPAL_SUCCESS != (rc = opal_getcwd(cwd, sizeof(cwd)))) {
                opal_show_help("help-orterun.txt", "orterun:init-failure",
                               true, "get the cwd", rc);
                goto cleanup;
            }
            /* construct the absolute path */
            app->app.cwd = opal_os_path(false, cwd, orte_cmd_options.wdir, NULL);
        }
    } else if (orte_cmd_options.set_cwd_to_session_dir) {
        val = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
        opal_list_append(&app->info, &val->super);
    } else {
        if (OPAL_SUCCESS != (rc = opal_getcwd(cwd, sizeof(cwd)))) {
            opal_show_help("help-orterun.txt", "orterun:init-failure",
                           true, "get the cwd", rc);
            goto cleanup;
        }
        app->app.cwd = strdup(cwd);
    }

    /* Did the user specify a hostfile. Need to check for both
     * hostfile and machine file.
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    found = false;
    if (0 < (j = opal_cmd_line_get_ninsts(orte_cmd_line, "hostfile"))) {
        if (1 < j) {
            opal_show_help("help-orterun.txt", "orterun:multiple-hostfiles",
                           true, "prun", NULL);
            return ORTE_ERR_FATAL;
        } else {
            value = opal_cmd_line_get_param(orte_cmd_line, "hostfile", 0, 0);
            val = OBJ_NEW(opal_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_HOSTFILE, value, PMIX_STRING);
            free(value);
            opal_list_append(&app->info, &val->super);
            found = true;
        }
    }
    if (0 < (j = opal_cmd_line_get_ninsts(orte_cmd_line, "machinefile"))) {
        if (1 < j || found) {
            opal_show_help("help-orterun.txt", "orterun:multiple-hostfiles",
                           true, "prun", NULL);
            return ORTE_ERR_FATAL;
        } else {
            value = opal_cmd_line_get_param(orte_cmd_line, "machinefile", 0, 0);
            val = OBJ_NEW(opal_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_HOSTFILE, value, PMIX_STRING);
            free(value);
            opal_list_append(&app->info, &val->super);
        }
    }

    /* Did the user specify any hosts? */
    if (0 < (j = opal_cmd_line_get_ninsts(orte_cmd_line, "host"))) {
        char **targ=NULL, *tval;
        for (i = 0; i < j; ++i) {
            value = opal_cmd_line_get_param(orte_cmd_line, "host", i, 0);
            opal_argv_append_nosize(&targ, value);
        }
        tval = opal_argv_join(targ, ',');
        val = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_HOST, tval, PMIX_STRING);
        free(tval);
        opal_list_append(&app->info, &val->super);
    }

    /* check for bozo error */
    if (0 > orte_cmd_options.num_procs) {
        opal_show_help("help-orterun.txt", "orterun:negative-nprocs",
                       true, "prun", app->app.argv[0],
                       orte_cmd_options.num_procs, NULL);
        return ORTE_ERR_FATAL;
    }

    app->app.maxprocs = orte_cmd_options.num_procs;

    /* see if we need to preload the binary to
     * find the app - don't do this for java apps, however, as we
     * can't easily find the class on the cmd line. Java apps have to
     * preload their binary via the preload_files option
     */
    if (NULL == strstr(app->app.argv[0], "java")) {
        if (orte_cmd_options.preload_binaries) {
            val = OBJ_NEW(opal_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_SET_SESSION_CWD, NULL, PMIX_BOOL);
            opal_list_append(&app->info, &val->super);
            val = OBJ_NEW(opal_ds_info_t);
            PMIX_INFO_CREATE(val->info, 1);
            PMIX_INFO_LOAD(val->info, PMIX_PRELOAD_BIN, NULL, PMIX_BOOL);
            opal_list_append(&app->info, &val->super);
        }
    }
    if (NULL != orte_cmd_options.preload_files) {
        val = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(val->info, 1);
        PMIX_INFO_LOAD(val->info, PMIX_PRELOAD_FILES, NULL, PMIX_BOOL);
        opal_list_append(&app->info, &val->super);
    }

    /* Do not try to find argv[0] here -- the starter is responsible
       for that because it may not be relevant to try to find it on
       the node where orterun is executing.  So just strdup() argv[0]
       into app. */

    app->app.cmd = strdup(app->app.argv[0]);
    if (NULL == app->app.cmd) {
        opal_show_help("help-orterun.txt", "orterun:call-failed",
                       true, "prun", "library", "strdup returned NULL", errno);
        rc = ORTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* if this is a Java application, we have a bit more work to do. Such
     * applications actually need to be run under the Java virtual machine
     * and the "java" command will start the "executable". So we need to ensure
     * that all the proper java-specific paths are provided
     */
    appname = opal_basename(app->app.cmd);
    if (0 == strcmp(appname, "java")) {
        /* see if we were given a library path */
        found = false;
        for (i=1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "java.library.path")) {
                char *dptr;
                /* find the '=' that delineates the option from the path */
                if (NULL == (dptr = strchr(app->app.argv[i], '='))) {
                    /* that's just wrong */
                    rc = ORTE_ERR_BAD_PARAM;
                    goto cleanup;
                }
                /* step over the '=' */
                ++dptr;
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                if (NULL == strstr(app->app.argv[i], opal_install_dirs.libdir)) {
                    /* doesn't appear to - add it to be safe */
                    if (':' == app->app.argv[i][strlen(app->app.argv[i]-1)]) {
                        asprintf(&value, "-Djava.library.path=%s%s", dptr, opal_install_dirs.libdir);
                    } else {
                        asprintf(&value, "-Djava.library.path=%s:%s", dptr, opal_install_dirs.libdir);
                    }
                    free(app->app.argv[i]);
                    app->app.argv[i] = value;
                }
                break;
            }
        }
        if (!found) {
            /* need to add it right after the java command */
            asprintf(&value, "-Djava.library.path=%s", opal_install_dirs.libdir);
            opal_argv_insert_element(&app->app.argv, 1, value);
            free(value);
        }

        /* see if we were given a class path */
        found = false;
        for (i=1; NULL != app->app.argv[i]; i++) {
            if (NULL != strstr(app->app.argv[i], "cp") ||
                NULL != strstr(app->app.argv[i], "classpath")) {
                /* yep - but does it include the path to the mpi libs? */
                found = true;
                /* check if mpi.jar exists - if so, add it */
                value = opal_os_path(false, opal_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    set_classpath_jar_file(app, i+1, "mpi.jar");
                }
                free(value);
                /* check for oshmem support */
                value = opal_os_path(false, opal_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    set_classpath_jar_file(app, i+1, "shmem.jar");
                }
                free(value);
                /* always add the local directory */
                asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[i+1]);
                free(app->app.argv[i+1]);
                app->app.argv[i+1] = value;
                break;
            }
        }
        if (!found) {
            /* check to see if CLASSPATH is in the environment */
            found = false;  // just to be pedantic
            for (i=0; NULL != environ[i]; i++) {
                if (0 == strncmp(environ[i], "CLASSPATH", strlen("CLASSPATH"))) {
                    value = strchr(environ[i], '=');
                    ++value; /* step over the = */
                    opal_argv_insert_element(&app->app.argv, 1, value);
                    /* check for mpi.jar */
                    value = opal_os_path(false, opal_install_dirs.libdir, "mpi.jar", NULL);
                    if (access(value, F_OK ) != -1) {
                        set_classpath_jar_file(app, 1, "mpi.jar");
                    }
                    free(value);
                    /* check for shmem.jar */
                    value = opal_os_path(false, opal_install_dirs.libdir, "shmem.jar", NULL);
                    if (access(value, F_OK ) != -1) {
                        set_classpath_jar_file(app, 1, "shmem.jar");
                    }
                    free(value);
                    /* always add the local directory */
                    (void)asprintf(&value, "%s:%s", app->app.cwd, app->app.argv[1]);
                    free(app->app.argv[1]);
                    app->app.argv[1] = value;
                    opal_argv_insert_element(&app->app.argv, 1, "-cp");
                    found = true;
                    break;
                }
            }
            if (!found) {
                /* need to add it right after the java command - have
                 * to include the working directory and trust that
                 * the user set cwd if necessary
                 */
                char *str, *str2;
                /* always start with the working directory */
                str = strdup(app->app.cwd);
                /* check for mpi.jar */
                value = opal_os_path(false, opal_install_dirs.libdir, "mpi.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    (void)asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                /* check for shmem.jar */
                value = opal_os_path(false, opal_install_dirs.libdir, "shmem.jar", NULL);
                if (access(value, F_OK ) != -1) {
                    asprintf(&str2, "%s:%s", str, value);
                    free(str);
                    str = str2;
                }
                free(value);
                opal_argv_insert_element(&app->app.argv, 1, str);
                free(str);
                opal_argv_insert_element(&app->app.argv, 1, "-cp");
            }
        }
        /* try to find the actual command - may not be perfect */
        for (i=1; i < opal_argv_count(app->app.argv); i++) {
            if (NULL != strstr(app->app.argv[i], "java.library.path")) {
                continue;
            } else if (NULL != strstr(app->app.argv[i], "cp") ||
                       NULL != strstr(app->app.argv[i], "classpath")) {
                /* skip the next field */
                i++;
                continue;
            }
            /* declare this the winner */
            opal_setenv("OMPI_COMMAND", app->app.argv[i], true, &app->app.env);
            /* collect everything else as the cmd line */
            if ((i+1) < opal_argv_count(app->app.argv)) {
                value = opal_argv_join(&app->app.argv[i+1], ' ');
                opal_setenv("OMPI_ARGV", value, true, &app->app.env);
                free(value);
            }
            break;
        }
    } else {
        /* add the cmd to the environment for MPI_Info to pickup */
        opal_setenv("OMPI_COMMAND", appname, true, &app->app.env);
        if (1 < opal_argv_count(app->app.argv)) {
            value = opal_argv_join(&app->app.argv[1], ' ');
            opal_setenv("OMPI_ARGV", value, true, &app->app.env);
            free(value);
        }
    }

    *app_ptr = app;
    app = NULL;
    *made_app = true;

    /* All done */

 cleanup:
    if (NULL != app) {
        OBJ_RELEASE(app);
    }
    if (NULL != appname) {
        free(appname);
    }
    return rc;
}

static void set_classpath_jar_file(opal_pmix_app_t *app, int index, char *jarfile)
{
    if (NULL == strstr(app->app.argv[index], jarfile)) {
        /* nope - need to add it */
        char *fmt = ':' == app->app.argv[index][strlen(app->app.argv[index]-1)]
                    ? "%s%s/%s" : "%s:%s/%s";
        char *str;
        asprintf(&str, fmt, app->app.argv[index], opal_install_dirs.libdir, jarfile);
        free(app->app.argv[index]);
        app->app.argv[index] = str;
    }
}
