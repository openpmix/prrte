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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <pmix_tool.h>

static int attach_to_running_job(char *nspace);

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_info_t *info;
    pmix_app_t *app;
    size_t ninfo, napps;
    char *tdir, *nspace = NULL, *dspace = NULL;
    int i;
    uint64_t u64;

    /* Process any arguments we were given */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "-h") ||
            0 == strcmp(argv[i], "--help")) {
            /* print the usage message and exit */

        }
        if (0 == strcmp(argv[i], "-a") ||
            0 == strcmp(argv[i], "--attach")) {
            if (NULL != nspace) {
                /* can only support one */
                fprintf(stderr, "Cannot attach to more than one nspace\n");
                exit(1);
            }
            /* the next argument must be the nspace */
            ++i;
            if (argc == i) {
                /* they goofed */
                fprintf(stderr, "The %s option requires an <nspace> argument\n", argv[i]);
                exit(1);
            }
            nspace = strdup(argv[i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            exit(1);
        }
    }

    /* we need to provide some info to the PMIx tool library so
     * it can find the server's contact info. The simplest way
     * of doing this here is to look for an environmental variable
     * that tells us where to look. The PMIx reference server only
     * allows one instantiation of the server per user, so setting
     * this up is something a user could do in their login script.
     * The reference server is based on OpenMPI, and so the contact
     * info will always be found at:
     *
     * $TMPDIR/ompi.<nodename>.<numerical-userid>/dvm
     */

    if (NULL == (tdir = getenv("PMIX_SERVER_TMPDIR"))) {
        fprintf(stderr, "Tool usage requires that the PMIX_SERVER_TMPDIR envar\n");
        fprintf(stderr, "be set to point at the directory where the PMIx Reference\n");
        fprintf(stderr, "Server leaves its contact info file.\n");
        exit(1);
    }

    /* init us - pass along the location of the contact file */
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TMPDIR, tdir, PMIX_STRING);

    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %d\n", rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    fprintf(stderr, "Tool ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

    /* if we are attaching to a running job, then attach to it */
    if (NULL != nspace) {
        if (PMIX_SUCCESS != (rc = attach_to_running_job(nspace))) {
            fprintf(stderr, "Failed to attach to nspace %s: error code %d\n",
                    nspace, rc);
            goto done;
        }
    } else {
        /* this is an initial launch - we need to launch the application
         * plus the debugger daemons, letting the RM know we are debugging
         * so that it will "pause" the app procs until we are ready */
        napps = 2;
        PMIX_APP_CREATE(app, napps);
        /* setup the executable */
        app[0].cmd = strdup("client");
        app[0].argc = 1;
        app[0].argv = (char**)malloc(2*sizeof(char*));
        app[0].argv[0] = strdup("client");
        app[0].argv[1] = NULL;
        /* provide directives so the apps do what the user requested */
        ninfo = 2;
        PMIX_INFO_CREATE(app[0].info, ninfo);
        u64 = 2;
        PMIX_INFO_LOAD(&app[0].info[0], PMIX_JOB_SIZE, &u64, PMIX_UINT64);
        PMIX_INFO_LOAD(&app[0].info[1], PMIX_MAPBY, "slot", PMIX_STRING);

        /* setup the name of the daemon executable to launch */
        app[1].cmd = strdup("debuggerd");
        app[1].argc = 1;
        app[1].argv = (char**)malloc(2*sizeof(char*));
        app[1].argv[0] = strdup("debuggerd");
        app[1].argv[1] = NULL;
        /* provide directives so the daemons go where we want, and
         * let the RM know these are debugger daemons */
        ninfo = 2;
        PMIX_INFO_CREATE(app[1].info, ninfo);
        PMIX_INFO_LOAD(&app[1].info[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
        PMIX_INFO_LOAD(&app[1].info[1], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL); // these are debugger daemons
        /* spawn the daemons */
        rc = PMIx_Spawn(NULL, 0, app, napps, dspace);
        /* cleanup */
        PMIX_APP_FREE(app, napps);

        /* this is where a debugger tool would wait until the debug operation is complete */
    }


 done:
    PMIx_tool_finalize();

    return(rc);
}

static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t release_fn,
                       void *release_cbdata)
{
    volatile bool *active = (volatile bool*)cbdata;


}

static int attach_to_running_job(char *nspace)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_query_t *query;
    pmix_app_t *app;
    size_t nq, napps;
    volatile bool active;

    /* query the active nspaces so we can verify that the
     * specified one exists */
    nq = 1;
    PMIX_QUERY_CREATE(query, nq);
    query[0].keys = (char**)malloc(2 * sizeof(char*));
    query[0].keys[0] = strdup(PMIX_QUERY_NAMESPACES);
    query[0].keys[1] = NULL;

    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, nq, infocbfunc, (void*)&active))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info failed: %d\n", myproc.nspace, myproc.rank, rc);
        return -1;
    }
    /* wait for a response */
    while (active) {
        sleep(1);
    }

    /* the query should have returned a comma-delimited list of nspaces */
    if (PMIX_STRING != info[0].type) {
        fprintf(stderr, "Query returned incorrect data type: %d\n", info[0].type);
        return -1;
    }
    if (NULL == info[0].data.string) {
        fprintf(stderr, "Query returned no active nspaces\n");
        return -1;
    }
    /* split the returned string and look for the given nspace */

    /* if not found, then we have an error */
    PMIX_INFO_FREE(info, ninfo);

    /* get the proctable for this nspace */
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    (void)strncpy(info[0].key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
    (void)strncpy(info[0].qualifier, nspace, PMIX_MAX_KEYLEN);
    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(info, ninfo, infocbfunc, (void*)&active))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info_nb failed: %d\n", myproc.nspace, myproc.rank, rc);
        return -1;
    }
    /* wait to get a response */

    /* the query should have returned a data_array */
    if (PMIX_DATA_ARRAY != info[0].type) {
        fprintf(stderr, "Query returned incorrect data type: %d\n", info[0].type);
        return -1;
    }
    if (NULL == info[0].data.darray.array) {
        fprintf(stderr, "Query returned no proctable info\n");
        return -1;
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

    /* this is where a debugger tool would process the proctable to
     * create whatever blob it needs to provide to its daemons */
    PMIX_INFO_FREE(info, ninfo);

    /* setup the debugger daemon spawn request */
    napps = 1;
    PMIX_APP_CREATE(app, napps);
    /* setup the name of the daemon executable to launch */
    app[0].cmd = strdup("debuggerdaemon");
    app[0].argc = 1;
    app[0].argv = (char**)malloc(2*sizeof(char*));
    app[0].argv[0] = strdup("debuggerdaemon");
    app[0].argv[1] = NULL;
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    ninfo = 3;
    PMIX_INFO_CREATE(app[0].info, ninfo);
    PMIX_INFO_LOAD(&app[0].info[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
    PMIX_INFO_LOAD(&app[0].info[1], PMIX_DEBUGGER_DAEMONS, true, PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LOAD(&app[0].info[2], PMIX_DEBUG_TARGET, nspace, PMIX_STRING); // the "jobid" of the application to be debugged

    /* spawn the daemons */
    PMIx_Spawn(NULL, 0, app, napps, dspace);
    /* cleanup */
    PMIX_APP_FREE(app, napps);

    /* this is where a debugger tool would wait until the debug operation is complete */

    return 0;
}
