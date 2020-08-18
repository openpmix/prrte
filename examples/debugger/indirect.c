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
static char clientspace[PMIX_MAX_NSLEN+1];
static char application_namespace[PMIX_MAX_NSLEN+1];

static int query_application_namespace(void);

/* This is the event notification function we pass down below
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
    printf("%s called for event %s source=%s:%d\n", __FUNCTION__,
           PMIx_Error_string(status), source->nspace, source->rank);
    /* This example doesn't do anything with default events */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* This is an event notification function that we explicitly request
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
    bool found;
    int exit_code;
    size_t n;
    pmix_proc_t *affected = NULL;

    printf("%s called for event %s source=%s:%d\n", __FUNCTION__,
           PMIx_Error_string(status), source->nspace, source->rank);
    /* Find our return object */
    lock = NULL;
    found = false;
    for (n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT,
                         PMIX_MAX_KEYLEN)) {
            lock = (myrel_t*)info[n].value.data.ptr;
            /* Not every RM will provide an exit code, but check if one was
             * given */
        } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
            exit_code = info[n].value.data.integer;
            found = true;
        } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC,
                                PMIX_MAX_KEYLEN)) {
            affected = info[n].value.data.proc;
        } 
    }
    /* If the object wasn't returned, then that is an error */
    if (NULL == lock) {
        fprintf(stderr, "LOCK WASN'T RETURNED IN RELEASE CALLBACK\n");
        /* Let the event handler progress */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    /* See if the code is LAUNCHER_READY */
    switch (status) {
    case PMIX_LAUNCHER_READY:
       printf("pid %d DEBUGGER NOTIFIED THAT LAUNCHER IS READY\n",
              (int)getpid());
       break;
    case PMIX_ERR_JOB_TERMINATED:
        printf("DEBUGGER NOTIFIED THAT JOB %s TERMINATED - AFFECTED %s\n",
               lock->nspace, (NULL == affected) ? "NULL" : affected->nspace);
        if (found) {
            lock->exit_code = exit_code;
            lock->exit_code_given = true;
        }
    case PMIX_LAUNCH_COMPLETE:
       printf("pid %d DEBUGGER NOTIFIED THAT LAUNCH IS COMPLETE\n",
              (int)getpid());
        break;
    }
    DEBUG_WAKEUP_THREAD(&lock->lock);

    /* Tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    return;
}

/* Event handler registration is done asynchronously because it
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

    printf("%s called, status=%s evhandler_ref=%d\n", __FUNCTION__,
           PMIx_Error_string(status), (int)evhandler_ref);
    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status,
                (unsigned long)evhandler_ref);
    }
    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

/* This function is the callback that handles notification that the
 * PMIx_Spawn_nb call is complete, and which saves the application namespace. */
static void spawn_cbfunc(pmix_status_t status,
                         pmix_nspace_t nspace, void *cbdata)
{
    myquery_data_t *mydata = (myquery_data_t*)cbdata;

    printf("%s called, status=%s, nspace='%s'\n", __FUNCTION__,
           PMIx_Error_string(status), nspace);
    PMIX_INFO_FREE(mydata->info, mydata->ninfo);
    PMIX_APP_FREE(mydata->apps, mydata->napps);
    free(mydata);
    printf("Debugger daemon job: %s\n", nspace);
}

static void spawn_debugger() 
{
    pmix_status_t rc;
    size_t n;
    char cwd[1024];
    myquery_data_t *mydata;

    /* If the namespace of the launched job wasn't returned, then that is an
     * error */
    if ('\0' == clientspace[0]) {
        fprintf(stderr, "LAUNCHED NAMESPACE WASN'T RETURNED BY PMIx_Spawn\n");
        return;
    }
    printf("Child job to be debugged: %s\n", clientspace);

    /* Set up the debugger */
    mydata = (myquery_data_t*)malloc(sizeof(myquery_data_t));
    mydata->napps = 1;
    PMIX_APP_CREATE(mydata->apps, mydata->napps);
    /* Set the name of the debugger daemon process */
    mydata->apps[0].cmd = strdup("./daemon");
    /* Set the arguments for the debugger daemon, in this case just argv[0] */
    PMIX_ARGV_APPEND(rc, mydata->apps[0].argv, "./daemon");
    /* Set the working directory to our current working directory */
    getcwd(cwd, 1024);  // point us to our current directory
    mydata->apps[0].cwd = strdup(cwd);
    /* No attributes set in pmix_app_t structure */
    mydata->apps[0].ninfo = 0;
    mydata->apps[0].info = NULL;
    /* One daemon process */
    mydata->apps[0].maxprocs = 1;
    /* Provide directives so the daemon goes where we want, and let the RM
     * know this is a debugger daemon */
    mydata->ninfo = 7;
    PMIX_INFO_CREATE(mydata->info, mydata->ninfo);
    n=0;
#if 0
    /* Co-locate daemons 1 per node -- Not implemented yet! */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_DEBUG_DAEMONS_PER_NODE, 1, PMIX_UINT16);
#else
    /* Map debugger demon by node */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);
