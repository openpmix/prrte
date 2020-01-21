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
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <pmix_tool.h>
#include "debugger.h"

static pmix_proc_t myproc;

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
static void cbfunc(pmix_status_t status,
                   pmix_info_t *info, size_t ninfo,
                   void *cbdata,
                   pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t*)cbdata;
    size_t n;

    mq->status = status;
    /* save the returned info - the PMIx library "owns" it
     * and will release it and perform other cleanup actions
     * when release_fn is called */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            fprintf(stderr, "Key %s Type %s(%d)\n", info[n].key, PMIx_Data_type_string(info[n].value.type), info[n].value.type);
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
static void notification_fn(size_t evhdlr_registration_id,
                            pmix_status_t status,
                            const pmix_proc_t *source,
                            pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc,
                            void *cbdata)
{
    /* this example doesn't do anything with default events */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* this is an event notification function that we explicitly request
 * be called when the PMIX_ERR_JOB_TERMINATED notification is issued.
 * We could catch it in the general event notification function and test
 * the status to see if it was "job terminated", but it often is simpler
 * to declare a use-specific notification callback point. In this case,
 * we are asking to know whenever a job terminates, and we will then
 * know we can exit */
static void release_fn(size_t evhdlr_registration_id,
                       pmix_status_t status,
                       const pmix_proc_t *source,
                       pmix_info_t info[], size_t ninfo,
                       pmix_info_t results[], size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    myrel_t *lock;
    pmix_status_t rc;
    bool found;
    int exit_code;
    size_t n;
    pmix_proc_t *affected = NULL;

    /* find our return object */
    lock = NULL;
    found = false;
    for (n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
            lock = (myrel_t*)info[n].value.data.ptr;
            /* not every RM will provide an exit code, but check if one was given */
        } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
            exit_code = info[n].value.data.integer;
            found = true;
        } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
            affected = info[n].value.data.proc;
        }
    }
    /* if the object wasn't returned, then that is an error */
    if (NULL == lock) {
        fprintf(stderr, "LOCK WASN'T RETURNED IN RELEASE CALLBACK\n");
        /* let the event handler progress */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    /* see if the code is LAUNCHER_READY */
    if (PMIX_LAUNCHER_READY == status) {
            fprintf(stderr, "%d DEBUGGER NOTIFIED THAT LAUNCHER IS READY\n", (int)getpid());
    } else {
        fprintf(stderr, "DEBUGGER NOTIFIED THAT JOB %s TERMINATED - AFFECTED %s\n", lock->nspace,
                (NULL == affected) ? "NULL" : affected->nspace);
        if (found) {
            lock->exit_code = exit_code;
            lock->exit_code_given = true;
        }
    }
    DEBUG_WAKEUP_THREAD(&lock->lock);

    /* tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    return;
}

/* event handler registration is done asynchronously because it
 * may involve the PMIx server registering with the host RM for
 * external events. So we provide a callback function that returns
 * the status of the request (success or an error), plus a numerical index
 * to the registered event. The index is used later on to deregister
 * an event handler - if we don't explicitly deregister it, then the
 * PMIx server will do so when it see us exit */
static void evhandler_reg_callbk(pmix_status_t status,
                                 size_t evhandler_ref,
                                 void *cbdata)
{
    mylock_t *lock = (mylock_t*)cbdata;

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                   myproc.nspace, myproc.rank, status, (unsigned long)evhandler_ref);
    }
    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

static void spawn_cbfunc(pmix_status_t status,
                         pmix_nspace_t nspace, void *cbdata)
{
    myquery_data_t *mydata = (myquery_data_t*)cbdata;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;

    PMIX_INFO_FREE(mydata->info, mydata->ninfo);
    PMIX_APP_FREE(mydata->apps, mydata->napps);
    free(mydata);
    fprintf(stderr, "Debugger daemon job: %s\n", nspace);
}

