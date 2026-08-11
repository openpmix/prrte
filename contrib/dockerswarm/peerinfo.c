/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * peerinfo -- ask every other rank in my own job where it is, and say what
 * I was told.
 *
 * Each rank prints one SELF line describing itself and one PEER line per
 * other rank in the job, both in the same field order:
 *
 *   SELF <rank> <fields...>
 *   PEER <myrank> <peerrank> <fields...>
 *
 * so the harness can assert the whole thing with a string compare: for
 * every pair of ranks r and p, the tail of "PEER r p" must equal the tail
 * of "SELF p".  A rank that cannot answer at all prints
 * "PEERFAIL <myrank> <peerrank> <key> <status>", and the process exits
 * non-zero, so a silent partial answer cannot pass.
 *
 * Why this exists:
 *
 * These are the keys PRRTE decided when it mapped the job - rank, app rank,
 * local and node rank, node id, hostname, cpuset, locality string.  Every
 * daemon used to publish all of them, for every proc in the job, to its
 * local PMIx server, which is a table that grows with the whole job on a
 * node that will only ever run its own slice of it.  A daemon can instead
 * publish only the procs it hosts and answer for the rest out of its own
 * job object when somebody asks, which is what prte_pmix_lazy_procdata
 * turns on.
 *
 * Nothing else in this harness ever asks one rank about another rank's
 * reserved keys.  Every other client here either puts its own data and
 * fetches it back, or asks about the job rather than about a peer, and
 * both of those are answered without the request ever reaching the daemon.
 * So the whole derive-on-demand path could be switched on, run the entire
 * suite green, and never once have executed.  This client is the shape of
 * an MPI initialization - each rank asking where its peers are - and it is
 * the only thing here that drives a PMIx_Get for a reserved key of a proc
 * this daemon does not host.
 *
 * That is a statement about this harness and nothing more.  PRRTE is not
 * one MPI's runtime, several programming libraries build on it, and what
 * any of them asks a peer for is not knowable from this tree - so read the
 * gap as missing coverage, not as evidence that these keys go unwanted.
 *
 * Run it with --map-by node so the peers are genuinely elsewhere; on one
 * node every answer comes out of the local publication and the interesting
 * path is not entered.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;
static int nfail = 0;

/* Every field is printed even when absent, as "-", so the SELF and PEER
 * lines line up positionally and can be compared as strings.  An absent
 * value is a legitimate answer for several of these - an unbound proc has
 * no cpuset - and the point of the compare is that a rank sees the same
 * absence about a peer that the peer sees about itself. */
#define FIELD_MAX 512

static void note_fail(pmix_rank_t peer, const char *key, pmix_status_t rc)
{
    printf("PEERFAIL %u %u %s %s\n", myproc.rank, peer, key,
           PMIx_Error_string(rc));
    fflush(stdout);
    ++nfail;
}

/* Fetch one key and render it into buf.  A key that is simply not there
 * renders as "-" and is not a failure: whether, say, a cpuset exists is a
 * property of how the job was bound, and the comparison downstream is what
 * decides whether the two sides agree about it.  Any other status is a
 * failure, because it means the question could not be answered at all.
 *
 * "self" selects which of those two readings applies to NOT_FOUND when we
 * are asking about ourselves - it does not change the fetch. */
static void fetch(pmix_proc_t *target, const char *key, char *buf, size_t sz)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;

    snprintf(buf, sz, "-");
    rc = PMIx_Get(target, key, NULL, 0, &val);
    if (PMIX_ERR_NOT_FOUND == rc) {
        return;
    }
    if (PMIX_SUCCESS != rc) {
        note_fail(target->rank, key, rc);
        return;
    }
    if (NULL == val) {
        note_fail(target->rank, key, PMIX_ERR_BAD_PARAM);
        return;
    }
    switch (val->type) {
    case PMIX_PROC_RANK:
        snprintf(buf, sz, "%u", val->data.rank);
        break;
    case PMIX_UINT32:
        snprintf(buf, sz, "%u", val->data.uint32);
        break;
    case PMIX_UINT16:
        snprintf(buf, sz, "%u", (unsigned) val->data.uint16);
        break;
    case PMIX_STRING:
        snprintf(buf, sz, "%s",
                 (NULL == val->data.string) ? "-" : val->data.string);
        break;
    default:
        snprintf(buf, sz, "?type%d", (int) val->type);
        break;
    }
    PMIX_VALUE_RELEASE(val);
}