#endif
    ++n;
    /* Indicate this is a debugger daemon */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL);
    ++n;
    /* Indicate that we want to target the application namespace (Replacement for deprecated PMIX_DEBUG_JOB) */
#if 0
    PMIX_INFO_LOAD(&dinfo[n], PMIX_DEBUG_TARGET, application_namespace, PMIX_STRING);
#else
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_DEBUG_JOB, application_namespace, PMIX_STRING);
#endif
    ++n;
    /* Send completion notification to this process */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);
    ++n;
    /* Indicate that the daemon is waiting for the application process to be
     * released */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_DEBUG_WAITING_FOR_NOTIFY, NULL,
                   PMIX_BOOL);
    ++n;
    /* Forward stdout to this process */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);
    ++n;
    /* Forward stderr to this process */
    PMIX_INFO_LOAD(&mydata->info[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);
    /* spawn the daemons */
    printf("Debugger: spawning %s\n", mydata->apps[0].cmd);
    if (PMIX_SUCCESS != (rc = PMIx_Spawn_nb(mydata->info, mydata->ninfo,
                         mydata->apps, mydata->napps, spawn_cbfunc,
                         (void*)mydata))) {
        fprintf(stderr, "Debugger daemons failed to launch with error: %s\n",
                PMIx_Error_string(rc));
        return;
    }
    return;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info;
    pmix_info_t *iptr;
    pmix_app_t *app;
    size_t ninfo;
    size_t napps;
    int i;
    int n;
    pmix_status_t code = 0;
    mylock_t mylock;
    myrel_t myrel;
    myrel_t launcher_ready;
    myrel_t dbrel;
    myrel_t launch_complete;
    pid_t pid;
    pmix_envar_t envar;
    char *launchers[] = {
        "prun",
        "mpirun",
        "mpiexec",
        "prterun",
        NULL
    };
    pmix_proc_t proc;
    bool found;
    pmix_data_array_t darray;
    char *tmp;
    char cwd[1024];

    pid = getpid();

    /* Process any arguments we were given */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "-h") ||
            0 == strcmp(argv[i], "--help")) {
            /* print the usage message and exit */
            printf("Usage: ./indirect [launcher with args]\n");
            printf(" Requires 'prte' persistent daemon is running.\n");
            printf(" -h|--help      Display this help message and exit.\n");
            printf("Recognized launchers:\n");
            for (n=0; NULL != launchers[n]; n++) {
                printf("  - %s\n", launchers[n]);
            }
            exit(0);
        }
    }
    if (1 >= argc) {
        fprintf(stderr, "Error: Must specify a launcher\n");
        exit(1);
    }
    /* Check to see if we are using an intermediate launcher - we only
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
        fprintf(stderr, "Error: Unrecognized launcher: \"%s\"\n", argv[1]);
        exit(1);
    }

    info = NULL;
    ninfo = 1;
    n = 0;

    /* Use the system connection first, if available */
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[n], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n",
                PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    printf("Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace,
           myproc.rank, (unsigned long)pid);

    /* Construct the debugger termination lock */
    DEBUG_CONSTRUCT_LOCK(&dbrel.lock);

    /* Register a default event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0, notification_fn,
                                evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* Register to receive the "launcher-ready" event telling us
     * that the launcher is ready for us to connect to it */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    code = PMIX_LAUNCHER_READY;
    /* Pass a lock object to release us when the launcher is ready */
    DEBUG_CONSTRUCT_LOCK(&launcher_ready.lock);
    ninfo = 2;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    /* Pass the lock we wait for to the callback */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_RETURN_OBJECT, &launcher_ready,
                   PMIX_POINTER);
    n++;
    /* Set the name of the callback */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_HDLR_NAME, "LAUNCHER-READY",
                   PMIX_STRING);
    PMIx_Register_event_handler(&code, 1, info, ninfo, release_fn,
                                evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    PMIX_INFO_FREE(info, ninfo);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr,
               "Registration for PMIX_LAUNCHER_READY notification failed: %s\n",
               PMIx_Error_string(mylock.status));
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* Register to receive the "launch complete" event telling us
     * the nspace of the job being debugged so we can spawn the
     * debugger daemons against it */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    DEBUG_CONSTRUCT_LOCK(&launch_complete.lock);
    code = PMIX_LAUNCH_COMPLETE;
    ninfo = 2;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    /* Pass the lock we will wait for to the callback */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_RETURN_OBJECT, &launch_complete,
                   PMIX_POINTER);
    n++;
    /* Set the name of the callback */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_HDLR_NAME, "LAUNCH-COMPLETE",
                   PMIX_STRING);
    PMIx_Register_event_handler(&code, 1, info, 2, release_fn,
                                evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    PMIX_INFO_FREE(info, ninfo);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr,
              "Registration for PMIX_LAUNCH_COMPLETE notification failed: %s\n",
              PMIx_Error_string(mylock.status));
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    /* We are using an intermediate launcher - we will use the
     * reference server to start it, but tell it to wait after
     * launch for directive prior to spawning the application */
    napps = 1;
    PMIX_APP_CREATE(app, napps);
    /* Set up the executable */
    app[0].cmd = strdup(argv[1]);
    /* Set the launcher command as argv[0] then append launcher command line
     * arguments to argv */
    PMIX_ARGV_APPEND(rc, app[0].argv, strdup(argv[1]));
    for (n=2; n < argc; n++) {
        PMIX_ARGV_APPEND(rc, app[0].argv, strdup(argv[n]));
    }
    /* No environment variables */
    app[0].env = NULL;
    /* Set the launcher working directory to our current directory */
    getcwd(cwd, 1024);
    app[0].cwd = strdup(cwd);
    /* There will be one launcher process */
    app[0].maxprocs = 1;
    /* No attributes set in the pmix_app_t structure */
    app[0].ninfo = 0;
    app[0].info = NULL;
    /* Provide job-level directives so the launcher does what the user
     * requested */
