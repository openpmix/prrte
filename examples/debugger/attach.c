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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <pmix_tool.h>
#include "debugger.h"


static pmix_proc_t myproc;
static char launcher_namespace[PMIX_MAX_NSLEN + 1];
static char application_namespace[PMIX_MAX_NSLEN + 1];
static char daemon_namespace[PMIX_MAX_NSLEN + 1];


/*
 * Launch debugger daemons alongside the application processes
 */
static int attach_to_running_job(void);

/*
 * Find the application namespace given the launcher namespace
 */
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
    printf("Debugger: %s called as callback for event=%s\n", __FUNCTION__,
           PMIx_Error_string(status));

    /* Note: This example doesn't do anything with default events */

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

    printf("Debugger: %s called as callback for event=%s\n", __FUNCTION__,
           PMIx_Error_string(status));

    /* find our return object */
    lock = NULL;
    found = false;
    for (n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT,
                         PMIX_MAX_KEYLEN)) {
            lock = (myrel_t*)info[n].value.data.ptr;
            /* not every RM will provide an exit code, but check if one was
             * given */
        } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
            exit_code = info[n].value.data.integer;
            found = true;
        } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC,
                                PMIX_MAX_KEYLEN)) {
            affected = info[n].value.data.proc;
            if( !PMIX_CHECK_NSPACE(affected->nspace, daemon_namespace) ) {
                fprintf(stderr, "Debugger: Error: Received notice of unexpected namespace \"%s\" instead of \"%s\"\n",
                        affected->nspace, daemon_namespace);
                /* Tell the event handler state machine that we are the last step */
                if (NULL != cbfunc) {
                    cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
                }
                return;
            }
        }
    }

    /* If the lock object wasn't returned, then that is an error */
    if (NULL == lock) {
        fprintf(stderr, "LOCK WASN'T RETURNED IN RELEASE CALLBACK\n");
        /* Let the event handler progress */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    fprintf(stderr, "DEBUGGER NOTIFIED THAT JOB %s TERMINATED - AFFECTED %s\n",
            lock->nspace, (NULL == affected) ? "NULL" : affected->nspace);
    if (found) {
        lock->exit_code = exit_code;
        lock->exit_code_given = true;
    }

    /* Tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }

    DEBUG_WAKEUP_THREAD(&lock->lock);
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

    printf("Debugger: %s called to register callback\n", __FUNCTION__);

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status,
                (unsigned long)evhandler_ref);
    }
    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info;
    size_t ninfo;
    pid_t pid;
    int n;
    int i;

    pid = getpid();

    /* Process any arguments we were given */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "-h") ||
            0 == strcmp(argv[i], "--help")) {
            /* print the usage message and exit */
            printf("Usage: ./attach NAMESPACE_OF_LAUNCHER\n");
            printf(" Requires knowledge of the namespace of the job launcher\n");
            printf(" -h|--help      Display this help message and exit.\n");
            exit(0);
        }
    }
    if (argc < 2) {
        printf("Usage: %s NAMESPACE_OF_LAUNCHER\n", argv[0]);
        exit(1);
    }
    strncpy(launcher_namespace, argv[1], PMIX_MAX_NSLEN);
    info = NULL;
    ninfo = 1;
    n = 0;

    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[n], PMIX_LAUNCHER, NULL, PMIX_BOOL);

    /* Initialize as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n",
                PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    printf("Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace,
           myproc.rank, (unsigned long)pid);
    printf("Launcher ns %s\n", launcher_namespace);

    /* Register a default event handler */
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, NULL, NULL);

    /* Resolve the application namespace
     * We are given the namespace of the launcher. The debugger daemon needs
     * the namespace of the application so it can interact with and control
     * execution of the application tasks.
     *
     * Query the namespaces known to the launcher to get the application
     * namespace.
     */
    query_application_namespace();
    fprintf(stderr, "Application ns %s\n", application_namespace);

    /* Attach to the running job */
    if (PMIX_SUCCESS != (rc = attach_to_running_job())) {
        fprintf(stderr, "Failed to attach to nspace %s: error code %d\n",
                launcher_namespace, rc);
    }

    /* Finalize the tool */
    PMIx_tool_finalize();

    return(rc);
}

