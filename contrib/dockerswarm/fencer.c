/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * fencer -- a minimal PMIx client that parks a job inside PMIx_Fence so a
 * daemon can be killed while the collective is in flight.
 *
 *   fencer [delay] [skip]
 *       Every rank puts a scrap of modex data, commits, and calls
 *       PMIx_Fence over the whole namespace.  Rank 0 waits <delay>
 *       seconds before entering, so every other rank is blocked in the
 *       fence -- and their daemons have rolled their contributions up to
 *       the HNP -- for that whole window.
 *
 *       With "skip", rank 0 never enters the fence at all: it waits out
 *       the delay and leaves.  That is what a test wants when it intends
 *       to break the collective rather than complete it -- the ranks that
 *       did enter must be released by the fault, and rank 0 must not then
 *       sit down in a fresh fence whose other participants have already
 *       gone home.
 *
 * Output lines, one per rank, all prefixed FENCE so the harness can grep:
 *
 *   FENCE <rank> HOST <hostname>
 *   FENCE <rank> ENTER
 *   FENCE <rank> RESULT <status-string>
 *   FENCE <rank> DONE <failures>
 *
 * Why this exists, and why it cannot be a unit test:
 *
 * A fence is an up-tree rollup to the HNP followed by an xcast release
 * back down, and what is being tested here is what happens to that
 * rollup when a daemon dies underneath it.  The fence fault handler used
 * to answer that by activating PRTE_JOB_STATE_COMM_FAILED with no job,
 * which the errmgr reads as the DAEMON job failing -- so a single lost
 * daemon during any fence took the whole DVM down with it
 * (https://github.com/openpmix/prrte/issues/2528).  It now completes the
 * in-flight fences with an error instead, and the DVM stands.
 *
 * Neither half of that can be seen on one host: there is one daemon,
 * it is the HNP, and killing it is not a fault -- it is the end of the
 * DVM either way.  The rank that delays is what gives the harness a
 * window in which the collective is provably still in flight rather
 * than a race against a fence that already completed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc, ret;
    pmix_value_t value;
    pmix_proc_t proc;
    char hostname[256];
    int delay = 0;
    int failures = 0;
    int skip = 0;

    if (2 <= argc) {
        delay = atoi(argv[1]);
    }
    if (3 <= argc && 0 == strcmp("skip", argv[2])) {
        skip = 1;
    }

    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    printf("FENCE %u HOST %s\n", myproc.rank, hostname);
    fflush(stdout);

    /* give the fence something to gather - a barrier and an allgather take
     * the same path here, but a payload makes the rollup buckets real */
    value.type = PMIX_STRING;
    value.data.string = strdup(hostname);
    rc = PMIx_Put(PMIX_GLOBAL, "fencer-host", &value);
    free(value.data.string);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Put: %s\n", PMIx_Error_string(rc));
        failures++;
        goto done;
    }
    rc = PMIx_Commit();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Commit: %s\n", PMIx_Error_string(rc));
        failures++;
        goto done;
    }

    /* rank 0 holds the collective open so the harness has a window in which
     * every other rank is provably inside it */
    if (0 == myproc.rank && 0 < delay) {
        sleep(delay);
    }
    if (0 == myproc.rank && skip) {
        printf("FENCE %u SKIP\n", myproc.rank);
        fflush(stdout);
        goto done;
    }

    printf("FENCE %u ENTER\n", myproc.rank);
    fflush(stdout);

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Fence(&proc, 1, NULL, 0);
    printf("FENCE %u RESULT %s\n", myproc.rank, PMIx_Error_string(rc));
    fflush(stdout);
    if (PMIX_SUCCESS != rc) {
        failures++;
    }

done:
    ret = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr, "ERROR PMIx_Finalize: %s\n", PMIx_Error_string(ret));
    }
    printf("FENCE %u DONE %d\n", myproc.rank, failures);
    fflush(stdout);
    return (0 == failures) ? 0 : 1;
}
