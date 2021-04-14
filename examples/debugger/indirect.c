/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <getopt.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "debugger.h"
#include <pmix_tool.h>

static pmix_proc_t myproc;
static volatile bool ilactive = true;
static volatile bool dbactive = true;
static volatile char *appnspace = NULL;
static volatile bool regpending = true;

/* this is a callback function for the PMIx_Query
 * API. The query will callback with a status indicating
 * if the request could be fully satisfied, partially
 * satisfied, or completely failed. The info parameter
 * contains an array of the returned data, with the
 * info->key field being the key that was provided in
 * the query call. Thus, you can correlate the returned
 * data in the info->value field to the requested key.
 *
 * Once we have dealt with the returned data, we must
 * call the release_fn so that the PMIx library can
 * cleanup */
static void cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                   pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t *) cbdata;
    size_t n;

    printf("%s called with status %s\n", __FUNCTION__, PMIx_Error_string(status));
    mq->status = status;
    /* save the returned info - the PMIx library "owns" it
     * and will release it and perform other cleanup actions
     * when release_fn is called */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n = 0; n < ninfo; n++) {
            printf("Key %s Type %s(%d)\n", info[n].key, PMIx_Data_type_string(info[n].value.type),
                   info[n].value.type);
            PMIX_INFO_XFER(&mq->info[n], &info[n]);
        }
    }

    /* let the library release the data and cleanup from
     * the operation */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* release the block */
    DEBUG_WAKEUP_THREAD(&mq->lock);
}

/* this is the event notification function we pass down below
 * when registering for general events - i.e.,, the default
 * handler. We don't technically need to register one, but it
 * is usually good practice to catch any events that occur */
static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    printf("DEFAULT EVENT HANDLER CALLED WITH STATUS %s\n", PMIx_Error_string(status));

    /* this example doesn't do anything with default events */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    ilactive = false;
    printf("\tCOMPLETE\n");
}

/* this is the event notification function we pass down below
 * when registering for LOST_CONNECTION, thereby indicating
 * that the intermediate launcher we started has terminated */
static void terminate_fn(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    printf("%s called with status %s\n", __FUNCTION__, PMIx_Error_string(status));
    /* this example doesn't do anything further */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    ilactive = false;
}

/* event handler registration is done asynchronously because it
 * may involve the PMIx server registering with the host RM for
 * external events. So we provide a callback function that returns
 * the status of the request (success or an error), plus a numerical index
 * to the registered event. The index is used later on to deregister
 * an event handler - if we don't explicitly deregister it, then the
 * PMIx server will do so when it see us exit */
static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    printf("%s called with status %s\n", __FUNCTION__, PMIx_Error_string(status));
    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status, (unsigned long) evhandler_ref);
    }
    lock->status = status;
    regpending = false;
    DEBUG_WAKEUP_THREAD(lock);
}

static void spawn_cbfunc(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NSPACE)) {
            appnspace = strdup(info[n].value.data.string);
            printf("GOT NSPACE %s\n", appnspace);
            break;
        }
    }
    printf("Debugger daemon job: %s\n", appnspace);
    dbactive = false;

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void cncbfunc(pmix_status_t status, void *cbdata)
{
    printf("%s called with status %s\n", __FUNCTION__, PMIx_Error_string(status));
    mylock_t *mylock = (mylock_t *) cbdata;
    mylock->status = status;
    DEBUG_WAKEUP_THREAD(mylock);
}