static void spawn_debugger(size_t evhdlr_registration_id,
                           pmix_status_t status,
                           const pmix_proc_t *source,
                           pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc,
                           void *cbdata)
{
    pmix_status_t rc;
    size_t n;
    char cwd[1024];
    char *appspace = NULL;
    myquery_data_t *mydata;

    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NSPACE)) {
            appspace = info[n].value.data.string;
            break;
        }
    }
    /* if the namespace of the launched job wasn't returned, then that is an error */
    if (NULL == appspace) {
        fprintf(stderr, "LAUNCHED NAMESPACE WASN'T RETURNED IN CALLBACK\n");
        /* let the event handler progress */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }
    fprintf(stderr, "Child job to be debugged: %s\n", appspace);

    /* setup the debugger */
    mydata = (myquery_data_t*)malloc(sizeof(myquery_data_t));
    mydata->napps = 1;
    PMIX_APP_CREATE(mydata->apps, mydata->napps);
    mydata->apps[0].cmd = strdup("./daemon");
    PMIX_ARGV_APPEND(rc, mydata->apps[0].argv, "./daemon");
    getcwd(cwd, 1024);  // point us to our current directory
    mydata->apps[0].cwd = strdup(cwd);
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    mydata->ninfo = 6;
    PMIX_INFO_CREATE(mydata->info, mydata->ninfo);
    n=0;
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
    ++n;
   // PMIX_INFO_LOAD(&dinfo[1], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_DEBUG_JOB, appspace, PMIX_STRING); // the nspace being debugged
    ++n;
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the debugger job completes
    ++n;
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_DEBUG_WAITING_FOR_NOTIFY, NULL, PMIX_BOOL);  // tell the daemon that the proc is waiting to be released
    ++n;
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);  // forward stdout to me
    ++n;
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);  // forward stderr to me
    /* spawn the daemons */
    fprintf(stderr, "Debugger: spawning %s\n", mydata->apps[0].cmd);
    if (PMIX_SUCCESS != (rc = PMIx_Spawn_nb(mydata->info, mydata->ninfo, mydata->apps, mydata->napps, spawn_cbfunc, (void*)mydata))) {
        fprintf(stderr, "Debugger daemons failed to launch with error: %s\n", PMIx_Error_string(rc));
        return;
    }

    /* tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    return;
}

#define DBGR_LOOP_LIMIT  10

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info, *iptr;
    pmix_app_t *app;
    size_t ninfo, napps;
    char *nspace = NULL;
    int i;
    pmix_query_t *query;
    size_t nq, n;
    myquery_data_t myquery_data;
    bool cospawn = false, stop_on_exec = false, cospawn_reqd = false;
    char cwd[1024];
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    mylock_t mylock;
    myrel_t myrel, launcher_ready, dbrel;
    pid_t pid;
    pmix_envar_t envar;
    char *launchers[] = {
        "prun",
        "mpirun",
        "mpiexec",
        "prrterun",
        NULL
    };
    pmix_proc_t proc;
    bool found;
    pmix_data_array_t darray;
    char *tmp;
    char clientspace[PMIX_MAX_NSLEN+1];

    pid = getpid();

    /* Process any arguments we were given */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "-h") ||
            0 == strcmp(argv[i], "--help")) {
            /* print the usage message and exit */

        }
    }
    info = NULL;
    ninfo = 0;

    /* use the system connection first, if available */
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n", PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    fprintf(stderr, "Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank, (unsigned long)pid);

    /* construct the debugger termination release */
    DEBUG_CONSTRUCT_LOCK(&dbrel.lock);

    /* register a default event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* check to see if we are using an intermediate launcher - we only
     * support those we recognize */
    found = false;
    if (1 < argc) {
        for (n=0; NULL != launchers[n]; n++) {
            if (0 == strcmp(argv[1], launchers[n])) {
                found = true;
            }
        }
    }
    if (!found) {
        fprintf(stderr, "Wrong test, dude\n");
        exit(1);
    }

    /* register to receive the "launcher-ready" event telling us
     * that the launcher is ready for us to connect to it */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    code = PMIX_LAUNCHER_READY;
    /* pass a lock object to release us when the launcher is ready */
    DEBUG_CONSTRUCT_LOCK(&launcher_ready.lock);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &launcher_ready, PMIX_POINTER);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_HDLR_NAME, "LAUNCHER-READY", PMIX_STRING);
    PMIx_Register_event_handler(&code, 1, info, 2,
                                release_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIX_INFO_FREE(info, 2);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 2);

    /* register to receive the "launch complete" event telling us
     * the nspace of the job being debugged so we can spawn the
     * debugger daemons against it */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    code = PMIX_LAUNCH_COMPLETE;
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "LAUNCH-COMPLETE", PMIX_STRING);
    PMIx_Register_event_handler(&code, 1, info, 1,
                                spawn_debugger, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIX_INFO_FREE(info, 2);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 2);

    /* we are using an intermediate launcher - we will use the
     * reference server to start it, but tell it to wait after
     * launch for directive prior to spawning the application */
    napps = 1;
    PMIX_APP_CREATE(app, napps);
    /* setup the executable */
    app[0].cmd = strdup(argv[1]);
    PMIX_ARGV_APPEND(rc, app[0].argv, argv[1]);
    for (n=2; n < argc; n++) {
        PMIX_ARGV_APPEND(rc, app[0].argv, argv[n]);
    }
    getcwd(cwd, 1024);  // point us to our current directory
    app[0].cwd = strdup(cwd);
    app[0].maxprocs = 1;
    /* provide job-level directives so the apps do what the user requested */
