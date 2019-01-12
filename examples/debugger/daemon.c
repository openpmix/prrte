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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdbool.h>
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

    /* save the returned info - it will be
     * released in the release_fn */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            fprintf(stderr, "Transferring %s\n", info[n].key);
            PMIX_INFO_XFER(&mq->info[n], &info[n]);
        }
    }

    /* let the library release the data */
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

    fprintf(stderr, "DEBUGGER DAEMON %s NOTIFIED THAT JOB TERMINATED - AFFECTED %s\n", lock->nspace,
            (NULL == affected) ? "NULL" : affected->nspace);

    if (found) {
        lock->exit_code = exit_code;
        lock->exit_code_given = true;
    }

    /* tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }

    DEBUG_WAKEUP_THREAD(&lock->lock);
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

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val;
    pmix_proc_t proc;
    pmix_info_t *info;
    size_t ninfo;
    pmix_query_t *query;
    size_t nq, n;
    myquery_data_t myquery_data;
    pid_t pid;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    mylock_t mylock;
    myrel_t myrel;
    uint16_t localrank;
    char *target = NULL;

    pid = getpid();

    /* init us - since we were launched by the RM, our connection info
     * will have been provided at startup. */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, NULL, 0))) {
        fprintf(stderr, "Debugger daemon: PMIx_tool_init failed: %d\n", rc);
        exit(0);
    }
    fprintf(stderr, "Debugger daemon ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank, (unsigned long)pid);


    /* register our default event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* get the nspace of the job we are to debug - it will be in our JOB info */
#ifdef PMIX_LOAD_PROCID
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
#else
    PMIX_PROC_CONSTRUCT(&proc);
    (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_KEYLEN);
    proc.rank = PMIX_RANK_WILDCARD;
#endif
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_DEBUG_JOB, NULL, 0, &val))) {
        fprintf(stderr, "[%s:%d:%lu] Failed to get job being debugged - error %s\n",
                myproc.nspace, myproc.rank,
                (unsigned long)pid, PMIx_Error_string(rc));
        goto done;
    }
    if (NULL == val || PMIX_STRING != val->type || NULL == val->data.string) {
        fprintf(stderr, "[%s:%d:%lu] Failed to get job being debugged - NULL data returned\n",
                myproc.nspace, myproc.rank, (unsigned long)pid);
        goto done;
    }
    /* save it for later */
    target = strdup(val->data.string);
    PMIX_VALUE_RELEASE(val);

    fprintf(stderr, "[%s:%d:%lu] Debugging %s\n", myproc.nspace, myproc.rank,
            (unsigned long)pid, target);

    /* get my local rank so I can determine which local proc is "mine"
     * to debug */
    val = NULL;
    if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, PMIX_LOCAL_RANK, NULL, 0, &val))) {
        fprintf(stderr, "[%s:%d:%lu] Failed to get my local rank - error %s\n",
                myproc.nspace, myproc.rank,
                (unsigned long)pid, PMIx_Error_string(rc));
        goto done;
    }
    if (NULL == val) {
        fprintf(stderr, "[%s:%d:%lu] Failed to get my local rank - NULL data returned\n",
                myproc.nspace, myproc.rank, (unsigned long)pid);
        goto done;
    }
    if (PMIX_UINT16 != val->type) {
        fprintf(stderr, "[%s:%d:%lu] Failed to get my local rank - returned wrong type %s\n",
                myproc.nspace, myproc.rank, (unsigned long)pid, PMIx_Data_type_string(val->type));
        goto done;
    }
    /* save the data */
    localrank = val->data.uint16;
    PMIX_VALUE_RELEASE(val);
    fprintf(stderr, "[%s:%d:%lu] my local rank %d\n", myproc.nspace, myproc.rank,
            (unsigned long)pid, (int)localrank);

    /* register another handler specifically for when the target
     * job completes */
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);
    myrel.nspace = strdup(proc.nspace);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
    /* only call me back when this specific job terminates */
    PMIX_LOAD_PROCID(&proc, target, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);

    fprintf(stderr, "[%s:%d:%lu] registering for termination of %s\n", myproc.nspace, myproc.rank,
            (unsigned long)pid, proc.nspace);


    DEBUG_CONSTRUCT_LOCK(&mylock);
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

    /* get our local proctable - for scalability reasons, we don't want to
     * have our "root" debugger process get the proctable for everybody and
     * send it out to us. So ask the local PMIx server for the pid's of
     * our local target processes */
    nq = 1;
    PMIX_QUERY_CREATE(query, nq);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_LOCAL_PROC_TABLE);
    query[0].nqual = 1;
    PMIX_INFO_CREATE(query[0].qualifiers, 1);
    PMIX_INFO_LOAD(&query[0].qualifiers[0], PMIX_NSPACE, target, PMIX_STRING);  // the nspace we are enquiring about
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
    PMIX_QUERY_FREE(query, nq);
    if (PMIX_SUCCESS != myquery_data.status) {
        rc = myquery_data.status;
        goto done;
    }

    fprintf(stderr, "[%s:%d:%lu] Local proctable received\n", myproc.nspace, myproc.rank, (unsigned long)pid);


    /* now that we have the proctable for our local processes, we can do our
     * magic debugger stuff and attach to them. We then send a "release" event
     * to them - i.e., it's the equivalent to setting the MPIR breakpoint. We
     * do this with the event notification system. For this example, we just
     * send it to all local procs of the job being debugged */
    (void)strncpy(proc.nspace, target, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);  // deliver to the target nspace
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);  // deliver to the target nspace
    fprintf(stderr, "[%s:%u:%lu] Sending release\n", myproc.nspace, myproc.rank, (unsigned long)pid);
    rc = PMIx_Notify_event(PMIX_ERR_DEBUGGER_RELEASE,
                           NULL, PMIX_RANGE_CUSTOM,
                           info, ninfo, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s[%s:%u:%lu] Sending release failed with error %s(%d)\n",
                myproc.nspace, myproc.rank, (unsigned long)pid, PMIx_Error_string(rc), rc);
        goto done;
    }

    /* do some debugger magic while waiting for the job to terminate */
    DEBUG_WAIT_THREAD(&myrel.lock);

  done:
    if (NULL != target) {
        free(target);
    }
    /* finalize us */
    fprintf(stderr, "Debugger daemon ns %s rank %d pid %lu: Finalizing\n", myproc.nspace, myproc.rank, (unsigned long)pid);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Debugger daemon ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace, myproc.rank, rc);
    } else {
        fprintf(stderr, "Debugger daemon ns %s rank %d pid %lu:PMIx_Finalize successfully completed\n", myproc.nspace, myproc.rank, (unsigned long)pid);
    }
    fflush(stderr);
    return(0);
}
