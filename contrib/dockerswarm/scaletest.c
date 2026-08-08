/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * scaletest -- a bare PMIx client that measures how long the DVM takes to
 * complete a full-data fence and a bare barrier, so the two can be plotted
 * against DVM size, process count, routing radix and payload size.
 *
 *   scaletest [--nkeys N] [--sizes A,B,C] [--iters N] [--warmup N]
 *             [--scope global|remote|local] [--tag STR] [--verify] [--neighbors]
 *
 * Each iteration is three timed phases:
 *
 *   PUT      nkeys uniquely-named values are PMIx_Put (their sizes cycle
 *            through --sizes) and PMIx_Commit is called.  Nothing leaves
 *            the node yet -- commit only hands the data to the local
 *            server -- so this is the cost of staging, not of moving.
 *   COLLECT  PMIx_Fence over the whole job with PMIX_COLLECT_DATA true.
 *            This is the allgather: every rank's committed data has to
 *            reach every daemon, up the routing tree to the HNP and back
 *            down.  Its cost is the one that should grow with the payload.
 *   BARRIER  PMIx_Fence over the whole job with PMIX_COLLECT_DATA FALSE.
 *            Same collective, no payload -- so the difference between the
 *            two is what the data itself costs, and BARRIER on its own is
 *            the latency floor the routing tree imposes.
 *
 * Every phase is preceded by an unmeasured barrier, so the ranks enter it
 * as nearly together as the DVM can arrange.  That matters: without it a
 * straggler's late arrival is charged to the collective, and what you
 * measure is the skew of the previous phase.
 *
 * WHY THE TIMESTAMPS ARE ABSOLUTE.  Each rank records CLOCK_REALTIME at
 * the start and end of every phase and prints all of them.  Every "node"
 * of this harness is a container on ONE host sharing ONE kernel clock, so
 * those timestamps are directly comparable across ranks and the driver can
 * compute the real answer to "how long until ALL procs cleared the fence":
 *
 *      max(end) - min(start)      over every rank
 *
 * A per-rank elapsed time cannot say that -- a rank that entered late has
 * a short elapsed time precisely because everyone else was already waiting
 * for it.  (On a real cluster the two clocks are not the same and only the
 * max of the per-rank elapsed times is meaningful; the driver reports that
 * column too.)
 *
 * NOTHING IS PRINTED UNTIL THE END.  Results are buffered and written after
 * a final barrier, because stdout from a rank travels back to the tool over
 * the IOF wire through the same daemons the next collective needs.  Printing
 * as you go measures the I/O forwarding, not the fence.
 *
 * Output, one line per rank per iteration, all timestamps in nanoseconds:
 *
 *   SCALE <tag> RANK <r> HOST <h> ITER <i> \
 *         PUT <start> <end> COLLECT <start> <end> BARRIER <start> <end>
 *
 * plus one line from rank 0 describing what was run:
 *
 *   SCALE <tag> CONFIG NPROCS <n> NKEYS <k> BYTES_PER_RANK <b> ITERS <i>
 *
 * MEMORY.  A collect fence leaves every daemon holding every rank's data,
 * and the iterations use distinct keys, so a daemon ends up with
 * nprocs * bytes_per_rank * iters bytes.  BYTES_PER_RANK is printed so the
 * driver can refuse a configuration that would not fit; scaletest.sh does.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pmix.h>

#define SCALE_MAX_SIZES 16

static pmix_proc_t myproc;
static pmix_proc_t wildcard;

/* CLOCK_REALTIME, not CLOCK_MONOTONIC: the point is to compare one rank's
 * timestamps against another's, and a monotonic clock has an arbitrary
 * per-process epoch.  Every container here shares the host kernel, so
 * realtime is the same clock for all of them. */
static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* An unmeasured, uncollected fence used only to line the ranks up before a
 * phase is timed. */
static pmix_status_t sync_ranks(void)
{
    pmix_info_t info;
    bool flag = false;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(&wildcard, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

static pmix_status_t timed_fence(bool collect, uint64_t *start, uint64_t *end)
{
    pmix_info_t info;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
    *start = now_ns();
    rc = PMIx_Fence(&wildcard, 1, &info, 1);
    *end = now_ns();
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--nkeys N] [--sizes A,B,C] [--iters N] [--warmup N]\n"
            "          [--scope global|remote|local] [--tag STR] [--verify]\n"
            "          [--neighbors]\n",
            argv0);
}

