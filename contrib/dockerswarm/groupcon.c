/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * groupcon -- a minimal PMIx client that drives PMIx_Group_construct /
 * PMIx_Group_destruct across a real, multi-node DVM.
 *
 *   groupcon [--ft] [--fence] [--delay <s>] <groupID> [seconds]
 *       Every rank contributes a PMIX_GROUP_LOCAL_CID of its own
 *       (1234 + rank) as PMIX_GROUP_INFO, asks for a context id, and
 *       constructs the group.  As soon as construct returns -- with NO
 *       fence in between -- each rank reads back EVERY rank's local cid
 *       and checks the value.  Then it destructs the group.
 *
 *       --ft         ask for PMIX_GROUP_FT_COLLECTIVE, so the construct
 *                    completes on the survivors if a member is lost
 *       --order      supply PMIX_GROUP_FINAL_MEMBERSHIP_ORDER, listing the
 *                    ranks in reverse, so the group comes back in an order
 *                    the DVM had to impose rather than the one it would
 *                    have sorted into by itself
 *       --fence      run a PMIx_Fence over the whole job instead of a
 *                    group construct.  Here --delay applies to the LAST
 *                    rank only, so everyone else blocks inside the fence
 *                    for that long and a daemon can be killed while the
 *                    allgather is genuinely in flight
 *       --delay <s>  sleep this long before calling construct (or fence).  Staggering
 *                    the ranks is what makes the collective still be in
 *                    flight when a daemon is killed; without it the
 *                    construct is over long before the kill lands.
 *
 * Output lines, one per rank, all prefixed GRP so the harness can grep:
 *
 *   GRP <rank> HOST <hostname>
 *   GRP <rank> CONSTRUCT <status> CID <cid> ASSIGNED <T|F>
 *   GRP <rank> MEMBERS <n>
 *   GRP <rank> ORDER <r>,<r>,...
 *   GRP <rank> MEMBER-FAILED <nspace>:<rank>
 *   GRP <rank> CID-OK <n>            (read back n peers' local cids)
 *   GRP <rank> CID-FAIL <peer> <status>
 *   GRP <rank> DESTRUCT <status>
 *   GRP <rank> DONE <rc>
 *
 * Why this exists, and why it cannot be a unit test:
 *
 * A group construct is a two-phase collective -- an up-tree rollup to the
 * HNP, then an xcast of the result back down -- and the interesting half
 * runs in grp_release() on EVERY daemon, not just the master.  Each daemon
 * takes the broadcast result, hands the group's context id, group info and
 * endpoint data to its OWN local PMIx server via
 * PMIx_server_register_resources(), and only then releases the local
 * participants of the collective.  On one host there is exactly one daemon
 * and that is also the HNP, so the down-tree path, the per-daemon
 * registration, and the tracker bookkeeping that goes with it are never
 * exercised against a daemon that merely received the release.
 *
 * That registration used to be waited on, which stalled the daemon's whole
 * event loop for its duration; it is now a continuation off the
 * registration's completion.  What this client is really probing is that
 * continuation: lose the caddy anywhere between issuing the registration and
 * resuming, and the local participants are never released at all -- the
 * construct does not fail, it hangs, on every daemon at once.
 *
 * Be careful about what the read-back does and does not show.  It reads
 * every rank local cid with NO fence after the construct returns, which
 * looks like proof that the daemon registered the group with its PMIx server
 * before releasing us.  It is not: the construct hands the same group info
 * back to the client in its results, so the client library answers these
 * reads out of its own cache -- skipping the registration entirely still
 * passes them (measured, not assumed).  Their value is that they check the
 * returned membership and group info are complete and identical on every
 * daemon, which is a per-daemon property.
 *
 * Note the timeout on each read.  A regression here should fail this case,
 * not wedge the suite behind it.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

#define GROUPCON_BASE_CID 1234UL

static pmix_proc_t myproc;

#ifdef PMIX_GROUP_MEMBER_FAILED
/* A member of the group was lost with its daemon. The DVM raises this at the
 * survivors of a fault-tolerant construct, so it is the visible half of the
 * degraded-mode behavior - the construct returning success only says the group
 * formed, not that anything went missing. */
static void member_failed_handler(size_t evhdlr_registration_id, pmix_status_t status,
                                  const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                  pmix_info_t results[], size_t nresults,
                                  pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    (void) evhdlr_registration_id;
    (void) status;
    (void) source;
    (void) results;
    (void) nresults;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            printf("GRP %u MEMBER-FAILED %s:%u\n", myproc.rank,
                   info[n].value.data.proc->nspace,
                   (unsigned) info[n].value.data.proc->rank);
            fflush(stdout);
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void reg_handler(pmix_status_t status, size_t refid, void *cbdata)
{
    (void) status;
    (void) refid;
    (void) cbdata;
}
#endif

int main(int argc, char **argv)
{
    pmix_status_t rc, ret;
    pmix_value_t *val = NULL;
    pmix_value_t value;
    pmix_proc_t proc, *procs = NULL, *members = NULL;
    size_t nmembers = 0;
    pmix_info_t *results = NULL, *info = NULL;
    pmix_info_t tinfo[2];
    pmix_data_array_t darray;
    void *grpinfo, *list;
    size_t nresults = 0, ninfo = 0, cid = 0, lcid, m;
    uint32_t nprocs = 0, n, ok = 0;
    uint32_t get_timeout = 30;
    char hostname[256];
    const char *grpid;
    int seconds = 0;
    int delay = 0;
    int failures = 0;
    int i, npos = 0;
    bool idassigned = false;
    bool ft = false;
    bool dofence = false;
    bool doorder = false;
#ifdef PMIX_GROUP_MEMBER_FAILED
    pmix_status_t evcode = PMIX_GROUP_MEMBER_FAILED;
#endif

    /* flags may appear anywhere; the positional arguments keep their old
     * meaning so existing cases in the harness are unaffected */
    grpid = NULL;
    for (i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--ft")) {
            ft = true;
        } else if (0 == strcmp(argv[i], "--order")) {
            doorder = true;
        } else if (0 == strcmp(argv[i], "--fence")) {
            dofence = true;
        } else if (0 == strcmp(argv[i], "--delay") && (i + 1) < argc) {
            delay = atoi(argv[++i]);
        } else if (0 == npos) {
            grpid = argv[i];
            ++npos;
        } else if (1 == npos) {
            seconds = atoi(argv[i]);
            ++npos;
        }
    }
    if (NULL == grpid) {
        fprintf(stderr,
                "usage: %s [--ft] [--fence] [--order] [--delay <s>] <groupID> [seconds]\n",
                argv[0]);
        return 2;
    }
#ifndef PMIX_GROUP_FT_COLLECTIVE
    if (ft) {
        fprintf(stderr, "ERROR --ft: this PMIx has no PMIX_GROUP_FT_COLLECTIVE\n");
        return 2;
    }
#endif

    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    printf("GRP %u HOST %s\n", myproc.rank, hostname);
    fflush(stdout);

#ifdef PMIX_GROUP_MEMBER_FAILED
    /* register before constructing - the notification can arrive with the
     * construct's own completion */
    PMIx_Register_event_handler(&evcode, 1, NULL, 0, member_failed_handler,
                                reg_handler, NULL);
#endif

    /* how many of us are there? */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Get job size: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* put a little modex data so the construct has endpoints to carry -
     * an empty group is a different (and less interesting) path */
    value.type = PMIX_STRING;
    value.data.string = strdup(hostname);
    rc = PMIx_Put(PMIX_GLOBAL, "groupcon-host", &value);
    free(value.data.string);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Put: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    rc = PMIx_Commit();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Commit: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }

    /* --fence: exercise the allgather rather than the group collective. The
     * point of the mode is what happens when a daemon dies while one is in
     * flight, which nothing else here can hold open long enough to test. */
    if (dofence) {
        /* Only the LAST rank waits. Everyone else enters the allgather at
         * once and blocks there, so the fence is genuinely in flight for the
         * length of the delay - which is the state a test needs in order to
         * kill a daemon during one. A uniform delay would not do: every rank
         * would enter after the kill, and the fence would simply run to
         * completion over whatever was left. */
        if (0 < delay && myproc.rank == (nprocs - 1)) {
            printf("GRP %u DELAYING %d\n", myproc.rank, delay);
            fflush(stdout);
            sleep(delay);
        }
        printf("GRP %u FENCING\n", myproc.rank);
        fflush(stdout);
        PMIX_INFO_CREATE(info, 1);
        ninfo = 1;
        /* collect the data each rank put, so this is a real allgather and
         * not just a barrier */
        PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, &dofence, PMIX_BOOL);
        rc = PMIx_Fence(procs, nprocs, info, ninfo);
        PMIX_INFO_FREE(info, ninfo);
        info = NULL;
        ninfo = 0;
        printf("GRP %u FENCE %s\n", myproc.rank, PMIx_Error_string(rc));
        fflush(stdout);
        if (PMIX_SUCCESS != rc) {
            failures++;
        }
        goto done;
    }

    /* ask for a context id, and contribute our own local cid as group info */
    grpinfo = PMIx_Info_list_start();
    rc = PMIx_Info_list_add(grpinfo, PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_add ctxid: %s\n", PMIx_Error_string(rc));
        goto done;
    }
