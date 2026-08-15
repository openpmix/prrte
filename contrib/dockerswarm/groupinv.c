/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * groupinv -- a minimal PMIx client that forms a group by INVITATION across
 * a real, multi-node DVM, asking for a context id.
 *
 *   groupinv <groupID>
 *
 * The highest rank is the leader: it calls PMIx_Group_invite for the whole
 * job with PMIX_GROUP_ASSIGN_CONTEXT_ID.  Every other rank registers a
 * PMIX_GROUP_INVITED handler and accepts with PMIx_Group_join_nb.  Every rank
 * also posts one PMIx_Put value at PMIX_REMOTE scope beforehand and never
 * commits or fences it, then reads every peer's value back once the group has
 * formed.
 *
 * **The leader being the highest rank is the whole point of this client.**
 * Mapped by node, that rank is on the last node of the DVM and never on the
 * HNP.  A group formed by invitation runs no server collective, so its leader
 * asks for the context id through PMIx_Job_control -- which lands on the
 * daemon hosting the leader.  Only the DVM master holds the id pool, so that
 * daemon has to relay the request to the HNP and hand the answer back
 * (PRTE_PMIX_GROUP_CTXID in src/prted/pmix).  On one host the leader IS the
 * HNP and the whole relay is skipped, so this cannot be a single-host test.
 * Run it with the leader on node1 and it will pass without proving anything.
 *
 * Output lines, one per rank, all prefixed GINV so the harness can grep:
 *
 *   GINV <rank> HOST <hostname>
 *   GINV <rank> ROLE <LEADER|MEMBER>
 *   GINV <rank> INVITE <status>            (leader only)
 *   GINV <rank> COMPLETE CID <cid> ASSIGNED <T|F>
 *   GINV <rank> ENDPT-OK <n>               (read back n peers' values)
 *   GINV <rank> ENDPT-FAIL <peer> <status>
 *   GINV <rank> DONE <rc>
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

#define ENDPT_KEY "ginv-endpt"

static pmix_proc_t myproc;
static volatile bool complete_seen = false;
static volatile bool aborted = false;
static size_t mycid = SIZE_MAX;
static char *grpid = NULL;

typedef struct {
    volatile bool active;
    pmix_status_t status;
} milock_t;

static void regcb(pmix_status_t status, size_t evref, void *cbdata)
{
    milock_t *lk = (milock_t *) cbdata;
    (void) evref;

    lk->status = status;
    lk->active = false;
}

/* accept the invitation - which is also what contributes our endpoint data */
static void invite_handler(size_t evid, pmix_status_t status, const pmix_proc_t *source,
                           pmix_info_t info[], size_t ninfo, pmix_info_t results[],
                           size_t nresults, pmix_event_notification_cbfunc_fn_t cbfunc,
                           void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;
    (void) evid; (void) status; (void) results; (void) nresults;

    if (PMIX_CHECK_PROCID(source, &myproc)) {
        /* our own invitation reflected back */
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            grp = info[n].value.data.string;
            break;
        }
    }
    rc = PMIx_Group_join_nb(grp, source, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "GINV %u JOIN-FAIL %s\n", myproc.rank, PMIx_Error_string(rc));
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void complete_handler(size_t evid, pmix_status_t status, const pmix_proc_t *source,
                             pmix_info_t info[], size_t ninfo, pmix_info_t results[],
                             size_t nresults, pmix_event_notification_cbfunc_fn_t cbfunc,
                             void *cbdata)
{
    size_t n;
    (void) evid; (void) source; (void) results; (void) nresults;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_CONTEXT_ID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &mycid, PMIX_SIZE)) {
                mycid = SIZE_MAX;
            }
            break;
        }
    }
    if (PMIX_GROUP_CONSTRUCT_ABORT == status) {
        aborted = true;
    }
    complete_seen = true;

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static pmix_status_t reg(pmix_status_t code, pmix_notification_fn_t fn)
{
    milock_t lk = {true, PMIX_SUCCESS};

    PMIx_Register_event_handler(&code, 1, NULL, 0, fn, regcb, (void *) &lk);
    while (lk.active) {
        usleep(10000);
    }
    return (0 > lk.status) ? lk.status : PMIX_SUCCESS;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val, value;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n;
    pmix_info_t dir, quals[2], *results;
    size_t nresults, nok = 0;
    char hostname[256];
    int waited;
    bool leader;

    if (2 > argc) {
        fprintf(stderr, "usage: groupinv <groupID>\n");
        return 1;
    }
    grpid = argv[1];

    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "GINV ? INIT-FAIL %s\n", PMIx_Error_string(rc));
        return 1;
    }
    printf("GINV %u HOST %s\n", myproc.rank, hostname);
    fflush(stdout);

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "GINV %u JOBSIZE-FAIL %s\n", myproc.rank, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (2 > nprocs) {
        fprintf(stderr, "GINV %u needs at least 2 ranks\n", myproc.rank);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* The HIGHEST rank leads - see the header. Mapped by node it is never on
     * the HNP, so the leader's context-id request has to be relayed. */
    leader = (myproc.rank == (nprocs - 1));
    printf("GINV %u ROLE %s\n", myproc.rank, leader ? "LEADER" : "MEMBER");
    fflush(stdout);

    /* post our contribution - deliberately NOT committed or fenced, so only
     * the group exchange can carry it to the other members */
    value.type = PMIX_UINT64;
    value.data.uint64 = 1234UL + (unsigned long) myproc.rank;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, ENDPT_KEY, &value))) {
        fprintf(stderr, "GINV %u PUT-FAIL %s\n", myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (PMIX_SUCCESS != (rc = reg(PMIX_GROUP_INVITED, invite_handler)) ||
        PMIX_SUCCESS != (rc = reg(PMIX_GROUP_CONSTRUCT_COMPLETE, complete_handler)) ||
        PMIX_SUCCESS != (rc = reg(PMIX_GROUP_CONSTRUCT_ABORT, complete_handler))) {
        fprintf(stderr, "GINV %u REG-FAIL %s\n", myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* everyone has posted and registered before anyone invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "GINV %u FENCE-FAIL %s\n", myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (leader) {
        PMIX_PROC_CREATE(procs, nprocs);
        for (n = 0; n < nprocs; n++) {
            PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
        }
        PMIX_INFO_LOAD(&dir, PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);
        results = NULL;
        nresults = 0;
        rc = PMIx_Group_invite(grpid, procs, nprocs, &dir, 1, &results, &nresults);
        PMIX_PROC_FREE(procs, nprocs);
        PMIX_INFO_DESTRUCT(&dir);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        printf("GINV %u INVITE %s\n", myproc.rank, PMIx_Error_string(rc));
        fflush(stdout);
        if (PMIX_SUCCESS != rc) {
            goto done;
        }
    }

    for (waited = 0; !complete_seen && waited < 300; waited++) {
        usleep(100000); /* 0.1s; up to 30s */
    }
    if (!complete_seen) {
        fprintf(stderr, "GINV %u NO-COMPLETE\n", myproc.rank);
        rc = PMIX_ERR_TIMEOUT;
        goto done;
    }
    if (aborted) {
        fprintf(stderr, "GINV %u ABORTED\n", myproc.rank);
        rc = PMIX_GROUP_CONSTRUCT_ABORT;
        goto done;
    }
    printf("GINV %u COMPLETE CID %lu ASSIGNED %s\n", myproc.rank,
           (unsigned long) mycid, (SIZE_MAX == mycid) ? "F" : "T");
    fflush(stdout);
    if (SIZE_MAX == mycid) {
        rc = PMIX_ERR_NOT_FOUND;
        goto done;
    }

    /* read back every peer's contribution. OPTIONAL keeps the request from
     * leaving this process, and the context id is a QUALIFIER because that is
     * how a contribution to a group holding one is stored. */
    PMIX_INFO_LOAD(&quals[0], PMIX_GROUP_CONTEXT_ID, &mycid, PMIX_SIZE);
    PMIX_INFO_SET_QUALIFIER(&quals[0]);
    PMIX_INFO_LOAD(&quals[1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
    for (n = 0; n < nprocs; n++) {
        if (n == myproc.rank) {
            continue;
        }
        PMIX_LOAD_PROCID(&proc, myproc.nspace, n);
        rc = PMIx_Get(&proc, ENDPT_KEY, quals, 2, &val);
        if (PMIX_SUCCESS != rc) {
            printf("GINV %u ENDPT-FAIL %u %s\n", myproc.rank, n, PMIx_Error_string(rc));
            fflush(stdout);
            continue;
        }
        if (PMIX_UINT64 == val->type && (1234UL + (unsigned long) n) == val->data.uint64) {
            ++nok;
        } else {
            printf("GINV %u ENDPT-FAIL %u BADVALUE\n", myproc.rank, n);
            fflush(stdout);
        }
        PMIX_VALUE_RELEASE(val);
    }
    PMIX_INFO_DESTRUCT(&quals[0]);
    PMIX_INFO_DESTRUCT(&quals[1]);
    printf("GINV %u ENDPT-OK %lu\n", myproc.rank, (unsigned long) nok);
    fflush(stdout);
    rc = (nok == (size_t)(nprocs - 1)) ? PMIX_SUCCESS : PMIX_ERR_NOT_FOUND;

    if (PMIX_SUCCESS == rc) {
        /* every member destructs - it is a collective over the membership */
        rc = PMIx_Group_destruct(grpid, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "GINV %u DESTRUCT-FAIL %s\n", myproc.rank,
                    PMIx_Error_string(rc));
        }
    }

done:
    printf("GINV %u DONE %s\n", myproc.rank, PMIx_Error_string(rc));
    fflush(stdout);
    PMIx_Finalize(NULL, 0);
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