static int attach_to_running_job(void)
{
    pmix_status_t rc;
    pmix_info_t *info;
    pmix_app_t *app;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    size_t ninfo;
    int n;
    mylock_t mylock;
    myrel_t myrel;
    char cwd[_POSIX_PATH_MAX];

    printf("Debugger: %s called to attach to application with namespace %s\n",
           __FUNCTION__, application_namespace);

    /* Note: This is where a debugger tool would process the proctable to
     * create whatever blob it needs to provide to its daemons */

    /* Set up the debugger daemon spawn request */
    PMIX_APP_CREATE(app, 1);
    /* Set up the name of the daemon executable to launch */
    app->cmd = strdup("./daemon");
    app->argv = (char**)malloc(2*sizeof(char*));
    /* Set up the debuger daemon arguments, in this case, just argv[0] */
    app->argv[0] = strdup("./daemon");
    app->argv[1] = NULL;
    /* No environment variables */
    app->env = NULL;
    /* Set the daemon's working directory to our current directory */
    getcwd(cwd, _POSIX_PATH_MAX);
    app->cwd = strdup(cwd);
    /* No attributes set in the pmix_app_t structure */
    app->info = NULL;
    app->ninfo = 0;
    /* One debugger daemon */
    app->maxprocs = 1;
    /* Provide directives so the daemon goes where we want, and
     * let the RM know this is a debugger daemon */
    ninfo = 6;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
#if 0
    /* Co-locate daemons 1 per node -- Not implemented yet! */
    PMIX_INFO_LOAD(&info[n], PMIX_DEBUG_DAEMONS_PER_NODE, 1, PMIX_UINT16);
#else
    /* Map debugger daemon processes by node */
    PMIX_INFO_LOAD(&info[n], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);
#endif
    n++;
    /* Indicate this is a debugger daemon */
    PMIX_INFO_LOAD(&info[n], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL);
    n++;
    /* Indicate that we want to target the application namespace (Replacement for deprecated PMIX_DEBUG_JOB) */
#if 0
    PMIX_INFO_LOAD(&info[n], PMIX_DEBUG_TARGET, application_namespace, PMIX_STRING);
#else
    PMIX_INFO_LOAD(&info[n], PMIX_DEBUG_JOB, application_namespace, PMIX_STRING);
#endif
    n++;
    /* Forward stdout to this process */
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);
    n++;
    /* Forward stderr to this process */
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);
    n++;
    /* Indicate the requestor is a tool process */
    PMIX_INFO_LOAD(&info[n], PMIX_REQUESTOR_IS_TOOL, NULL, PMIX_BOOL);
    n++;

    printf("Debugger: Spawn debugger daemon\n");
    /* Spawn the daemon */
    rc = PMIx_Spawn(info, ninfo, app, 1, daemon_namespace);
    PMIX_APP_FREE(app, 1);
    PMIX_INFO_FREE(info, ninfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Error spawning debugger daemon, %s\n",
                PMIx_Error_string(rc));
        return -1;
    }
    printf("Debugger: Daemon namespace '%s'\n", daemon_namespace);

    /* Note: This is where a debugger tool would wait until the debug operation is
     * complete */

    /* Register callback for when the debugger daemon terminates */
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);
    myrel.nspace = strdup(daemon_namespace);
    PMIX_INFO_CREATE(info, 2);
    /* Only call me back when this specific job terminates */
    PMIX_INFO_LOAD(&info[0], PMIX_NSPACE, daemon_namespace, PMIX_STRING);
    /* Return access to the lock object */
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, info, 2, release_fn,
                                evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock); // wait for evhandler_reg_callbk
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);

    printf("Debugger: Waiting for debugger daemon namespace %s to complete\n", daemon_namespace);
    DEBUG_WAIT_THREAD(&myrel.lock); // wait for release_fn
    PMIX_INFO_FREE(info, 2);

    printf("Debugger: Daemon namespace %s terminated\n", daemon_namespace);
    return rc;
}

int query_application_namespace(void)
{
    pmix_info_t *namespace_query_data;
    size_t namespace_query_size;
    pmix_status_t rc;
    pmix_query_t namespace_query;
    int wildcard_rank = PMIX_RANK_WILDCARD;
    int n;
    char *ptr;

    PMIX_QUERY_CONSTRUCT(&namespace_query);
    PMIX_ARGV_APPEND(rc, namespace_query.keys, PMIX_QUERY_NAMESPACES);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "An error occurred creating namespace query.");
        PMIX_QUERY_DESTRUCT(&namespace_query);
        return -1;
    }

    namespace_query.nqual = 2;
    PMIX_INFO_CREATE(namespace_query.qualifiers, namespace_query.nqual);
    n = 0;
    PMIX_INFO_LOAD(&namespace_query.qualifiers[n], PMIX_NSPACE, launcher_namespace,
                   PMIX_STRING);
    n++;
    PMIX_INFO_LOAD(&namespace_query.qualifiers[n], PMIX_RANK, &wildcard_rank,
                   PMIX_INT32);

    /* Query all namespaces associated with the launcher_namespace */
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
     * Copy only the application namespace and terminate the name with '\0'
     */
    ptr = strchr(namespace_query_data->value.data.string, ',');
    if (NULL != ptr) {
#if 0
        // Last item in the list
        ptr = strrchr(namespace_query_data->value.data.string, ',');
        ptr += 1;
        strcpy(application_namespace, ptr);
#else
        // First item in the list
        int len = ptr - namespace_query_data->value.data.string;
        strncpy(application_namespace, namespace_query_data->value.data.string, len);
        application_namespace[len] = '\0';
#endif
    } else {
        strcpy(application_namespace, namespace_query_data->value.data.string);
    }
    printf("Application namespace is '%s'\n", application_namespace);

    return 0;
}

