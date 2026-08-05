/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A PMIx client that does exactly one fence, either as a modex or as a pure
 * barrier, so a test can drive the two apart.
 *
 * This exists because PMIX_COLLECT_DATA is the only thing that distinguishes
 * them, and it is what the "auto" fence-movement selection reads.  No other
 * harness client makes that distinction visible: a job running `hostname`
 * never fences at all, and groupcon's collectives are group operations.
 *
 *   fencer collect   -- fence carrying PMIX_COLLECT_DATA (a modex)
 *   fencer barrier   -- fence with no directives at all
 *
 * Prints one line per rank so the caller can count completions, and puts
 * something in the local store first so a collect fence actually has data to
 * gather rather than being a barrier wearing a hat.
 */

#include <pmix.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    pmix_proc_t me, wild;
    pmix_info_t info[1];
    pmix_value_t val;
    pmix_status_t rc;
    bool collect = false;
    char key[PMIX_MAX_KEYLEN + 1];

    if (2 > argc || (0 != strcmp(argv[1], "collect") && 0 != strcmp(argv[1], "barrier"))) {
        fprintf(stderr, "usage: fencer collect|barrier\n");
        return 2;
    }
    collect = (0 == strcmp(argv[1], "collect"));

    if (PMIX_SUCCESS != (rc = PMIx_Init(&me, NULL, 0))) {
        fprintf(stderr, "FENCER init failed %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* contribute something, so a collect fence has a reason to exist */
    snprintf(key, sizeof(key), "fencer.rank.%u", me.rank);
    PMIX_VALUE_LOAD(&val, &me.rank, PMIX_UINT32);
    rc = PMIx_Put(PMIX_GLOBAL, key, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "FENCER put failed %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "FENCER commit failed %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    PMIX_LOAD_PROCID(&wild, me.nspace, PMIX_RANK_WILDCARD);
    if (collect) {
        bool t = true;
        PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, &t, PMIX_BOOL);
        rc = PMIx_Fence(&wild, 1, info, 1);
    } else {
        rc = PMIx_Fence(&wild, 1, NULL, 0);
    }

    printf("FENCER %s rank %u rc %s\n", argv[1], me.rank, PMIx_Error_string(rc));

    /* After a collect fence every rank must be able to read every OTHER
     * rank's contribution, and that is the only assertion here that actually
     * looks at the gathered bytes.  "the fence returned success" does not: a
     * fence whose payload was assembled wrongly still returns success, and
     * the damage only appears when something tries to parse it. */
    if (PMIX_SUCCESS == rc && collect) {
        pmix_proc_t peer;
        pmix_value_t *got = NULL;
        uint32_t universe = 0, r;
        int found = 0, missing = 0;

        PMIX_LOAD_PROCID(&peer, me.nspace, PMIX_RANK_WILDCARD);
        if (PMIX_SUCCESS == PMIx_Get(&peer, PMIX_JOB_SIZE, NULL, 0, &got) &&
            NULL != got) {
            PMIx_Value_get_number(got, &universe, PMIX_UINT32);
            PMIX_VALUE_RELEASE(got);
        }
        for (r = 0; r < universe; r++) {
            if (r == me.rank) {
                continue;   /* only remote peers prove the collective ran */
            }
            PMIX_LOAD_PROCID(&peer, me.nspace, r);
            snprintf(key, sizeof(key), "fencer.rank.%u", r);
            got = NULL;
            if (PMIX_SUCCESS == PMIx_Get(&peer, key, NULL, 0, &got) && NULL != got) {
                uint32_t v = 0;
                PMIx_Value_get_number(got, &v, PMIX_UINT32);
                if (v == r) {
                    found++;
                } else {
                    missing++;
                }
                PMIX_VALUE_RELEASE(got);
            } else {
                missing++;
            }
        }
        printf("FENCER modex rank %u peers-ok %d peers-bad %d of %u\n",
               me.rank, found, missing, universe ? universe - 1 : 0);
        if (0 != missing) {
            rc = PMIX_ERROR;
        }
    }
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