int main(int argc, char **argv)
{
    size_t nkeys = 8;
    size_t sizes[SCALE_MAX_SIZES] = {1024};
    size_t nsizes = 1;
    size_t iters = 3;
    size_t warmup = 1;
    size_t total, maxsize, i, k, n;
    pmix_scope_t scope = PMIX_GLOBAL;
    const char *tag = "run";
    bool verify = false;
    bool neighbors = false;
    char hostname[256];
    char key[PMIX_MAX_KEYLEN + 1];
    char *payload = NULL;
    uint64_t *marks = NULL;
    pmix_value_t val;
    pmix_value_t *vptr;
    pmix_status_t rc;
    uint32_t nprocs = 1;
    int a;

    for (a = 1; a < argc; a++) {
        if (0 == strcmp(argv[a], "--nkeys") && a + 1 < argc) {
            nkeys = strtoul(argv[++a], NULL, 10);
        } else if (0 == strcmp(argv[a], "--sizes") && a + 1 < argc) {
            char *dup = strdup(argv[++a]);
            char *save = NULL;
            char *tok;
            nsizes = 0;
            for (tok = strtok_r(dup, ",", &save); NULL != tok && nsizes < SCALE_MAX_SIZES;
                 tok = strtok_r(NULL, ",", &save)) {
                sizes[nsizes++] = strtoul(tok, NULL, 10);
            }
            free(dup);
            if (0 == nsizes) {
                usage(argv[0]);
                return 1;
            }
        } else if (0 == strcmp(argv[a], "--iters") && a + 1 < argc) {
            iters = strtoul(argv[++a], NULL, 10);
        } else if (0 == strcmp(argv[a], "--warmup") && a + 1 < argc) {
            warmup = strtoul(argv[++a], NULL, 10);
        } else if (0 == strcmp(argv[a], "--tag") && a + 1 < argc) {
            tag = argv[++a];
        } else if (0 == strcmp(argv[a], "--verify")) {
            verify = true;
        } else if (0 == strcmp(argv[a], "--neighbors")) {
            neighbors = true;
        } else if (0 == strcmp(argv[a], "--scope") && a + 1 < argc) {
            ++a;
            if (0 == strcmp(argv[a], "global")) {
                scope = PMIX_GLOBAL;
            } else if (0 == strcmp(argv[a], "remote")) {
                scope = PMIX_REMOTE;
            } else if (0 == strcmp(argv[a], "local")) {
                scope = PMIX_LOCAL;
            } else {
                usage(argv[0]);
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (0 == iters) {
        usage(argv[0]);
        return 1;
    }
    /* --verify fetches rank+1, which is exactly one of the two peers the
     * NEIGHBORS phase then times.  Run together, verify warms the cache the
     * next phase is trying to measure a miss against. */
    if (verify && neighbors) {
        fprintf(stderr, "ERROR --verify and --neighbors are mutually exclusive:"
                        " verify fetches rank+1, which is a peer NEIGHBORS times\n");
        return 1;
    }

    if (0 != gethostname(hostname, sizeof(hostname))) {
        strcpy(hostname, "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);

    rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &vptr);
    if (PMIX_SUCCESS == rc && NULL != vptr) {
        PMIX_VALUE_GET_NUMBER(rc, vptr, nprocs, uint32_t);
        PMIX_VALUE_RELEASE(vptr);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR job size: %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* One buffer, reused for every put: PMIx_Put copies the value into its
     * own store, so the caller keeps ownership. */
    maxsize = 0;
    total = 0;
    for (k = 0; k < nkeys; k++) {
        size_t sz = sizes[k % nsizes];
        total += sz;
        if (sz > maxsize) {
            maxsize = sz;
        }
    }
    if (0 < maxsize) {
        payload = malloc(maxsize);
        if (NULL == payload) {
            fprintf(stderr, "ERROR out of memory (%zu bytes)\n", maxsize);
            PMIx_Finalize(NULL, 0);
            return 1;
        }
        for (n = 0; n < maxsize; n++) {
            payload[n] = (char) ((myproc.rank + n) & 0xff);
        }
    }

    /* 8 marks per iteration: put start/end, collect start/end, barrier
     * start/end, neighbors start/end.  Buffered, not printed, until every
     * iteration is done -- printing travels the IOF wire through the daemons
     * the next collective is about to use. */
    marks = calloc((warmup + iters) * 8, sizeof(uint64_t));
    if (NULL == marks) {
        fprintf(stderr, "ERROR out of memory\n");
        free(payload);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    for (i = 0; i < warmup + iters; i++) {
        uint64_t *m = &marks[i * 8];

        if (PMIX_SUCCESS != (rc = sync_ranks())) {
            fprintf(stderr, "ERROR sync before iter %zu: %s\n", i, PMIx_Error_string(rc));
            goto done;
        }

        m[0] = now_ns();
        for (k = 0; k < nkeys; k++) {
            /* The key carries the iteration, so every iteration moves data
             * the DVM has never seen -- reusing one key would let the second
             * fence collect nothing. */
            snprintf(key, sizeof(key), "st-%u-%zu-%zu", myproc.rank, i, k);
            PMIX_VALUE_CONSTRUCT(&val);
            val.type = PMIX_BYTE_OBJECT;
            val.data.bo.bytes = payload;
            val.data.bo.size = sizes[k % nsizes];
            rc = PMIx_Put(scope, key, &val);
            /* The buffer is ours and is reused; do not let the destructor
             * free it out from under the next put. */
            val.data.bo.bytes = NULL;
            val.data.bo.size = 0;
            PMIX_VALUE_DESTRUCT(&val);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "ERROR PMIx_Put %s: %s\n", key, PMIx_Error_string(rc));
                goto done;
            }
        }
        if (0 < nkeys) {
            if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
                fprintf(stderr, "ERROR PMIx_Commit: %s\n", PMIx_Error_string(rc));
                goto done;
            }
        }
        m[1] = now_ns();

        if (PMIX_SUCCESS != (rc = sync_ranks())) {
            fprintf(stderr, "ERROR sync before collect %zu: %s\n", i, PMIx_Error_string(rc));
            goto done;
        }
        /* Collecting, EXCEPT under --neighbors.  That phase exists to price
         * fetching only the peers you need, and a collect fence here would
         * have already put every rank's data on every daemon -- so the gets
         * below would be local cache hits and would measure nothing.
         * Measured: with the collect left in, a 4-node neighbors run issued
         * ZERO direct-modex requests and reported ~6us.  In neighbors mode
         * this fence therefore carries no payload, and the COLLECT column of
         * such a run is a second barrier rather than a collect. */
        if (PMIX_SUCCESS != (rc = timed_fence(!neighbors, &m[2], &m[3]))) {
            fprintf(stderr, "ERROR collect fence %zu: %s\n", i, PMIx_Error_string(rc));
            goto done;
        }

        if (verify && 0 < nkeys && 1 < nprocs) {
            /* Read one key from each of two peers.  This is what says the
             * collect fence actually made the bytes reachable rather than
             * merely synchronizing -- but it is off by default, because the
             * gets themselves perturb the next iteration.
             *
             * Two peers, not one, because the interesting way for a fence to
             * be wrong is to deliver less than the whole modex rather than to
             * deliver nothing.  Under one proc per node the NEAR peer is a
             * rank the local daemon is most likely to hold anyway, so a run
             * that checked only it would pass with the on-demand direct-modex
             * path completely broken.  The FAR peer, half the job away, is
             * the one that can only come back through that path. */
            pmix_proc_t peer;
            uint32_t peers[2];
            const char *which[2] = {"NEAR", "FAR"};
            size_t np = 1;

            peers[0] = (myproc.rank + 1) % nprocs;
            if (3 < nprocs) {
                peers[1] = (myproc.rank + nprocs / 2) % nprocs;
                np = 2;
            }
            for (size_t p = 0; p < np; p++) {
                uint32_t pr = peers[p];

                PMIX_LOAD_PROCID(&peer, myproc.nspace, pr);
                snprintf(key, sizeof(key), "st-%u-%zu-%zu", pr, i, (size_t) 0);
                rc = PMIx_Get(&peer, key, NULL, 0, &vptr);
                if (PMIX_SUCCESS != rc || NULL == vptr) {
                    printf("SCALE %s RANK %u ITER %zu VERIFY-FAIL %s %s\n", tag,
                           myproc.rank, i, which[p], PMIx_Error_string(rc));
                } else if (PMIX_BYTE_OBJECT != vptr->type || vptr->data.bo.size != sizes[0]) {
                    printf("SCALE %s RANK %u ITER %zu VERIFY-BAD %s size %zu want %zu\n",
                           tag, myproc.rank, i, which[p],
                           (size_t) vptr->data.bo.size, sizes[0]);
                    PMIX_VALUE_RELEASE(vptr);
                } else {
                    printf("SCALE %s RANK %u ITER %zu VERIFY-OK %s %zu bytes from rank %u\n",
                           tag, myproc.rank, i, which[p],
                           (size_t) vptr->data.bo.size, pr);
                    PMIX_VALUE_RELEASE(vptr);
                }
            }
        }

        if (PMIX_SUCCESS != (rc = sync_ranks())) {
            fprintf(stderr, "ERROR sync before barrier %zu: %s\n", i, PMIx_Error_string(rc));
            goto done;
        }
        if (PMIX_SUCCESS != (rc = timed_fence(false, &m[4], &m[5]))) {
            fprintf(stderr, "ERROR barrier fence %zu: %s\n", i, PMIx_Error_string(rc));
            goto done;
        }

        /* NEIGHBORS -- what a job pays for only the peers it actually needs.
         *
         * A plain run pays a collect fence to put every rank's data on every
         * daemon.  This phase prices the other end of that trade: neither
         * fence above has collected anything, so the two gets below MUST go
         * out as direct modex - the local daemon does not hold the peer's
         * data, so it fetches it from the daemon that does.  Both halves
         * already exist here, so the comparison Slurm's PMIX_Ring is an
         * argument for - O(1) per peer you actually need against O(N) to
         * everybody - can be measured in PRRTE rather than inferred.  Nothing
         * here implements a ring; this is the number that would have to
         * justify one.
         *
         * Check that with `--prtemca pmix_server_verbose 2
         * --leave-session-attached | grep -c "DMODX REQ FOR"'.  It must be
         * non-zero, and it is the assertion this phase rests on: when the
         * collect was still being run alongside, the count was ZERO and the
         * phase reported ~6us of local cache hits.
         *
         * OFF BY DEFAULT, and it must stay that way: these gets perturb the
         * next iteration, and the COLLECT column of a --neighbors run is a
         * second barrier rather than a collect.  So read the NEIGHBORS column
         * from this run and the COLLECT column from a separate plain one.
         *
         * Left and right are fetched in that order and not overlapped, so
         * this is the pessimistic serial reading of the pattern. */
        if (neighbors && 1 < nprocs) {
            uint32_t lrank = (myproc.rank + nprocs - 1) % nprocs;
            uint32_t rrank = (myproc.rank + 1) % nprocs;
            pmix_proc_t nbr;
            pmix_value_t *nval;
            size_t w;

            if (PMIX_SUCCESS != (rc = sync_ranks())) {
                fprintf(stderr, "ERROR sync before neighbors %zu: %s\n", i,
                        PMIx_Error_string(rc));
                goto done;
            }
            m[6] = now_ns();
            for (w = 0; w < 2; w++) {
                uint32_t peer_rank = (0 == w) ? lrank : rrank;

                PMIX_LOAD_PROCID(&nbr, myproc.nspace, peer_rank);
                snprintf(key, sizeof(key), "st-%u-%zu-%zu", peer_rank, i, (size_t) 0);
                rc = PMIx_Get(&nbr, key, NULL, 0, &nval);
                if (PMIX_SUCCESS != rc || NULL == nval) {
                    fprintf(stderr, "ERROR neighbor get %s from %u: %s\n", key,
                            peer_rank, PMIx_Error_string(rc));
                    goto done;
                }
                PMIX_VALUE_RELEASE(nval);
            }
            m[7] = now_ns();
        } else {
            m[6] = now_ns();
            m[7] = m[6];
        }
    }

    /* Everything is done; now it is safe to talk. */
    if (PMIX_SUCCESS != (rc = sync_ranks())) {
        fprintf(stderr, "ERROR final sync: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        printf("SCALE %s CONFIG NPROCS %u NKEYS %zu BYTES_PER_RANK %zu ITERS %zu WARMUP %zu\n",
               tag, nprocs, nkeys, total, iters, warmup);
    }
    for (i = warmup; i < warmup + iters; i++) {
        uint64_t *m = &marks[i * 8];

        printf("SCALE %s RANK %u HOST %s ITER %zu PUT %llu %llu COLLECT %llu %llu "
               "BARRIER %llu %llu NEIGHBORS %llu %llu\n",
               tag, myproc.rank, hostname, i - warmup, (unsigned long long) m[0],
               (unsigned long long) m[1], (unsigned long long) m[2], (unsigned long long) m[3],
               (unsigned long long) m[4], (unsigned long long) m[5],
               (unsigned long long) m[6], (unsigned long long) m[7]);
    }
    fflush(stdout);

done:
    free(marks);
    free(payload);
    if (PMIX_SUCCESS != rc) {
        /* Say so on stdout as well: the driver reads the capture file, and a
         * run that produced no SCALE lines has to be distinguishable from a
         * run whose lines went missing. */
        printf("SCALE %s RANK %u FAILED %s\n", tag, myproc.rank, PMIx_Error_string(rc));
        fflush(stdout);
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    PMIx_Finalize(NULL, 0);
    return 0;
}
