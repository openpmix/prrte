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
 * With --timeout N the fence carries a PMIX_TIMEOUT, so the controller ends
 * it whether or not every contribution arrived; with --twice a second fence
 * over the same participants follows, and it is the second one that is
 * verified.  Together with the grpcomm_fence_delay_ms fault-injection knob
 * those two reproduce a post-release straggler: one daemon's contribution is
 * still climbing the tree when the deadline ends the fence, and the next
 * fence over those participants must not inherit it.
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
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    pmix_proc_t me, wild;
    pmix_info_t info[2];
    size_t ninfo = 0;
    pmix_value_t val;
    pmix_status_t rc;
    bool collect = false, twice = false;
    int timeout = 0, a;
    char key[PMIX_MAX_KEYLEN + 1];

    if (2 > argc || (0 != strcmp(argv[1], "collect") && 0 != strcmp(argv[1], "barrier"))) {
        fprintf(stderr, "usage: fencer collect|barrier [--timeout N] [--twice]\n");
        return 2;
    }
    collect = (0 == strcmp(argv[1], "collect"));
    for (a = 2; a < argc; a++) {
        if (0 == strcmp(argv[a], "--timeout") && a + 1 < argc) {
            /* Put a deadline on the fence, so the controller ends it whether
             * or not every contribution has arrived.  That is what makes a
             * straggler reachable: pair it with grpcomm_fence_delay_ms on one
             * daemon and its contribution is still on the wire when the
             * release lands. */
            timeout = atoi(argv[++a]);
        } else if (0 == strcmp(argv[a], "--twice")) {
            /* Fence AGAIN over the same participants after the first one
             * ended.  This is the assertion that matters for the round
             * discriminator: a straggler from the first fence arrives with no
             * tracker to join, and if nothing recognizes which round it
             * belongs to it builds one that this second fence then finds -
             * inheriting a contribution count and a bucket it never gathered,
             * and converging early on the previous round's data. */
            twice = true;
        } else {
            fprintf(stderr, "usage: fencer collect|barrier [--timeout N] [--twice]\n");
            return 2;
        }
    }

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
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_COLLECT_DATA, &t, PMIX_BOOL);
    }
    if (0 < timeout) {
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_TIMEOUT, &timeout, PMIX_INT);
    }
    rc = PMIx_Fence(&wild, 1, ninfo ? info : NULL, ninfo);

    printf("FENCER %s rank %u rc %s\n", argv[1], me.rank, PMIx_Error_string(rc));

    /* The second fence.  Its result is reported separately and deliberately
     * is NOT folded into the first one's: the first is expected to fail when
     * a deadline ended it, and what is being asserted here is that the next
     * fence over the same participants is unaffected by that. */
    if (twice) {
        pmix_status_t rc2;
        uint32_t second = me.rank + 1000;

        /* Publish something NEW before the second fence, and this is the
         * whole point of the exercise rather than a detail.
         *
         * A straggler from the first round carries whatever that round's
         * contribution held.  If both fences carried the same bytes, a second
         * fence that wrongly counted the straggler in place of this round's
         * real contribution would still end up with a complete and correct
         * modex - the defect would happen and be invisible.  Giving the second
         * round data the first one never had is what makes the difference
         * observable: absorb the straggler and this key is missing. */
        snprintf(key, sizeof(key), "fencer.second.%u", me.rank);
        PMIX_VALUE_LOAD(&val, &second, PMIX_UINT32);
        rc2 = PMIx_Put(PMIX_GLOBAL, key, &val);
        if (PMIX_SUCCESS == rc2) {
            rc2 = PMIx_Commit();
        }
        if (PMIX_SUCCESS != rc2) {
            fprintf(stderr, "FENCER second put/commit failed %s\n",
                    PMIx_Error_string(rc2));
            PMIx_Finalize(NULL, 0);
            return 1;
        }

        ninfo = 0;
        if (collect) {
            bool t = true;
            PMIX_INFO_LOAD(&info[ninfo++], PMIX_COLLECT_DATA, &t, PMIX_BOOL);
        }
        rc2 = PMIx_Fence(&wild, 1, ninfo ? info : NULL, ninfo);
        printf("FENCER second rank %u rc %s\n", me.rank, PMIx_Error_string(rc2));
        /* verify against the SECOND fence, which is the one under test */
        rc = rc2;
    }

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
        pmix_status_t rc1st, rc2nd;

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
            /* PMIX_OPTIONAL here too, and for the reason spelled out at the
             * second key below: without it this asks whether the runtime can
             * find the value by any means - including fetching it from the
             * owning daemon on demand - rather than whether the collective
             * carried it.  A fence that delivered nothing at all would still
             * satisfy an ordinary Get. */
            {
                pmix_info_t opt[1];
                bool t = true;
                PMIX_INFO_LOAD(&opt[0], PMIX_OPTIONAL, &t, PMIX_BOOL);
                snprintf(key, sizeof(key), "fencer.rank.%u", r);
                got = NULL;
                rc1st = PMIx_Get(&peer, key, opt, 1, &got);
                PMIX_INFO_DESTRUCT(&opt[0]);
            }
            if (PMIX_SUCCESS == rc1st && NULL != got) {
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
            if (!twice) {
                continue;
            }
            /* The key that only the second round carried - see the note
             * where it is published.
             *
             * PMIX_OPTIONAL, and that is essential rather than tidy: without
             * it a PMIx_Get for a key the fence did not deliver falls through
             * to a direct modex and fetches it from the owning daemon anyway.
             * The value turns up either way and the check proves nothing.
             * OPTIONAL says "answer from what this client already has", which
             * is exactly the question - did the collective deliver it? */
            {
                pmix_info_t opt[1];
                bool t = true;
                PMIX_INFO_LOAD(&opt[0], PMIX_OPTIONAL, &t, PMIX_BOOL);
                snprintf(key, sizeof(key), "fencer.second.%u", r);
                got = NULL;
                rc2nd = PMIx_Get(&peer, key, opt, 1, &got);
                PMIX_INFO_DESTRUCT(&opt[0]);
            }
            if (PMIX_SUCCESS == rc2nd && NULL != got) {
                uint32_t v = 0;
                PMIx_Value_get_number(got, &v, PMIX_UINT32);
                if (v == r + 1000) {
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
