/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * pmixloop -- the reproducer from openpmix issue #4113, "Fence semantics
 * when clients rapidly cycle PMIx_Init/PMIx_Finalize".
 *
 * Each rank cycles Init -> collecting fence -> bare fence -> Finalize, with
 * a per-rank sleep before the first fence so the ranks are deliberately
 * skewed: rank 0 reaches cycle N+1 while its peers are still finalizing
 * cycle N.  That skew is the whole experiment.  The defect (openpmix
 * PR #4124) recorded a rank that had left through PMIx_Finalize on the
 * fence tracker's departed list even though the rank had not left the
 * accounting -- so a later cycle's fence completed one contribution short
 * with LOST_CONNECTION, the ranks drifted a cycle apart, and a collecting
 * fence merged with a non-collecting one to produce INVALID_ARG.
 *
 * The issue's own harness runs this under PMIx's simptest, where all four
 * ranks are local to one server and the fence never reaches the host.  Run
 * here under prun with the ranks spread over several nodes, the fence is
 * a real PRRTE collective across daemons instead -- which is the half a
 * single host cannot test.
 *
 * The body of the loop is the issue's program unchanged.  What is added is
 * a tally at the end, so a run can be judged by counting rather than by
 * grepping for the absence of something:
 *
 *   PMIXLOOP <rank> ALLDONE iters=<n> fence1_bad=<n> fence2_bad=<n> \
 *            init_bad=<n> fini_bad=<n>
 *
 * Every _bad count must be zero, and every rank must print the line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pmix.h>

int main(int argc, char *argv[])
{
    int iters = (argc > 1) ? atoi(argv[1]) : 100;
    pmix_proc_t myproc, wildcard;
    pmix_status_t rc;
    pmix_info_t info;
    uint32_t rank = UINT32_MAX;
    int fence1_bad = 0, fence2_bad = 0, init_bad = 0, fini_bad = 0;
    bool collect;
    int i;

    for (i = 0; i < iters; i++) {
        rc = PMIx_Init(&myproc, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            /* an Init that fails leaves us with no library to continue
             * against, so this one is still fatal -- but say which cycle */
            fprintf(stderr, "PMIXLOOP %u iter %d: PMIx_Init failed: %s\n",
                    rank, i, PMIx_Error_string(rc));
            ++init_bad;
            exit(1);
        }
        rank = myproc.rank;
        PMIX_PROC_CONSTRUCT(&wildcard);
        PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);

        usleep(1000 * (rank + 1)); /* per-rank jitter */

        collect = true;
        PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
        rc = PMIx_Fence(&wildcard, 1, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIXLOOP %u iter %d: fence1 rc=%d (%s)\n",
                    rank, i, rc, PMIx_Error_string(rc));
            ++fence1_bad;
        }

        rc = PMIx_Fence(&wildcard, 1, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIXLOOP %u iter %d: fence2 rc=%d (%s)\n",
                    rank, i, rc, PMIx_Error_string(rc));
            ++fence2_bad;
        }

        rc = PMIx_Finalize(NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIXLOOP %u iter %d: PMIx_Finalize rc=%d (%s)\n",
                    rank, i, rc, PMIx_Error_string(rc));
            ++fini_bad;
        }
    }

    fprintf(stderr,
            "PMIXLOOP %u ALLDONE iters=%d fence1_bad=%d fence2_bad=%d "
            "init_bad=%d fini_bad=%d\n",
            rank, iters, fence1_bad, fence2_bad, init_bad, fini_bad);
    return 0;
}
