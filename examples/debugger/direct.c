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
    myrel_t *lock;
    size_t n;

    if (PMIX_ERR_UNREACH == status ||
        PMIX_ERR_LOST_CONNECTION == status) {
        /* we should always have info returned to us - if not, there is
         * nothing we can do */
        if (NULL != info) {
            for (n=0; n < ninfo; n++) {
                if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
                    lock = (myrel_t*)info[n].value.data.ptr;
                }
            }
        }

        /* save the status */
        lock->exit_code = status;
        lock->exit_code_given = true;
        /* always release the lock if we lose connection
         * to our host server */
        DEBUG_WAKEUP_THREAD(&lock->lock);
    }

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

    /* find the return object */
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

    fprintf(stderr, "DEBUGGER NOTIFIED THAT JOB %s TERMINATED \n",
            (NULL == affected) ? "NULL" : affected->nspace);
    if (found) {
        if (!lock->exit_code_given) {
            lock->exit_code = exit_code;
            lock->exit_code_given = true;
        }
    }
    lock->lock.count--;
    if (0 == lock->lock.count) {
        DEBUG_WAKEUP_THREAD(&lock->lock);
    }

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

static pmix_status_t spawn_debugger(char *appspace, myrel_t *myrel)
{
    pmix_status_t rc;
    pmix_info_t *dinfo;
    pmix_app_t *debugger;
    size_t dninfo;
    char cwd[1024];
    char dspace[PMIX_MAX_NSLEN+1];
    mylock_t mylock;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    pmix_proc_t proc;

    /* setup the debugger */
    PMIX_APP_CREATE(debugger, 1);
    debugger[0].cmd = strdup("./daemon");
    PMIX_ARGV_APPEND(rc, debugger[0].argv, "./daemon");
    getcwd(cwd, 1024);  // point us to our current directory
    debugger[0].cwd = strdup(cwd);
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    dninfo = 7;
    PMIX_INFO_CREATE(dinfo, dninfo);
    PMIX_INFO_LOAD(&dinfo[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
    PMIX_INFO_LOAD(&dinfo[1], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LOAD(&dinfo[2], PMIX_DEBUG_JOB, appspace, PMIX_STRING); // the nspace being debugged
    PMIX_INFO_LOAD(&dinfo[3], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the debugger job completes
    PMIX_INFO_LOAD(&dinfo[4], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);  // forward stdout to me
    PMIX_INFO_LOAD(&dinfo[5], PMIX_FWD_STDERR, NULL, PMIX_BOOL);  // forward stderr to me
    PMIX_INFO_LOAD(&dinfo[6], PMIX_SETUP_APP_ENVARS, NULL, PMIX_BOOL);
    /* spawn the daemons */
    fprintf(stderr, "Debugger: spawning %s\n", debugger[0].cmd);
    if (PMIX_SUCCESS != (rc = PMIx_Spawn(dinfo, dninfo, debugger, 1, dspace))) {
        fprintf(stderr, "Debugger daemons failed to launch with error: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_FREE(dinfo, dninfo);
        PMIX_APP_FREE(debugger, 1);
        return rc;
    }
    /* cleanup */
    PMIX_INFO_FREE(dinfo, dninfo);
    PMIX_APP_FREE(debugger, 1);

    /* register callback for when this job terminates */
    myrel->nspace = strdup(dspace);
    PMIX_INFO_CREATE(dinfo, 2);
    PMIX_INFO_LOAD(&dinfo[0], PMIX_EVENT_RETURN_OBJECT, myrel, PMIX_POINTER);
    /* only call me back when this specific job terminates */
    PMIX_LOAD_PROCID(&proc, dspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&dinfo[1], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);
    /* track that we need both jobs to terminate */
    myrel->lock.count++;

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, dinfo, 2,
                                release_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    fprintf(stderr, "Debugger: Registered for termination on nspace %s\n", dspace);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(dinfo, 2);

    return rc;
}

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
    myrel_t myrel;
    pid_t pid;
    pmix_envar_t envar;
    pmix_proc_t proc;
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
        if (0 == strcmp(argv[i], "-c") ||
            0 == strcmp(argv[i], "--cospawn")){
            cospawn_reqd = true;
            break;
        }
    }
    info = NULL;
    ninfo = 0;

    /* use the system connection first, if available */
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, 1))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n", PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, 1);

    fprintf(stderr, "Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank, (unsigned long)pid);

    /* construct my own release first */
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);

    /* register a default event handler */
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, info, 1,
                                notification_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 1);

    /* this is an initial launch - we need to launch the application
     * plus the debugger daemons, letting the RM know we are debugging
     * so that it will "pause" the app procs until we are ready. First
     * we need to know if this RM supports co-spawning of daemons with
     * the application, or if we need to launch the daemons as a separate
     * spawn command. The former is faster and more scalable, but not
     * every RM may support it. We also need to ask for debug support
     * so we know if the RM can stop-on-exec, or only supports stop-in-init */
    nq = 1;
    PMIX_QUERY_CREATE(query, nq);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_SPAWN_SUPPORT);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_DEBUG_SUPPORT);
    /* setup the caddy to retrieve the data */
    DEBUG_CONSTRUCT_LOCK(&myquery_data.lock);
    myquery_data.info = NULL;
    myquery_data.ninfo = 0;
    /* execute the query */
    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, nq, cbfunc, (void*)&myquery_data))) {
        fprintf(stderr, "PMIx_Query_info failed: %d\n", rc);
        goto done;
    }
    DEBUG_WAIT_THREAD(&myquery_data.lock);
    DEBUG_DESTRUCT_LOCK(&myquery_data.lock);

    /* we should have received back two info structs, one containing
     * a comma-delimited list of PMIx spawn attributes the RM supports,
     * and the other containing a comma-delimited list of PMIx debugger
     * attributes it supports */
    if (2 != myquery_data.ninfo) {
        /* this is an error */
        fprintf(stderr, "PMIx Query returned an incorrect number of results: %lu\n", myquery_data.ninfo);
        PMIX_INFO_FREE(myquery_data.info, myquery_data.ninfo);
        goto done;
    }

    /* we would like to co-spawn the debugger daemons with the app, but
     * let's first check to see if this RM supports that operation by
     * looking for the PMIX_COSPAWN_APP attribute in the spawn support
     *
     * We will also check to see if "stop_on_exec" is supported. Few RMs
     * do so, which is why we have to check. The reference server sadly is
     * not one of them, so we shouldn't find it here
     *
     * Note that the PMIx reference server always returns the query results
     * in the same order as the query keys. However, this is not guaranteed,
     * so we should search the returned info structures to find the desired key
     */
    for (n=0; n < myquery_data.ninfo; n++) {
        if (0 == strcmp(myquery_data.info[n].key, PMIX_QUERY_SPAWN_SUPPORT)) {
            /* see if the cospawn attribute is included */
            if (NULL != strstr(myquery_data.info[n].value.data.string, PMIX_COSPAWN_APP)) {
                cospawn = true;
            } else {
                cospawn = false;
            }
        } else if (0 == strcmp(myquery_data.info[n].key, PMIX_QUERY_DEBUG_SUPPORT)) {
            if (NULL != strstr(myquery_data.info[n].value.data.string, PMIX_DEBUG_STOP_ON_EXEC)) {
                stop_on_exec = true;
            } else {
                stop_on_exec = false;
            }
        }
    }

    /* if cospawn is available and they requested it, then we launch both
     * the app and the debugger daemons at the same time */
    if (cospawn && cospawn_reqd) {

    } else {
        /* we must do these as separate launches, so do the app first */
        napps = 1;
        PMIX_APP_CREATE(app, napps);
        /* setup the executable */
        app[0].cmd = strdup("hello");
        PMIX_ARGV_APPEND(rc, app[0].argv, "./hello");
        getcwd(cwd, 1024);  // point us to our current directory
        app[0].cwd = strdup(cwd);
        app[0].maxprocs = 2;
        /* provide job-level directives so the apps do what the user requested */
        ninfo = 6;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "slot", PMIX_STRING);  // map by slot
        if (stop_on_exec) {
            PMIX_INFO_LOAD(&info[1], PMIX_DEBUG_STOP_ON_EXEC, NULL, PMIX_BOOL);  // procs are to stop on first instruction
        } else {
            PMIX_INFO_LOAD(&info[1], PMIX_DEBUG_STOP_IN_INIT, NULL, PMIX_BOOL);  // procs are to pause in PMIx_Init for debugger attach
        }
        PMIX_INFO_LOAD(&info[2], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);  // forward stdout to me
        PMIX_INFO_LOAD(&info[3], PMIX_FWD_STDERR, NULL, PMIX_BOOL);  // forward stderr to me
        PMIX_INFO_LOAD(&info[4], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the job completes
        PMIX_INFO_LOAD(&info[5], PMIX_SETUP_APP_ENVARS, NULL, PMIX_BOOL);

        /* spawn the job - the function will return when the app
         * has been launched */
        fprintf(stderr, "Debugger: spawning %s\n", app[0].cmd);
        if (PMIX_SUCCESS != (rc = PMIx_Spawn(info, ninfo, app, napps, clientspace))) {
            fprintf(stderr, "Application failed to launch with error: %s(%d)\n", PMIx_Error_string(rc), rc);
            goto done;
        }
        PMIX_INFO_FREE(info, ninfo);
        PMIX_APP_FREE(app, napps);

        /* register callback for when the app terminates */
        PMIX_INFO_CREATE(info, 2);
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
        /* only call me back when this specific job terminates */
        PMIX_LOAD_PROCID(&proc, clientspace, PMIX_RANK_WILDCARD);
        PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);
        /* track number of jobs to terminate */
        myrel.lock.count++;

        DEBUG_CONSTRUCT_LOCK(&mylock);
        PMIx_Register_event_handler(&code, 1, info, 2,
                                    release_fn, evhandler_reg_callbk, (void*)&mylock);
        DEBUG_WAIT_THREAD(&mylock);
        fprintf(stderr, "Debugger: Registered for termination on nspace %s\n", clientspace);
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIX_INFO_FREE(info, 2);

        /* get the proctable for this nspace */
        PMIX_QUERY_CREATE(query, 1);
        PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_PROC_TABLE);
        query[0].nqual = 1;
        PMIX_INFO_CREATE(query->qualifiers, query[0].nqual);
        PMIX_INFO_LOAD(&query->qualifiers[0], PMIX_NSPACE, clientspace, PMIX_STRING);

        DEBUG_CONSTRUCT_LOCK(&myquery_data.lock);
        myquery_data.info = NULL;
        myquery_data.ninfo = 0;

        if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, 1, cbfunc, (void*)&myquery_data))) {
            fprintf(stderr, "Debugger[%s:%d] Proctable query failed: %d\n", myproc.nspace, myproc.rank, rc);
            goto done;
        }
        /* wait to get a response */
        DEBUG_WAIT_THREAD(&myquery_data.lock);
        DEBUG_DESTRUCT_LOCK(&myquery_data.lock);
        /* we should have gotten a response */
        if (PMIX_SUCCESS != myquery_data.status) {
            fprintf(stderr, "Debugger[%s:%d] Proctable query failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(myquery_data.status));
            goto done;
        }
        /* there should have been data */
        if (NULL == myquery_data.info || 0 == myquery_data.ninfo) {
            fprintf(stderr, "Debugger[%s:%d] Proctable query return no results\n",
                    myproc.nspace, myproc.rank);
            goto done;
        }
        /* the query should have returned a data_array */
        if (PMIX_DATA_ARRAY != myquery_data.info[0].value.type) {
            fprintf(stderr, "Debugger[%s:%d] Query returned incorrect data type: %s(%d)\n",
                    myproc.nspace, myproc.rank,
                    PMIx_Data_type_string(myquery_data.info[0].value.type),
                    (int)myquery_data.info[0].value.type);
            return -1;
        }
        if (NULL == myquery_data.info[0].value.data.darray->array) {
            fprintf(stderr, "Debugger[%s:%d] Query returned no proctable info\n");
            goto done;
        }
        /* the data array consists of a struct:
         *     size_t size;
         *     void* array;
         *
         * In this case, the array is composed of pmix_proc_info_t structs:
         *     pmix_proc_t proc;   // contains the nspace,rank of this proc
         *     char* hostname;
         *     char* executable_name;
         *     pid_t pid;
         *     int exit_code;
         *     pmix_proc_state_t state;
         */
        fprintf(stderr, "Received proc table for %d procs\n", (int)myquery_data.info[0].value.data.darray->size);
        /* now launch the debugger daemons */
        if (PMIX_SUCCESS != (rc = spawn_debugger(clientspace, &myrel))) {
            fprintf(stderr, "Debugger daemons failed to spawn: %s\n", PMIx_Error_string(rc));
            goto done;
        }
    }

  rundebugger:
    /* this is where a debugger tool would wait until the debug operation is complete */
    DEBUG_WAIT_THREAD(&myrel.lock);

  done:
    DEBUG_DESTRUCT_LOCK(&myrel.lock);
    PMIx_tool_finalize();
    return(rc);
}
