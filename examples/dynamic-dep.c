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
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdbool.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;

int main(int argc, char **argv)
{

#ifndef PMIX_SPAWN_CHILD_SEP
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stderr, "PMIX_SPAWN_CHILD_SEP is not available - this example is not supported\n");
    return -1;
#else

    int rc, exitcode;
    char nsp2[PMIX_MAX_NSLEN + 1];
    pmix_proc_t proc;
    pmix_app_t *app;
    bool test_dep = true;
    pmix_info_t info;

    if (1 < argc) {
        if (0 == strcmp(argv[1], "dep")) {
            test_dep = false;
        }
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        exit(rc);
    }

    /* rank=0 calls spawn */
    if (0 == myproc.rank) {
        PMIX_APP_CREATE(app, 1);
        app->cmd = strdup("sleep");
        app->maxprocs = 2;
        PMIX_ARGV_APPEND(rc, app->argv, app->cmd);
        PMIX_ARGV_APPEND(rc, app->argv, "5");

        PMIX_INFO_LOAD(&info, PMIX_SPAWN_CHILD_SEP, &test_dep, PMIX_BOOL);
        rc = PMIx_Spawn(&info, 1, app, 1, nsp2);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Spawn failed: %s(%d)\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc), rc);
            exitcode = rc;
            /* terminate our peers */
            PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
            PMIx_Abort(rc, "FAILED TO START CHILD JOB", &proc, 1);
            goto done;
        } else {
          fprintf(stderr, "Spawn success.\n");
        }
        PMIX_APP_FREE(app, 1);

        // now terminate
    }


done:
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace,
                myproc.rank, rc);
    }

    fflush(stderr);
    return (0);
#endif
}