#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
    ninfo = 6;
#else
    ninfo = 5;
#endif
    /* Set the namespace and rank of the tool to pause for */
    asprintf(&tmp, "%s:%d", myproc.nspace, myproc.rank);
    PMIX_ENVAR_LOAD(&envar, "PMIX_LAUNCHER_PAUSE_FOR_TOOL", tmp, ':');
    PMIX_INFO_CREATE(info, ninfo);
    n=0;
    /* Map processes by slot */
    PMIX_INFO_LOAD(&info[n], PMIX_MAPBY, "slot", PMIX_STRING);
    n++;
    PMIX_INFO_LOAD(&info[n], PMIX_SET_ENVAR, &envar, PMIX_ENVAR);
    n++;
    /* Forward stdout to this process */
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);
    n++;
    /* Forward stderr to this process */
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);
    n++;
    /* Notify this process when the job completes */
    PMIX_INFO_LOAD(&info[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);
    n++;
#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
    /* Specify the rendezvous file to create */
    PMIX_INFO_LOAD(&info[n], PMIX_LAUNCHER_RENDEZVOUS_FILE, "dbgr.rndz.txt",
                   PMIX_STRING);
#endif
    /* Spawn the job - the function will return when the launcher
     * has been launched. Note that this doesn't tell us anything
     * about the launcher's state - it just means that the launcher
     * has been fork/exec'd */
    printf("Debugger: spawning %s\n", app[0].cmd);
    rc = PMIx_Spawn(info, ninfo, app, napps, clientspace);
    PMIX_INFO_FREE(info, ninfo);
    PMIX_APP_FREE(app, napps);
    PMIX_ENVAR_DESTRUCT(&envar);
    free(tmp);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Application failed to launch with error: %s(%d)\n",
                PMIx_Error_string(rc), rc);
        goto done;
    }
    printf("Launcher namespace is '%s'\n", clientspace);
    /* Wait here for the launcher to declare itself ready */
    DEBUG_WAIT_THREAD(&launcher_ready.lock);
    DEBUG_DESTRUCT_LOCK(&launcher_ready.lock);

    /* Register callback for when launcher terminates */
    code = PMIX_ERR_JOB_TERMINATED;
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);
    myrel.nspace = strdup(clientspace);
    ninfo = 2;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
    /* Only call me back when this specific job terminates */
    PMIX_LOAD_PROCID(&proc, clientspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, info, ninfo, release_fn,
                                evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    PMIX_INFO_FREE(info, ninfo);
    rc = mylock.status;
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "Registration for PMIX_ERR_JOB_TERMINATED notification failed: %s\n",
                PMIx_Error_string(mylock.status));
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* Provide a few job-level directives */
    darray.type = PMIX_INFO;
    darray.size = 2;
    n = 0;
    PMIX_INFO_CREATE(darray.array, darray.size);
    iptr = (pmix_info_t*)darray.array;
    /* Pause the application in PMIx_Init */
    PMIX_INFO_LOAD(&iptr[n], PMIX_DEBUG_STOP_IN_INIT, NULL, PMIX_BOOL);
    n++;
    /* Notify this process when application is launched */
    PMIX_INFO_LOAD(&iptr[n], PMIX_NOTIFY_LAUNCH, NULL, PMIX_BOOL);

    /* Send the launch directives */
    ninfo = 3;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_PROC_LOAD(&proc, clientspace, 0);
    /* Send notification to target launcher */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);
    n++;
    /* Don't notify default event callbacks */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    n++;
    /* Load the 'darray' array */
    PMIX_INFO_LOAD(&info[n], PMIX_DEBUG_JOB_DIRECTIVES, &darray,
                   PMIX_DATA_ARRAY);

    printf("[%s:%u%lu] Sending launch directives\n", myproc.nspace, myproc.rank,
          (unsigned long)pid);
    PMIx_Notify_event(PMIX_LAUNCH_DIRECTIVE, NULL, PMIX_RANGE_CUSTOM, info,
                      ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);

    printf("Waiting for launch to complete\n");
    DEBUG_WAIT_THREAD(&launch_complete.lock);
    printf("Launch is complete\n");
    if (-1 == query_application_namespace()) {
        goto done;
    }
    /* We can't spawn the debugger until we have the application namespace,
     * and we can't get the application namespace until the launch is
     * complete */
    spawn_debugger();
    ninfo = 2;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    /* Specify that target launcher namespace is to be notified */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);
    n++;
    /* Don't send notification to default handler */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    printf("Resuming launcher process '%s:%d'\n", proc.nspace, proc.rank);
    rc = PMIx_Notify_event(PMIX_ERR_DEBUGGER_RELEASE, NULL, PMIX_RANGE_CUSTOM,
                           info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    if ((PMIX_SUCCESS != rc) && (PMIX_OPERATION_SUCCEEDED != rc)) {
        fprintf(stderr, "An error occurred resuming launcher process: %s.\n",
                PMIx_Error_string(rc));
        goto done;
    }
    ninfo = 2;
    PMIX_PROC_LOAD(&proc, application_namespace, PMIX_RANK_WILDCARD);
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    /* Specify that application namespace is to be notified */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);
    n++;
    /* Don't send notification to default handler */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    printf("Resuming application processes '%s:%d'\n", proc.nspace, proc.rank);
    rc = PMIx_Notify_event(PMIX_ERR_DEBUGGER_RELEASE, NULL, PMIX_RANGE_CUSTOM,
                           info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    if ((PMIX_SUCCESS != rc) && (PMIX_OPERATION_SUCCEEDED != rc)) {
        fprintf(stderr, "An error occurred resuming application process: %s.\n",
                PMIx_Error_string(rc));
        goto done;
    }

    printf("Waiting for launcher to terminate\n");
    DEBUG_WAIT_THREAD(&myrel.lock);

  done:
    DEBUG_DESTRUCT_LOCK(&launch_complete.lock);
    DEBUG_DESTRUCT_LOCK(&myrel.lock);
    DEBUG_DESTRUCT_LOCK(&dbrel.lock);
    PMIx_tool_finalize();

    return(rc);
}

