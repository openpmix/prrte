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
 * Copyright (c) 2013-2016 Intel, Inc.  All rights reserved.
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

#include <pmix.h>

static volatile bool waiting_for_debugger = true;
static pmix_proc_t myproc;

static void notification_fn(size_t evhdlr_registration_id,
                            pmix_status_t status,
                            const pmix_proc_t *source,
                            pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc,
                            void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void release_fn(size_t evhdlr_registration_id,
                       pmix_status_t status,
                       const pmix_proc_t *source,
                       pmix_info_t info[], size_t ninfo,
                       pmix_info_t results[], size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    waiting_for_debugger = false;
}

static void evhandler_reg_callbk(pmix_status_t status,
                                 size_t evhandler_ref,
                                 void *cbdata)
{
    volatile bool *active = (volatile bool*)cbdata;

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                   myproc.nspace, myproc.rank, status, (unsigned long)evhandler_ref);
    }
    *active = status;
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t value;
    pmix_value_t *val = &value;
    char *tmp;
    pmix_proc_t proc;
    uint32_t nprocs, n;
    pmix_info_t *info;
    bool flag;
    volatile int active;
    pmix_status_t dbg = PMIX_ERR_DEBUGGER_RELEASE;

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank, rc);
        exit(0);
    }
    fprintf(stderr, "Client ns %s rank %d: Running\n", myproc.nspace, myproc.rank);


    /* register our event handler */
    active = -1;
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, evhandler_reg_callbk, (void*)&active);
    while (-1 == active) {
        sleep(1);
    }
    if (0 != active) {
        exit(active);
    }

    /* if we were told to stop for debugger, do it here */
    if (NULL != getenv("PMIX_STOP_FOR_DEBUGGER")) {
        /* register for debugger release */
        active = -1;
        PMIx_Register_event_handler(&dbg, 1, NULL, 0,
                                    release_fn, evhandler_reg_callbk, (void*)&active);
        /* wait for registration to complete */
        while (-1 == active) {
            sleep(1);
        }
        if (0 != active) {
            exit(active);
        }
        /* wait for debugger release */
        while (waiting_for_debugger) {
            sleep(1);
        }
    }

    PMIX_PROC_CONSTRUCT(&proc);
    (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;

    /* get our universe size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_UNIV_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get universe size failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    fprintf(stderr, "Client %s:%d universe size %d\n", myproc.nspace, myproc.rank, nprocs);

    /* put a few values */
    if (0 > asprintf(&tmp, "%s-%d-internal", myproc.nspace, myproc.rank)) {
        exit(1);
    }
    value.type = PMIX_UINT32;
    value.data.uint32 = 1234;
    if (PMIX_SUCCESS != (rc = PMIx_Store_internal(&myproc, tmp, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Store_internal failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    free(tmp);

    if (0 > asprintf(&tmp, "%s-%d-local", myproc.nspace, myproc.rank)) {
        exit(1);
    }
    value.type = PMIX_UINT64;
    value.data.uint64 = 1234;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, tmp, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put internal failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    free(tmp);

    if (0 > asprintf(&tmp, "%s-%d-remote", myproc.nspace, myproc.rank)) {
        exit(1);
    }
    value.type = PMIX_STRING;
    value.data.string = "1234";
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, tmp, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put internal failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    free(tmp);

    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }

    /* call fence to ensure the data is received */
    PMIX_INFO_CREATE(info, 1);
    flag = true;
    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, info, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    PMIX_INFO_FREE(info, 1);

    /* check the returned data */
    for (n=0; n < nprocs; n++) {
        if (0 > asprintf(&tmp, "%s-%d-local", myproc.nspace, myproc.rank)) {
            exit(1);
        }
        if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, tmp, NULL, 0, &val))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s failed: %d\n", myproc.nspace, myproc.rank, tmp, rc);
            goto done;
        }
        if (PMIX_UINT64 != val->type) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned wrong type: %d\n", myproc.nspace, myproc.rank, tmp, val->type);
            PMIX_VALUE_RELEASE(val);
            free(tmp);
            goto done;
        }
        if (1234 != val->data.uint64) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned wrong value: %d\n", myproc.nspace, myproc.rank, tmp, (int)val->data.uint64);
            PMIX_VALUE_RELEASE(val);
            free(tmp);
            goto done;
        }
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned correct\n", myproc.nspace, myproc.rank, tmp);
        PMIX_VALUE_RELEASE(val);
        free(tmp);
        if (0 > asprintf(&tmp, "%s-%d-remote", myproc.nspace, myproc.rank)) {
            exit(1);
        }
        if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, tmp, NULL, 0, &val))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s failed: %d\n", myproc.nspace, myproc.rank, tmp, rc);
            goto done;
        }
        if (PMIX_STRING != val->type) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned wrong type: %d\n", myproc.nspace, myproc.rank, tmp, val->type);
            PMIX_VALUE_RELEASE(val);
            free(tmp);
            goto done;
        }
        if (0 != strcmp(val->data.string, "1234")) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned wrong value: %s\n", myproc.nspace, myproc.rank, tmp, val->data.string);
            PMIX_VALUE_RELEASE(val);
            free(tmp);
            goto done;
        }
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned correct\n", myproc.nspace, myproc.rank, tmp);
        PMIX_VALUE_RELEASE(val);
        free(tmp);
    }

 done:
    /* finalize us */
    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace, myproc.rank, rc);
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n", myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return(0);
}
