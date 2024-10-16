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
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <pmix.h>

static pmix_proc_t myproc;


void* myThread(void* vargp)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_value_t *val;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, 1);
    fprintf(stderr, "Fetching remote\n");
    rc = PMIx_Get(&proc, "remote", NULL, 0, &val);
    fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s returned: %s(%d)\n",
            myproc.nspace, myproc.rank, "foobar", PMIx_Error_string(rc), rc);
    return NULL;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t value;
    pmix_value_t *val = &value;
    char *tmp;
    pmix_proc_t proc;
    uint32_t nprocs, n, k;
    pmix_info_t *info, i2;
    bool flag;
    pthread_t thread_id[4];

    /* init us - note that the call to "init" includes the return of
     * any job-related info provided by the RM. This includes the
     * location of all procs in our job */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        exit(0);
    }
    fprintf(stderr, "Client ns %s rank %d Running\n", myproc.nspace, myproc.rank);

    value.type = PMIX_UINT64;
    value.data.uint64 = 1234;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, "local", &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put internal failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }

    value.type = PMIX_UINT64;
    value.data.uint64 = 5678;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, "remote", &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put internal failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }

    /* push the data to our PMIx server */
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }

    /* call fence to synchronize with our peers - instruct
     * the fence operation to collect and return all "put"
     * data from our peers */
    PMIX_INFO_CREATE(info, 1);
    flag = true;
    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, info, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        goto done;
    }
    PMIX_INFO_FREE(info, 1);

    // spawn a few threads - each thread attempts to get data
    // published by a peer
    if (0 == myproc.rank) {
        fprintf(stderr, "%u: SPAWNING THREADS\n", myproc.rank);
        for (n=0; n < 4; n++) {
            pthread_create(&thread_id[n], NULL, myThread, NULL);
        }
        // collect them all
        for (n=0; n < 4; n++) {
            pthread_join(thread_id[n], NULL);
        }
    }

done:
    /* finalize us */
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace,
                myproc.rank, rc);
    }
    fflush(stderr);
    return (0);
}