#ifdef PMIX_GROUP_FT_COLLECTIVE
    if (ft) {
        rc = PMIx_Info_list_add(grpinfo, PMIX_GROUP_FT_COLLECTIVE, NULL, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "ERROR list_add ftcoll: %s\n", PMIx_Error_string(rc));
            goto done;
        }
    }
#endif
    list = PMIx_Info_list_start();
    lcid = GROUPCON_BASE_CID + (size_t) myproc.rank;
    rc = PMIx_Info_list_add(list, PMIX_GROUP_LOCAL_CID, &lcid, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_add lcid: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    rc = PMIx_Info_list_convert(list, &darray);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_convert lcid: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    rc = PMIx_Info_list_add(grpinfo, PMIX_GROUP_INFO, &darray, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_add grpinfo: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    PMIx_Info_list_release(list);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    /* Ask for a specific final membership order - the ranks in reverse.
     *
     * This is the one directive that hands the DVM an array of procs to keep
     * for the length of the operation, and the array belongs to the PMIx
     * server that delivers it: the DVM must copy it, not point at it, or it
     * frees what PMIx frees again.  Reversing is what makes the result
     * checkable: the DVM sorts the membership itself when no order is given,
     * so a directive that was quietly dropped looks exactly like the default.
     */
    if (doorder) {
        PMIX_DATA_ARRAY_CONSTRUCT(&darray, nprocs, PMIX_PROC);
        for (n = 0; n < nprocs; n++) {
            PMIX_PROC_LOAD(&((pmix_proc_t *) darray.array)[n], myproc.nspace,
                           nprocs - 1 - n);
        }
        rc = PMIx_Info_list_add(grpinfo, PMIX_GROUP_FINAL_MEMBERSHIP_ORDER,
                                &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "ERROR list_add order: %s\n", PMIx_Error_string(rc));
            goto done;
        }
    }

    rc = PMIx_Info_list_convert(grpinfo, &darray);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_convert grpinfo: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    info = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIx_Info_list_release(grpinfo);

    /* hold here if asked, so the collective is still in flight when whatever
     * the test is doing to the DVM happens */
    if (0 < delay) {
        printf("GRP %u DELAYING %d\n", myproc.rank, delay);
        fflush(stdout);
        sleep(delay);
    }

    rc = PMIx_Group_construct(grpid, procs, nprocs, info, ninfo, &results, &nresults);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    if (PMIX_SUCCESS != rc) {
        printf("GRP %u CONSTRUCT %s CID 0 ASSIGNED F\n", myproc.rank, PMIx_Error_string(rc));
        fflush(stdout);
        failures++;
        goto done;
    }
    for (m = 0; m < nresults; m++) {
        if (PMIX_CHECK_KEY(&results[m], PMIX_GROUP_CONTEXT_ID)) {
            PMIx_Value_get_number(&results[m].value, &cid, PMIX_SIZE);
            idassigned = true;
        } else if (PMIX_CHECK_KEY(&results[m], PMIX_GROUP_MEMBERSHIP)) {
            if (PMIX_DATA_ARRAY == results[m].value.type && NULL != results[m].value.data.darray) {
                printf("GRP %u MEMBERS %u\n", myproc.rank,
                       (unsigned) results[m].value.data.darray->size);
                /* keep the membership: it is what we read back below. After a
                 * fault-tolerant construct it is SMALLER than the job, and
                 * asking a departed rank for its contribution is a guaranteed
                 * not-found rather than a real failure. */
                nmembers = results[m].value.data.darray->size;
                PMIX_PROC_CREATE(members, nmembers);
                memcpy(members, results[m].value.data.darray->array,
                       nmembers * sizeof(pmix_proc_t));
            }
        }
    }
    printf("GRP %u CONSTRUCT %s CID %lu ASSIGNED %s\n", myproc.rank,
           PMIx_Error_string(PMIX_SUCCESS), (unsigned long) cid, idassigned ? "T" : "F");
    fflush(stdout);
    if (doorder && 0 < nmembers) {
        /* report the membership in the order we were handed it, so the
         * caller can see whether the order it asked for was applied */
        printf("GRP %u ORDER", myproc.rank);
        for (m = 0; m < nmembers; m++) {
            printf("%s%u", (0 == m) ? " " : ",", (unsigned) members[m].rank);
        }
        printf("\n");
        fflush(stdout);
    }
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
    }

    /* Read every rank's local cid back RIGHT NOW.  No fence: if our daemon
     * released us before it finished registering the group's resources with
     * its own PMIx server, this is where that shows up. */
    PMIX_INFO_CONSTRUCT(&tinfo[0]);
    PMIX_INFO_LOAD(&tinfo[0], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
    PMIX_INFO_SET_QUALIFIER(&tinfo[0]);
    PMIX_INFO_CONSTRUCT(&tinfo[1]);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TIMEOUT, &get_timeout, PMIX_UINT32);

    /* read back the members the group actually ended up with, not every rank
     * the job started with */
    for (n = 0; n < nmembers; n++) {
        if (PMIX_RANK_WILDCARD == members[n].rank) {
            continue;
        }
        rc = PMIx_Get(&members[n], PMIX_GROUP_LOCAL_CID, tinfo, 2, &val);
        if (PMIX_SUCCESS != rc) {
            printf("GRP %u CID-FAIL %u %s\n", myproc.rank,
                   (unsigned) members[n].rank, PMIx_Error_string(rc));
            fflush(stdout);
            failures++;
            continue;
        }
        if (PMIX_SIZE != val->type
            || (GROUPCON_BASE_CID + (size_t) members[n].rank) != val->data.size) {
            printf("GRP %u CID-FAIL %u BADVALUE\n", myproc.rank,
                   (unsigned) members[n].rank);
            fflush(stdout);
            failures++;
            PMIX_VALUE_RELEASE(val);
            continue;
        }
        ok++;
        PMIX_VALUE_RELEASE(val);
    }
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    printf("GRP %u CID-OK %u\n", myproc.rank, ok);
    fflush(stdout);

    rc = PMIx_Group_destruct(grpid, NULL, 0);
    printf("GRP %u DESTRUCT %s\n", myproc.rank, PMIx_Error_string(rc));
    fflush(stdout);
    if (PMIX_SUCCESS != rc) {
        failures++;
    }

done:
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
    }
    if (NULL != members) {
        PMIX_PROC_FREE(members, nmembers);
    }
    if (0 < seconds) {
        sleep(seconds);
    }
    ret = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr, "ERROR PMIx_Finalize: %s\n", PMIx_Error_string(ret));
        failures++;
    }
    printf("GRP %u DONE %d\n", myproc.rank, failures);
    fflush(stdout);
    return (0 == failures) ? 0 : 1;
}