#define DBGR_LOOP_LIMIT 10
static int help = 0;
static int mycmd = 0;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info, *iptr;
    pmix_app_t *app;
    size_t ninfo, napps;
    char *nspace = NULL;
    char *requested_launcher;
    int i;
    pmix_query_t *query;
    size_t nq, n;
    myquery_data_t myquery_data;
    bool cospawn = false, stop_on_exec = false, cospawn_reqd = false;
    char cwd[1024];
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    mylock_t mylock;
    myrel_t launcher_ready, dbrel;
    pid_t pid;
    pmix_envar_t envar;
    char *launchers[] = {"prun", "mpirun", "mpiexec", "prterun", NULL};
    pmix_proc_t proc, target_proc;
    bool found;
    pmix_data_array_t darray, d2;
    char *tmp;
    pmix_nspace_t clientspace, dbnspace;
    pmix_value_t *val;
    char *myuri = NULL;
    void *jinfo, *linfo, *dirs;
    myquery_data_t *mydata = NULL;
    pmix_rank_t rank;

    /* need to provide args */
    if (2 > argc) {
        fprintf(stderr, "Usage: %s [launcher] [app]\n", argv[0]);
        exit(0);
    }

    /* check to see if we are using an intermediate launcher - we only
     * support those we recognize */
    found = false;
    requested_launcher = basename(argv[1]);
    for (n = 0; NULL != launchers[n]; n++) {
        if (0 == strcmp(requested_launcher, launchers[n])) {
            found = true;
        }
    }
    if (!found) {
        fprintf(stderr, "Wrong test, dude\n");
        exit(1);
    }

    pid = getpid();

    info = NULL;
    ninfo = 0;

    /* do not connect to anyone */
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    PMIX_INFO_LOAD(&info[n], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    ++n;
    PMIX_INFO_LOAD(&info[n], PMIX_LAUNCHER, NULL, PMIX_BOOL);

    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n", PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    printf("Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank,
           (unsigned long) pid);

    /* get our URI as we will need it later */
    rc = PMIx_Get(&myproc, PMIX_SERVER_URI, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Failed to retrieve our URI: %s\n", PMIx_Error_string(rc));
        PMIx_tool_finalize();
        exit(rc);
    }
    myuri = strdup(val->data.string);
    PMIX_VALUE_RELEASE(val);
    printf("DEBUGGER URI: %s\n", myuri);

    /* register an event handler to pickup when the IL
     * we spawned dies */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    code = PMIX_ERR_LOST_CONNECTION;
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "LOST-CONNECTION", PMIX_STRING);
    PMIx_Register_event_handler(&code, 1, info, 1, terminate_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 1);

    /* register a default event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "DEFAULT", PMIX_STRING);
    PMIx_Register_event_handler(NULL, 0, info, 1, notification_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 1);

    /* we are using an intermediate launcher - we will either use the
     * reference server to start it or will fork/exec it ourselves,
     * but either way tell it to wait after launch for directives */
    napps = 1;
    PMIX_APP_CREATE(app, napps);
    /* setup the executable */
    app[0].cmd = strdup(argv[1]);
    PMIX_ARGV_APPEND(rc, app[0].argv, argv[1]);
    /* pass it the rest of the cmd line as we don't know
     * how to parse it */
    for (n = 2; n < argc; n++) {
        PMIX_ARGV_APPEND(rc, app[0].argv, argv[n]);
    }
    getcwd(cwd, 1024); // point us to our current directory
    app[0].cwd = strdup(cwd);
    app[0].maxprocs = 1; // only start one instance of the IL

    /* tell the IL how to connect back to us */
    PMIX_SETENV(rc, PMIX_LAUNCHER_RNDZ_URI, myuri, &app[0].env);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Failed to set URI in app environment: %s\n", PMIx_Error_string(rc));
        PMIx_tool_finalize();
        exit(rc);
    }

    /* provide job-level directives so the launcher does what we want
     * when it spawns the actual job - note that requesting the stdout
     * and stderr of the launcher will automatically get us the output
     * from the application as the launcher will have had it forwarded
     * to itself */
    PMIX_INFO_LIST_START(jinfo);
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_NOTIFY_JOB_EVENTS, NULL, PMIX_BOOL);
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_FWD_STDOUT, NULL, PMIX_BOOL); // forward stdout to me
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_FWD_STDERR, NULL, PMIX_BOOL); // forward stderr to me
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_SPAWN_TOOL, NULL, PMIX_BOOL); // we are spawning a tool
    /* create the launch directives to tell the launcher what
     * to do with the app it is going to spawn for us */
    PMIX_INFO_LIST_START(linfo);
    rank = PMIX_RANK_WILDCARD;
    PMIX_INFO_LIST_ADD(rc, linfo, PMIX_DEBUG_STOP_IN_INIT, &rank, PMIX_PROC_RANK);  // stop all procs in init
    PMIX_INFO_LIST_CONVERT(rc, linfo, &darray);
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_LAUNCH_DIRECTIVES, &darray, PMIX_DATA_ARRAY);
    PMIX_INFO_LIST_RELEASE(linfo);

    /* convert job info to array */
    PMIX_INFO_LIST_CONVERT(rc, jinfo, &darray);
    PMIX_INFO_LIST_RELEASE(jinfo);
    info = (pmix_info_t *) darray.array;
    ninfo = darray.size;

    /* spawn the launcher - the function will return when the launcher
     * has been started. */
    printf("SPAWNING LAUNCHER\n");
    rc = PMIx_Spawn(info, ninfo, app, napps, clientspace);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    PMIX_APP_FREE(app, napps);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Launcher %s failed to start with error: %s(%d)\n", argv[1],
                PMIx_Error_string(rc), rc);
        goto done;
    }

    printf("RECONNECT TO IL AT %s\n", clientspace);
    /* set the spawned launcher as our primary server - wait for
     * it to connect to us but provide a timeout so we don't hang
     * waiting forever. The launcher shall connect to us prior
     * to spawning the job we provided it */
    PMIX_LOAD_PROCID(&proc, clientspace, 0);
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    PMIX_INFO_LOAD(&info[n], PMIX_WAIT_FOR_CONNECTION, NULL, PMIX_BOOL);
    ++n;
    i = 2;
    PMIX_INFO_LOAD(&info[n], PMIX_TIMEOUT, &i, PMIX_INT);
    rc = PMIx_tool_set_server(&proc, info, ninfo);
    if (PMIX_SUCCESS != rc) {
        /* connection failed */
        fprintf(stderr, "Failed to set spawned launcher as primary server: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }
    PMIX_INFO_FREE(info, ninfo);

    /* register to receive the ready-for-debug event alerting us that things are ready
     * for us to spawn the debugger daemons - this will be registered
     * with the IL we started */
    printf("REGISTERING READY-FOR-DEBUG HANDLER\n");
    DEBUG_CONSTRUCT_LOCK(&mylock);
    code = PMIX_READY_FOR_DEBUG;
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "READY-FOR-DEBUG", PMIX_STRING);
    PMIX_LOAD_PROCID(&proc, (char*)appnspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);
    PMIx_Register_event_handler(&code, 1, info, 2, spawn_cbfunc, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 2);
    if (!ilactive) {
        fprintf(stderr, "Error: Launcher not active\n");
        goto done;
    }

    printf("RELEASING %s [%s:%d]\n", argv[1], proc.nspace, proc.rank);
    /* release the IL to spawn its job */
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    /* target this notification solely to that one tool */
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);
    // PMIX_INFO_LOAD(&info[2], PMIX_EVENT_DO_NOT_CACHE, NULL, PMIX_BOOL);
    PMIx_Notify_event(PMIX_DEBUGGER_RELEASE, &myproc, PMIX_RANGE_CUSTOM, info, 2, NULL, NULL);
    PMIX_INFO_FREE(info, 2);
    printf("WAITING FOR APPLICATION LAUNCH\n");
    /* wait for the IL to have launched its application */
    int icount = 0;
    while (dbactive && ilactive) {
        struct timespec tp = {0, 500000000};
        nanosleep(&tp, NULL);
        ++icount;
        if (icount > 10) {
            fprintf(stderr, "Error: Failed to launch by the timeout\n");
            goto done;
        }
    }
    if (!ilactive) {
        /* the launcher failed */
        fprintf(stderr, "Error: Launcher failed\n");
        goto done;
    }

    if (NULL == appnspace) {
        fprintf(stderr, "Error: The application has failed to launch\n");
        goto done;
    }
    printf("APPLICATION HAS LAUNCHED: %s\n", (char *) appnspace);

    /* setup the debugger */
    mydata = (myquery_data_t *) malloc(sizeof(myquery_data_t));
    mydata->napps = 1;
    PMIX_APP_CREATE(mydata->apps, mydata->napps);
    mydata->apps[0].cmd = strdup("./daemon");
    PMIX_ARGV_APPEND(rc, mydata->apps[0].argv, "./daemon");
    getcwd(cwd, 1024); // point us to our current directory
    mydata->apps[0].cwd = strdup(cwd);
    mydata->apps[0].maxprocs = 1;
    PMIX_LOAD_PROCID(&target_proc, (void *) appnspace, PMIX_RANK_WILDCARD);
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    PMIX_INFO_LIST_START(dirs);
    PMIX_INFO_LIST_ADD(rc, dirs, PMIX_MAPBY, "ppr:1:node",
                       PMIX_STRING); // instruct the RM to launch one copy of the executable on each
                                     // node
    PMIX_INFO_LIST_ADD(rc, dirs, PMIX_DEBUGGER_DAEMONS, NULL,
                       PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LIST_ADD(rc, dirs, PMIX_DEBUG_TARGET, &target_proc,
                       PMIX_PROC); // the nspace being debugged
    PMIX_INFO_LIST_ADD(rc, dirs, PMIX_NOTIFY_COMPLETION, NULL,
                       PMIX_BOOL); // notify us when the debugger job completes
    PMIX_INFO_LIST_ADD(rc, dirs, PMIX_FWD_STDOUT, NULL, PMIX_BOOL); // forward stdout to me
    PMIX_INFO_LIST_ADD(rc, dirs, PMIX_FWD_STDERR, NULL, PMIX_BOOL); // forward stderr to me
    PMIX_INFO_LIST_CONVERT(rc, dirs, &darray);
    PMIX_INFO_LIST_RELEASE(dirs);
    mydata->info = darray.array;
    mydata->ninfo = darray.size;
    darray.array = NULL;
    darray.size = 0;
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    /* spawn the daemons */
    printf("Debugger: spawning %s\n", mydata->apps[0].cmd);
    rc = PMIx_Spawn(mydata->info, mydata->ninfo, mydata->apps, mydata->napps, dbnspace);
    PMIX_INFO_FREE(mydata->info, mydata->ninfo);
    PMIX_APP_FREE(mydata->apps, mydata->napps);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Debugger daemons failed to launch with error: %s\n",
                PMIx_Error_string(rc));
        free(mydata);
        goto done;
    }
    free(mydata);

    /* wait for the IL to terminate */
    printf("WAITING FOR IL TO TERMINATE\n");
    while (ilactive) {
        struct timespec tp = {0, 500000};
        nanosleep(&tp, NULL);
    }

done:
    PMIx_tool_finalize();

    if (NULL != myuri) {
        free(myuri);
    }

    return (rc);
}