int query_application_namespace(void)
{
    pmix_info_t *namespace_query_data;
    char *p;
    int len;
    size_t namespace_query_size;
    pmix_status_t rc;
    pmix_query_t namespace_query;
    int wildcard_rank = PMIX_RANK_WILDCARD;

    PMIX_QUERY_CONSTRUCT(&namespace_query);
    PMIX_ARGV_APPEND(rc, namespace_query.keys, PMIX_QUERY_NAMESPACES);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "An error occurred creating namespace query.");
        PMIX_QUERY_DESTRUCT(&namespace_query);
        return -1;
    }
    PMIX_INFO_CREATE(namespace_query.qualifiers, 2);
    PMIX_INFO_LOAD(&namespace_query.qualifiers[0], PMIX_NSPACE,
                   clientspace, PMIX_STRING);
    PMIX_INFO_LOAD(&namespace_query.qualifiers[1], PMIX_RANK, &wildcard_rank,
                   PMIX_INT32);
    namespace_query.nqual = 2;
    rc = PMIx_Query_info(&namespace_query, 1, &namespace_query_data,
                         &namespace_query_size);
    PMIX_QUERY_DESTRUCT(&namespace_query);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr,
                "An error occurred querying application namespace: %s.\n",
                PMIx_Error_string(rc));
        return -1;
    }
    if ((1 != namespace_query_size) ||
              (PMIX_STRING != namespace_query_data->value.type)) {
        fprintf(stderr, "The response to namespace query has wrong format.\n");
        return -1;
    }

      /* The query retruns a comma-delimited list of namespaces. If there are
       * multple namespaces in the list, then assume the first is the
       * application namespace and the second is the daemon namespace.
       * Copy only the application namespace and terminate the name with '\0' */
    p = strchr(namespace_query_data->value.data.string, ',');
    if (NULL == p) {
        len = strlen(namespace_query_data->value.data.string);
    }
    else {
        len = p - namespace_query_data->value.data.string;
    }
    strncpy(application_namespace, namespace_query_data->value.data.string,
            len);
    application_namespace[len] = '\0';

    printf("Application namespace is '%s'\n", application_namespace);
    return 0;
}
