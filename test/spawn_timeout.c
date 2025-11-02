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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
    int rc, exitcode;
    pmix_value_t value;
    pmix_value_t *val = &value;
    char nsp2[PMIX_MAX_NSLEN + 1];
    pmix_app_t *app;
    char hostname[1024], dir[1024];

    if (0 > gethostname(hostname, sizeof(hostname))) {
        exit(1);
    }
    if (NULL == getcwd(dir, 1024)) {
        exit(1);
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        exit(rc);
    }
    fprintf(stderr, "Client ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

    rc = PMIx_Get(&myproc, PMIX_PARENT_ID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        // we are the parent
        PMIX_APP_CREATE(app, 1);
        if (0 > asprintf(&app->cmd, "%s/%s", dir, argv[0])) {
            exitcode = 1;
            goto done;
        }
        app->maxprocs = 1;
        PMIX_ARGV_APPEND(rc, app->argv, app->cmd);

        fprintf(stderr, "Client ns %s rank %d: calling PMIx_Spawn\n", myproc.nspace, myproc.rank);
        if (PMIX_SUCCESS != (rc = PMIx_Spawn(NULL, 0, app, 1, nsp2))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Spawn failed: %s(%d)\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc), rc);
            exitcode = rc;
            goto done;
        } else {
          fprintf(stderr, "Spawn success.\n");
        }
        PMIX_APP_FREE(app, 1);
    }

    sleep(15);

done:
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace,
                myproc.rank, rc);
    }
    fflush(stderr);
    printf("exit\n");
    return (exitcode);
}
