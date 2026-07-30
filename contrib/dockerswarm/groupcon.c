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
 *   groupcon <groupID> [seconds]
 *       Every rank contributes a PMIX_GROUP_LOCAL_CID of its own
 *       (1234 + rank) as PMIX_GROUP_INFO, asks for a context id, and
 *       constructs the group.  As soon as construct returns -- with NO
 *       fence in between -- each rank reads back EVERY rank's local cid
 *       and checks the value.  Then it destructs the group.
 *
 * Output lines, one per rank, all prefixed GRP so the harness can grep:
 *
 *   GRP <rank> HOST <hostname>
 *   GRP <rank> CONSTRUCT <status> CID <cid> ASSIGNED <T|F>
 *   GRP <rank> MEMBERS <n>
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

int main(int argc, char **argv)
{
    pmix_status_t rc, ret;
    pmix_value_t *val = NULL;
    pmix_value_t value;
    pmix_proc_t proc, *procs = NULL;
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
    int failures = 0;
    bool idassigned = false;

    if (2 > argc) {
        fprintf(stderr, "usage: %s <groupID> [seconds]\n", argv[0]);
        return 2;
    }
    grpid = argv[1];
    if (3 <= argc) {
        seconds = atoi(argv[2]);
    }

    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    printf("GRP %u HOST %s\n", myproc.rank, hostname);
    fflush(stdout);

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

    /* ask for a context id, and contribute our own local cid as group info */
    grpinfo = PMIx_Info_list_start();
    rc = PMIx_Info_list_add(grpinfo, PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_add ctxid: %s\n", PMIx_Error_string(rc));
        goto done;
    }
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

    rc = PMIx_Info_list_convert(grpinfo, &darray);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR list_convert grpinfo: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    info = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIx_Info_list_release(grpinfo);

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
            }
        }
    }
    printf("GRP %u CONSTRUCT %s CID %lu ASSIGNED %s\n", myproc.rank,
           PMIx_Error_string(PMIX_SUCCESS), (unsigned long) cid, idassigned ? "T" : "F");
    fflush(stdout);
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

    for (n = 0; n < nprocs; n++) {
        PMIX_LOAD_PROCID(&proc, myproc.nspace, n);
        rc = PMIx_Get(&proc, PMIX_GROUP_LOCAL_CID, tinfo, 2, &val);
        if (PMIX_SUCCESS != rc) {
            printf("GRP %u CID-FAIL %u %s\n", myproc.rank, n, PMIx_Error_string(rc));
            fflush(stdout);
            failures++;
            continue;
        }
        if (PMIX_SIZE != val->type || (GROUPCON_BASE_CID + (size_t) n) != val->data.size) {
            printf("GRP %u CID-FAIL %u BADVALUE\n", myproc.rank, n);
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
