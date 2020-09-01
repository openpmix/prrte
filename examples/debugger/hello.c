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
 * Copyright (c) 2013-2018 Intel, Inc. All rights reserved.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>

#include <pmix.h>


static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pid_t pid;
    char hostname[1024];
    pmix_value_t *val;
    uint16_t localrank;
    int spin = 0;
    pmix_proc_t wildproc;

    pid = getpid();
    gethostname(hostname, 1024);

    if (1 < argc) {
        if (isdigit(argv[1][0]) && '0' != argv[1][0]) {
            spin = strtoul(argv[1], NULL, 10);
        } else {
            fprintf(stderr, "Error: 'spin' must be a whole number greater than 0.\n");
            fprintf(stderr, "Usage: %s [spin] [] []\n", argv[0]);
            exit(1);
        }
    }

    /* init us - note that the call to "init" includes the return of
     * any job-related info provided by the RM. This includes any
     * debugger flag instructing us to stop-in-init. If such a directive
     * is included, then the process will be stopped in this call until
     * the "debugger release" notification arrives */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        exit(0);
    }
    /* get our local rank */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, PMIX_LOCAL_RANK, NULL, 0,
                                       &val))) {
        fprintf(stderr,
                "Client ns %s rank %d: PMIx_Get local rank failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    localrank = val->data.uint16;
    PMIX_VALUE_RELEASE(val);

    printf("Client ns %s rank %d pid %lu: Running on host %s localrank %d\n",
            myproc.nspace, myproc.rank, (unsigned long)pid, hostname ,
            (int)localrank);

    // 0 arguments then do nothing.
    if (1 == argc) {
        ;
    }
    // 1 argument then everyone sleeps for a bit then finalizes
    else if (2 == argc) {
        if (0 < spin) {
            sleep(spin);
        }
    }
    // 2 arguments then rank 0 waits in a spin loop, others pause and go to finalize
    else if (3 == argc) {
        if (0 == myproc.rank) {
            spin = 1;
            while (0 < spin) {
                sleep(1);
            }
        } else {
            if (0 < spin) {
                sleep(spin);
            }
        }
    }
    // Otherwise then rank 0 waits in a spin loop, others block in the fence
    else {
        if (0 == myproc.rank) {
            spin = 1;
            while (0 < spin) {
                sleep(1);
            }
        }

        // Fence to hold all processes
        PMIX_PROC_CONSTRUCT(&wildproc);
        (void)strncpy(wildproc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
        wildproc.rank = PMIX_RANK_WILDCARD;

        PMIx_Fence(&wildproc, 1, NULL, 0);
    }

  done:
    /* finalize us */
    printf("Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    } else {
        printf("Client ns %s rank %d:PMIx_Finalize successfully completed\n",
               myproc.nspace, myproc.rank);
    }
    fclose(stderr);
    fclose(stdout);
    exit(0);
}