/* The keys PRRTE is the authority for.  This list is deliberately the same
 * set the daemon can derive: a key added to the derivation and not to this
 * list is a key nothing tests, and a key on this list that the daemon
 * cannot derive shows up here as a disagreement rather than as silence.
 *
 * PMIX_RANK is deliberately absent.  A process does not hold its own
 * PMIX_RANK in its datastore - it was handed its identity at init and there
 * is nothing there to look up - so a rank asking about itself gets
 * NOT_FOUND where a rank asking about a peer gets the value.  That is a
 * property of PMIx and has nothing to do with what is under test here, but
 * it would make every SELF/PEER pair disagree on the first field.  It is
 * checked separately, against the one thing that makes it worth checking:
 * the answer has to be the rank we asked about. */
static const char *keys[] = {
    PMIX_GLOBAL_RANK,
    PMIX_APP_RANK,
    PMIX_APPNUM,
    PMIX_LOCAL_RANK,
    PMIX_NODE_RANK,
    PMIX_NODEID,
    PMIX_HOSTNAME,
    PMIX_CPUSET,
    PMIX_LOCALITY_STRING,
    NULL
};

static void describe(pmix_proc_t *target, char *out, size_t sz)
{
    char field[FIELD_MAX];
    size_t used = 0;
    int k, n;

    out[0] = '\0';
    for (k = 0; NULL != keys[k]; k++) {
        fetch(target, keys[k], field, sizeof(field));
        /* snprintf reports what it WOULD have written, so accumulating its
         * return value walks `used` past the buffer on the first truncation
         * and every pointer built from it afterwards is out of bounds. Stop
         * at the truncation instead. The caller sizes `out` for the whole
         * line, so this is a backstop rather than an expected path - but a
         * line that silently ran short would read as a disagreement. */
        n = snprintf(out + used, sz - used, "%s%s", (0 == k) ? "" : " ", field);
        if (0 > n || (size_t) n >= sz - used) {
            break;
        }
        used += (size_t) n;
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t wildcard, target;
    pmix_value_t *val = NULL;
    uint32_t jobsize = 0, r;
    char line[FIELD_MAX * 12];

    (void) argc;
    (void) argv;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        fprintf(stderr, "ERROR job size: %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    jobsize = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* describe myself first.  This is answered out of what my own daemon
     * published about me, which it publishes either way, so it is the
     * fixed point the peers' answers are judged against */
    describe(&myproc, line, sizeof(line));
    printf("SELF %u %s\n", myproc.rank, line);
    fflush(stdout);

    for (r = 0; r < jobsize; r++) {
        if ((pmix_rank_t) r == myproc.rank) {
            continue;
        }
        PMIX_LOAD_PROCID(&target, myproc.nspace, (pmix_rank_t) r);
        describe(&target, line, sizeof(line));
        printf("PEER %u %u %s\n", myproc.rank, r, line);
        fflush(stdout);
        /* the one field with a right answer known here rather than by
         * comparison: whoever answered has to have answered about the proc
         * we named.  A derivation that walked to the wrong entry produces a
         * whole line of plausible values, and this is what catches it. */
        fetch(&target, PMIX_RANK, line, sizeof(line));
        if (0 != strcmp(line, "-") && (unsigned long) r != strtoul(line, NULL, 10)) {
            printf("PEERFAIL %u %u %s answered for rank %s\n", myproc.rank, r,
                   PMIX_RANK, line);
            fflush(stdout);
            ++nfail;
        }
    }

    printf("PEERINFO-DONE %u %d\n", myproc.rank, nfail);
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return (0 == nfail) ? 0 : 1;
}
