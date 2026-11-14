/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * roletool -- a PMIx tool that asks a DVM for something only some users may
 * have, so the harness can run it as a user other than the DVM's and see
 * what it is given.
 *
 *   roletool <uri> scheduler
 *   roletool <uri> spawn
 *   roletool <uri> parent <nspace> <rank>
 *   roletool <uri> instantiate <session-id> <hosts>
 *   roletool <uri> pset <name> <nspace> <rank>
 *   roletool <uri> claim <nspace> <rank> [seconds]
 *   roletool <uri> purge <nspace> <rank>
 *   roletool <uri> notify <nspace>
 *   roletool <uri> allocids
 *   roletool <uri> instantiate-for <session-id> <hosts> <nspace> <rank>
 *
 * It connects to the DVM by its URI rather than by rendezvous file, since a
 * DVM's rendezvous files are readable by its own user only. A <uri> of "-"
 * connects to the daemon on this node by its rendezvous file instead, which
 * is how a tool reaches a non-master daemon.
 *
 *   scheduler    connect declaring itself the DVM's scheduler, then leave
 *   spawn        start /bin/true, naming no parent
 *   parent       start /bin/true, naming <nspace>:<rank> as its parent
 *   instantiate  create session <session-id> holding <hosts>
 *   pset         define process set <name> holding <nspace>:<rank>
 *   claim        connect naming itself <nspace>:<rank>, then leave - after
 *                [seconds], when given, so its namespace is held meanwhile
 *   purge        unpublish everything, naming <nspace>:<rank> as the
 *                process whose data it is (PMIX_REQUESTOR)
 *   notify       raise PMIX_ERR_PROC_ABORTED across the whole DVM, naming
 *                rank 0 of <nspace> as the process affected
 *   allocids     ask which allocations the DVM holds
 *   instantiate-for  create session <session-id> holding <hosts>, naming
 *                <nspace>:<rank> as the requester (PMIX_REQUESTOR)
 *
 * Output lines, all prefixed ROLE so the harness can grep:
 *
 *   ROLE INIT <status>
 *   ROLE NAME <nspace> <rank>    (claim: the name it connected under)
 *   ROLE SPAWN <status>
 *   ROLE SESSION <status>
 *   ROLE PSET <status>
 *   ROLE PURGE <status>
 *   ROLE NOTIFY <status>
 *   ROLE ALLOCID <id>            (one per allocation listed)
 *   ROLE ALLOCIDS <status>
 *
 * Why this cannot be a unit test: what is being asked is whether the DVM
 * judges the requester by the user PMIx authenticated for its connection,
 * which needs a second user, a live connection, and the DVM's own server.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix_tool.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool active;
    pmix_status_t status;
} lock_t;

static void lock_init(lock_t *l)
{
    pthread_mutex_init(&l->mutex, NULL);
    pthread_cond_init(&l->cond, NULL);
    l->active = true;
    l->status = PMIX_SUCCESS;
}

static void lock_wake(lock_t *l, pmix_status_t status)
{
    pthread_mutex_lock(&l->mutex);
    l->status = status;
    l->active = false;
    pthread_cond_broadcast(&l->cond);
    pthread_mutex_unlock(&l->mutex);
}

static void lock_wait(lock_t *l)
{
    pthread_mutex_lock(&l->mutex);
    while (l->active) {
        pthread_cond_wait(&l->cond, &l->mutex);
    }
    pthread_mutex_unlock(&l->mutex);
}

