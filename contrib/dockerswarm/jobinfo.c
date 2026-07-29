/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * jobinfo -- a minimal PMIx client for exercising the daemon's direct-modex
 * paths (src/prted/pmix/pmix_server_fence.c) across nodes.
 *
 *   jobinfo publish <seconds>
 *       Print "NSPACE <ns>" and stay alive.  Used as the *target* job: run it
 *       pinned to one node so that every other daemon in the DVM knows about
 *       the job but hosts none of its processes.
 *
 *   jobinfo fetch <nspace> [seconds]
 *       PMIx_Get(<nspace>, PMIX_RANK_WILDCARD, PMIX_JOB_SIZE) -- a request for
 *       another job's JOB-LEVEL data.  Print "JOBSIZE <n>" on success.
 *
 *   jobinfo fetchkey <nspace> <rank> <key> [seconds]
 *       PMIx_Get of a specific remote rank's key, with PMIX_REQUIRED_KEY so
 *       the hosting daemon waits for the key rather than answering "no".
 *       Used to *park* a request in the local daemon's request array for the
 *       duration; see the swarm test for why that matters.
 *
 * Why this exists: a wildcard (job-level) direct-modex is answered locally --
 * the daemon already has the job data, it just has to register the nspace with
 * its PMIx server.  Before it gets there it checks whether some other request
 * for the same target is already in flight, and that check used to compare
 * against the wrong field of the request tracker.  Because an unset target is
 * {"", PMIX_RANK_INVALID}, and PMIx treats an empty nspace as matching
 * anything and PMIX_RANK_WILDCARD as matching anything, a wildcard request
 * "matched" the first unrelated entry in the array, was parked as
 * already-requested, and was never answered.  Reproducing that needs two
 * clients of the SAME daemon and a job whose procs live somewhere else --
 * i.e. a real multi-node DVM.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;

static int do_publish(int seconds)
{
    pmix_status_t rc;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* the harness scrapes this line to learn the nspace it has to ask about */
    printf("NSPACE %s\n", myproc.nspace);
    fflush(stdout);

    sleep(seconds);

    PMIx_Finalize(NULL, 0);
    return 0;
}

static int do_fetch(const char *nspace, int seconds)
{
    pmix_status_t rc;
    pmix_proc_t target;
    pmix_value_t *val = NULL;
    pmix_info_t info;
    uint32_t jobsize = 0;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* rank=WILDCARD means "the job-level data for this job" */
    PMIX_LOAD_PROCID(&target, nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_TIMEOUT, &seconds, PMIX_INT);

    rc = PMIx_Get(&target, PMIX_JOB_SIZE, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc || NULL == val) {
        fprintf(stderr, "ERROR PMIx_Get(JOB_SIZE of %s): %s\n",
                nspace, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    PMIx_Value_get_number(val, (void *) &jobsize, PMIX_UINT32);
    PMIX_VALUE_RELEASE(val);

    printf("JOBSIZE %u\n", jobsize);
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return 0;
}

static int do_fetchkey(const char *nspace, unsigned int rank, const char *key, int seconds)
{
    pmix_status_t rc;
    pmix_proc_t target;
    pmix_value_t *val = NULL;
    pmix_info_t info[2];

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_LOAD_PROCID(&target, nspace, (pmix_rank_t) rank);
    /* REQUIRED_KEY makes the hosting daemon hold the request until the key
     * shows up, which is what keeps a tracker parked in OUR daemon's request
     * array for the duration */
    PMIX_INFO_LOAD(&info[0], PMIX_REQUIRED_KEY, key, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_TIMEOUT, &seconds, PMIX_INT);

    printf("FETCHKEY-STARTED\n");
    fflush(stdout);

    rc = PMIx_Get(&target, key, info, 2, &val);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    /* we fully expect this to end in a timeout or not-found - the point was
     * to occupy a request slot, not to get an answer */
    printf("FETCHKEY-DONE %s\n", PMIx_Error_string(rc));
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (2 > argc) {
        fprintf(stderr, "usage: %s publish <secs> | fetch <nspace> [secs] |"
                        " fetchkey <nspace> <rank> <key> [secs]\n", argv[0]);
        return 2;
    }

    if (0 == strcmp(argv[1], "publish")) {
        return do_publish((3 > argc) ? 120 : atoi(argv[2]));
    }
    if (0 == strcmp(argv[1], "fetch")) {
        if (3 > argc) {
            fprintf(stderr, "fetch needs an nspace\n");
            return 2;
        }
        return do_fetch(argv[2], (4 > argc) ? 20 : atoi(argv[3]));
    }
    if (0 == strcmp(argv[1], "fetchkey")) {
        if (5 > argc) {
            fprintf(stderr, "fetchkey needs <nspace> <rank> <key>\n");
            return 2;
        }
        return do_fetchkey(argv[2], (unsigned int) strtoul(argv[3], NULL, 10),
                           argv[4], (6 > argc) ? 60 : atoi(argv[5]));
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
