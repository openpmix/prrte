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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "examples.h"
#include <pmix_tool.h>

static void cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                   pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t *) cbdata;
    size_t n;

    mq->lock.status = status;

    /* save the returned info - it will be
     * released in the release_fn */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n = 0; n < ninfo; n++) {
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

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t myproc, target;
    size_t ninfo = 0, n;
    pmix_info_t *info = NULL;
    char *nspace = NULL, *server = NULL, *mynspace = NULL;
    pid_t pid = -1;

    for (n = 1; n < (size_t) argc; n++) {
        if (0 == strcmp("-p", argv[n]) || 0 == strcmp("--pid", argv[n])) {
            if (NULL == argv[n + 1]) {
                fprintf(stderr, "Must provide PID argument to %s option\n", argv[n]);
                exit(1);
            }
            pid = strtol(argv[n + 1], NULL, 10);
        } else if (0 == strcmp("-nspace", argv[n]) || 0 == strcmp("--nspace", argv[n])) {
            if (NULL == argv[n + 1]) {
                fprintf(stderr, "Must provide nspace argument to %s option\n", argv[n]);
                exit(1);
            }
            nspace = argv[n + 1];
        } else if (0 == strcmp("-server", argv[n]) || 0 == strcmp("--server", argv[n])) {
            if (NULL == argv[n + 1]) {
                fprintf(stderr, "Must provide nspace argument to %s option\n", argv[n]);
                exit(1);
            }
            server = argv[n + 1];
        } else if (0 == strcmp("-mynspace", argv[n]) || 0 == strcmp("--mynspace", argv[n])) {
            if (NULL == argv[n + 1]) {
                fprintf(stderr, "Must provide nspace argument to %s option\n", argv[n]);
                exit(1);
            }
            mynspace = argv[n + 1];
        }
    }
    if (NULL == nspace && NULL == mynspace) {
        fprintf(stderr, "Must provide nspace\n");
        exit(1);
    }
    if (NULL != mynspace) {
        setenv("PMIX_NAMESPACE", mynspace, true);
        setenv("PMIX_RANK", "0", true);
        nspace = mynspace;
    }

    if (0 < pid) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
    } else if (NULL != server) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, server, PMIX_STRING);
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        exit(rc);
    }
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }

    // abort the provided nspace
    PMIx_Load_procid(&target, nspace, PMIX_RANK_WILDCARD);
    fprintf(stderr, "Aborting %s:%u\n", target.nspace, target.rank);
    rc = PMIx_Abort(PMIX_ERROR, "DIE HUMAN SCUM", &target, 1);
    fprintf(stderr, "Abort returned: %s\n", PMIx_Error_string(rc));


    /* finalize us */
    PMIx_tool_finalize();
    return (rc);
}