static void ctrl_cb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    (void) info;
    (void) ninfo;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    lock_wake((lock_t *) cbdata, status);
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s <uri> scheduler | spawn | parent <nspace> <rank> |"
            " instantiate <session-id> <hosts> | pset <name> <nspace> <rank> |"
            " claim <nspace> <rank> | purge <nspace> <rank> | notify <nspace> |"
            " allocids | instantiate-for <session-id> <hosts> <nspace> <rank>\n",
            name);
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc, parent, member;
    pmix_info_t iinfo[4], jinfo[1], dirs[3];
    pmix_app_t app;
    pmix_nspace_t child;
    pmix_status_t rc;
    size_t ninit = 0;
    const char *op;
    uint32_t sid;
    lock_t lock;
    bool flag = true;

    if (3 > argc) {
        usage(argv[0]);
        return 1;
    }
    op = argv[2];
    if ((0 == strcmp(op, "parent") && 5 != argc) ||
        (0 == strcmp(op, "instantiate") && 5 != argc) ||
        (0 == strcmp(op, "pset") && 6 != argc) ||
        (0 == strcmp(op, "claim") && 5 != argc && 6 != argc) ||
        (0 == strcmp(op, "purge") && 5 != argc) ||
        (0 == strcmp(op, "notify") && 4 != argc) ||
        (0 == strcmp(op, "instantiate-for") && 7 != argc)) {
        usage(argv[0]);
        return 1;
    }

    if (0 != strcmp(argv[1], "-")) {
        PMIX_INFO_LOAD(&iinfo[ninit++], PMIX_SERVER_URI, argv[1], PMIX_STRING);
    }
    if (0 == strcmp(op, "claim")) {
        /* a tool chooses its own name, including one that is taken */
        char *end;
        unsigned long ul;
        pmix_rank_t rank;

        errno = 0;
        ul = strtoul(argv[4], &end, 10);
        if (0 != errno || end == argv[4] || '\0' != *end || PMIX_RANK_VALID <= ul) {
            fprintf(stderr, "%s: claim: '%s' is not a rank\n", argv[0], argv[4]);
            return 1;
        }
        rank = (pmix_rank_t) ul;
        PMIX_INFO_LOAD(&iinfo[ninit++], PMIX_TOOL_NSPACE, argv[3], PMIX_STRING);
        PMIX_INFO_LOAD(&iinfo[ninit++], PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);
    } else if (0 == strcmp(op, "scheduler")) {
        /* a scheduler names itself */
        pmix_rank_t rank = 0;
        PMIX_INFO_LOAD(&iinfo[ninit++], PMIX_SERVER_SCHEDULER, &flag, PMIX_BOOL);
        PMIX_INFO_LOAD(&iinfo[ninit++], PMIX_TOOL_NSPACE, "roletool-scheduler", PMIX_STRING);
        PMIX_INFO_LOAD(&iinfo[ninit++], PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);
    }
    rc = PMIx_tool_init(&myproc, iinfo, ninit);
    printf("ROLE INIT %s\n", PMIx_Error_string(rc));
    fflush(stdout);
    if (PMIX_SUCCESS != rc) {
        return 1;
    }

    if (0 == strcmp(op, "scheduler")) {
        rc = PMIX_SUCCESS;

    } else if (0 == strcmp(op, "claim")) {
        /* PMIx may have connected it under a fresh name instead */
        printf("ROLE NAME %s %u\n", myproc.nspace, myproc.rank);
        fflush(stdout);
        if (6 == argc) {
            sleep((unsigned) strtoul(argv[5], NULL, 10));
        }
        rc = PMIX_SUCCESS;

    } else if (0 == strcmp(op, "spawn") || 0 == strcmp(op, "parent")) {
        PMIX_APP_CONSTRUCT(&app);
        app.cmd = strdup("/bin/true");
        app.argv = (char **) calloc(2, sizeof(char *));
        app.argv[0] = strdup("/bin/true");
        app.maxprocs = 1;
        if (0 == strcmp(op, "parent")) {
            PMIX_LOAD_PROCID(&parent, argv[3], (pmix_rank_t) strtoul(argv[4], NULL, 10));
            PMIX_INFO_LOAD(&jinfo[0], PMIX_PARENT_ID, &parent, PMIX_PROC);
            rc = PMIx_Spawn(jinfo, 1, &app, 1, child);
            PMIX_INFO_DESTRUCT(&jinfo[0]);
        } else {
            rc = PMIx_Spawn(NULL, 0, &app, 1, child);
        }
        printf("ROLE SPAWN %s\n", PMIx_Error_string(rc));
        PMIX_APP_DESTRUCT(&app);

    } else if (0 == strcmp(op, "instantiate") || 0 == strcmp(op, "instantiate-for")) {
        size_t ndirs = 2;

        sid = (uint32_t) strtoul(argv[3], NULL, 10);
        PMIX_INFO_LOAD(&dirs[0], PMIX_SESSION_INSTANTIATE, &flag, PMIX_BOOL);
        PMIX_INFO_LOAD(&dirs[1], PMIX_ALLOC_NODE_LIST, argv[4], PMIX_STRING);
        if (0 == strcmp(op, "instantiate-for")) {
            PMIX_LOAD_PROCID(&member, argv[5], (pmix_rank_t) strtoul(argv[6], NULL, 10));
            PMIX_INFO_LOAD(&dirs[ndirs], PMIX_REQUESTOR, &member, PMIX_PROC);
            ++ndirs;
        }
        lock_init(&lock);
        rc = PMIx_Session_control(sid, dirs, ndirs, ctrl_cb, &lock);
        if (PMIX_SUCCESS != rc) {
            lock_wake(&lock, rc);
        } else {
            lock_wait(&lock);
        }
        rc = lock.status;
        printf("ROLE SESSION %s\n", PMIx_Error_string(rc));
        while (0 < ndirs) {
            --ndirs;
            PMIX_INFO_DESTRUCT(&dirs[ndirs]);
        }

    } else if (0 == strcmp(op, "purge")) {
        PMIX_LOAD_PROCID(&member, argv[3], (pmix_rank_t) strtoul(argv[4], NULL, 10));
        PMIX_INFO_LOAD(&dirs[0], PMIX_REQUESTOR, &member, PMIX_PROC);
        rc = PMIx_Unpublish(NULL, dirs, 1);
        printf("ROLE PURGE %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&dirs[0]);

    } else if (0 == strcmp(op, "notify")) {
        PMIX_LOAD_PROCID(&member, argv[3], 0);
        PMIX_INFO_LOAD(&dirs[0], PMIX_EVENT_AFFECTED_PROC, &member, PMIX_PROC);
        rc = PMIx_Notify_event(PMIX_ERR_PROC_ABORTED, NULL, PMIX_RANGE_GLOBAL, dirs, 1,
                               NULL, NULL);
        printf("ROLE NOTIFY %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&dirs[0]);

    } else if (0 == strcmp(op, "allocids")) {
#ifdef PMIX_QUERY_ALLOC_IDS
        pmix_query_t query;
        pmix_info_t *results = NULL, *ids;
        size_t nresults = 0, n, k, nids;

        PMIX_QUERY_CONSTRUCT(&query);
        query.keys = (char **) calloc(2, sizeof(char *));
        query.keys[0] = strdup(PMIX_QUERY_ALLOC_IDS);
        rc = PMIx_Query_info(&query, 1, &results, &nresults);
        for (n = 0; PMIX_SUCCESS == rc && n < nresults; n++) {
            if (!PMIX_CHECK_KEY(&results[n], PMIX_QUERY_ALLOC_IDS) ||
                PMIX_DATA_ARRAY != results[n].value.type ||
                NULL == results[n].value.data.darray ||
                PMIX_INFO != results[n].value.data.darray->type) {
                continue;
            }
            ids = (pmix_info_t *) results[n].value.data.darray->array;
            nids = results[n].value.data.darray->size;
            for (k = 0; k < nids; k++) {
                if (PMIX_STRING == ids[k].value.type && NULL != ids[k].value.data.string) {
                    printf("ROLE ALLOCID %s\n", ids[k].value.data.string);
                }
            }
        }
        printf("ROLE ALLOCIDS %s\n", PMIx_Error_string(rc));
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_QUERY_DESTRUCT(&query);
#else
        rc = PMIX_ERR_NOT_SUPPORTED;
        printf("ROLE ALLOCIDS %s\n", PMIx_Error_string(rc));
#endif

    } else if (0 == strcmp(op, "pset")) {
        PMIX_LOAD_PROCID(&member, argv[4], (pmix_rank_t) strtoul(argv[5], NULL, 10));
        PMIX_INFO_LOAD(&dirs[0], PMIX_JOB_CTRL_DEFINE_PSET, argv[3], PMIX_STRING);
        lock_init(&lock);
        rc = PMIx_Job_control_nb(&member, 1, dirs, 1, ctrl_cb, &lock);
        if (PMIX_SUCCESS != rc) {
            lock_wake(&lock, rc);
        } else {
            lock_wait(&lock);
        }
        rc = lock.status;
        printf("ROLE PSET %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&dirs[0]);

    } else {
        usage(argv[0]);
        rc = PMIX_ERR_BAD_PARAM;
    }
    fflush(stdout);

    PMIx_tool_finalize();
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