#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
    ninfo = 7;
#else
    ninfo = 6;
#endif
    PMIX_INFO_CREATE(info, ninfo);
    n=0;
    PMIX_INFO_LOAD(&info[n], PMIX_MAPBY, "slot", PMIX_STRING);  // map by slot
    n++;
    asprintf(&tmp, "%s:%d", myproc.nspace, myproc.rank);
    PMIX_ENVAR_LOAD(&envar, "PMIX_LAUNCHER_PAUSE_FOR_TOOL", tmp, ':');
    free(tmp);
    PMIX_INFO_LOAD(&info[n], PMIX_SET_ENVAR, &envar, PMIX_ENVAR);  // launcher is to wait for directives
    n++;
    PMIX_ENVAR_DESTRUCT(&envar);
    cospawn = true;
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDOUT, &cospawn, PMIX_BOOL);  // forward stdout to me
    n++;
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDERR, &cospawn, PMIX_BOOL);  // forward stderr to me
    n++;
    PMIX_INFO_LOAD(&info[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the job completes
    n++;
    PMIX_INFO_LOAD(&info[n], PMIX_SPAWN_TOOL, NULL, PMIX_BOOL); // we are spawning a tool
    n++;
#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
    PMIX_INFO_LOAD(&info[n], PMIX_LAUNCHER_RENDEZVOUS_FILE, "dbgr.rndz.txt", PMIX_STRING);  // have it output a specific rndz file
#endif
    /* spawn the job - the function will return when the launcher
     * has been launched. Note that this doesn't tell us anything
     * about the launcher's state - it just means that the launcher
     * has been fork/exec'd */
    fprintf(stderr, "Debugger: spawning %s\n", app[0].cmd);
    rc = PMIx_Spawn(info, ninfo, app, napps, clientspace);
    PMIX_INFO_FREE(info, ninfo);
    PMIX_APP_FREE(app, napps);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Application failed to launch with error: %s(%d)\n", PMIx_Error_string(rc), rc);
        goto done;
    }

    /* wait here for the launcher to declare itself ready */
    DEBUG_WAIT_THREAD(&launcher_ready.lock);
    DEBUG_DESTRUCT_LOCK(&launcher_ready.lock);


    /* register callback for when launcher terminates */
    code = PMIX_ERR_JOB_TERMINATED;
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);
    myrel.nspace = strdup(clientspace);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
    /* only call me back when this specific job terminates */
    PMIX_LOAD_PROCID(&proc, clientspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, info, 2,
                                release_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 2);

    /* send the launch directives */
    ninfo = 3;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_PROC_LOAD(&proc, clientspace, 0);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);  // deliver to the target launcher
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);  // only non-default handlers
    /* provide a few job-level directives */
    darray.type = PMIX_INFO;
    darray.size = 4;
    PMIX_INFO_CREATE(darray.array, darray.size);
    iptr = (pmix_info_t*)darray.array;
    PMIX_ENVAR_LOAD(&envar, "FOOBAR", "1", ':');
    PMIX_INFO_LOAD(&iptr[0], PMIX_SET_ENVAR, &envar, PMIX_ENVAR);
    PMIX_ENVAR_DESTRUCT(&envar);
    PMIX_ENVAR_LOAD(&envar, "PATH", "/home/common/local/toad", ':');
    PMIX_INFO_LOAD(&iptr[1], PMIX_PREPEND_ENVAR, &envar, PMIX_ENVAR);
    PMIX_ENVAR_DESTRUCT(&envar);
    PMIX_INFO_LOAD(&iptr[2], PMIX_DEBUG_STOP_IN_INIT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&iptr[3], PMIX_NOTIFY_LAUNCH, NULL, PMIX_BOOL); // notify us when the job is launched
    /* load the array */
    PMIX_INFO_LOAD(&info[2], PMIX_DEBUG_JOB_DIRECTIVES, &darray, PMIX_DATA_ARRAY);

    fprintf(stderr, "[%s:%u%lu] Sending launch directives\n", myproc.nspace, myproc.rank, (unsigned long)pid);
    PMIx_Notify_event(PMIX_LAUNCH_DIRECTIVE,
                      NULL, PMIX_RANGE_CUSTOM,
                      info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);

    DEBUG_WAIT_THREAD(&myrel.lock);

  done:
    DEBUG_DESTRUCT_LOCK(&myrel.lock);
    DEBUG_DESTRUCT_LOCK(&dbrel.lock);
    PMIx_tool_finalize();

    return(rc);
}
