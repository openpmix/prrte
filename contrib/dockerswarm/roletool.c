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
 *   roletool <uri> instantiate-owned <session-id> <hosts> <uid> <gid>
 *   roletool <uri> sched-instantiate <session-id> <hosts> [<uid> <gid> [cmd...]]
 *   roletool <uri> terminate <session-id>
 *   roletool <uri> alloc [<uid> <gid> [seconds]]
 *   roletool <uri> alloc-release <alloc-id>
 *   roletool <uri> instantiate-id <session-id> <hosts> <alloc-id|->
 *   roletool <uri> alloc-naming <alloc-id>
 *   roletool <uri> allocprops <alloc-id>
 *   roletool <uri> allocnodes [<alloc-id>]
 *   roletool <uri> signal <signum> <nspace:rank|nspace:*>...
 *   roletool <uri> alloc-reqid <req-id> <count> <seconds>
 *   roletool <uri> alloc-extend-reqid <req-id> <time>
 *
 * It connects to the DVM by its URI rather than by rendezvous file, since a
 * DVM's rendezvous files are readable by its own user only. A <uri> of "-"
 * connects to the daemon on this node by its rendezvous file instead, which
 * is how a tool reaches a non-master daemon.
 *
 *   scheduler    connect declaring itself the DVM's scheduler, then leave
 *   spawn        start /bin/true, naming no parent
 *   parent       start /bin/true, naming <nspace>:<rank> as its parent
 *   instantiate  create session <session-id> holding <hosts>, with
 *                allocation id roletool.<session-id> (PMIX_ALLOC_ID), as
 *                every instantiate op gives it
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
 *   instantiate-owned  create session <session-id> holding <hosts>, naming
 *                <uid>:<gid> as its owner (PMIX_ALLOC_UID/GID)
 *   sched-instantiate  the same, connected declaring itself the scheduler -
 *                naming no owner when <uid> <gid> are left out, and running
 *                [cmd...] in the session when it is given
 *   terminate    terminate session <session-id>
 *   alloc        ask for a new allocation of no nodes, naming <uid> and
 *                <gid> as whom it is for (PMIX_ALLOC_UID/GID) - either may
 *                be "-" to leave it out - then leave after [seconds], when
 *                given: a reservation is released when its requester ends
 *   alloc-release  release allocation <alloc-id>
 *   instantiate-id  create session <session-id> holding <hosts>, with
 *                allocation id <alloc-id> - or none, given "-"
 *   alloc-naming  ask for a new allocation of no nodes, naming <alloc-id>
 *                as its PMIX_ALLOC_ID
 *   allocprops   ask for the properties of allocation <alloc-id>
 *   allocnodes   ask for the nodes of allocation <alloc-id>, or of the
 *                whole DVM (PMIX_QUERY_ALLOCATION)
 *   signal       send <signum> by PMIx_Job_control (PMIX_JOB_CTRL_SIGNAL)
 *                to every target given, each <nspace>:<rank>, or
 *                <nspace>:* for the whole job
 *   alloc-reqid  ask for <count> new allocations of no nodes, each under
 *                PMIX_ALLOC_REQ_ID <req-id>; then extend by <req-id> alone
 *                for 600 seconds, show the first one's properties, and
 *                leave after <seconds>
 *   alloc-extend-reqid  extend by PMIX_ALLOC_REQ_ID <req-id> alone, for
 *                <time>
 *
 * Output lines, all prefixed ROLE so the harness can grep:
 *
 *   ROLE INIT <status>
 *   ROLE NAME <nspace> <rank>    (claim: the name it connected under)
 *   ROLE SPAWN <status>
 *   ROLE SESSION <status>
 *   ROLE TERMINATE <status>
 *   ROLE PSET <status>
 *   ROLE PURGE <status>
 *   ROLE NOTIFY <status>
 *   ROLE ALLOCID <id>            (one per allocation listed)
 *   ROLE ALLOCIDS <status>
 *   ROLE ALLOC <status>
 *   ROLE ALLOCATED <id>          (alloc: the id the allocation was given)
 *   ROLE RELEASE <status>
 *   ROLE ALLOCPROP <key> <value> (one per property shown)
 *   ROLE ALLOCPROPS <status>
 *   ROLE ALLOCNODE <hostname>    (one per node told)
 *   ROLE ALLOCNODES <status>
 *   ROLE SIGNAL <status>
 *   ROLE REQID-EXTEND <status>
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

/* a spawn whose requester does not wait for the answer */
static void leave_cb(pmix_status_t status, pmix_nspace_t nspace, void *cbdata)
{
    (void) status;
    (void) nspace;
    (void) cbdata;
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
            " allocids | instantiate-for <session-id> <hosts> <nspace> <rank> |"
            " instantiate-owned <session-id> <hosts> <uid> <gid> |"
            " sched-instantiate <session-id> <hosts> [<uid> <gid> [cmd...]] |"
            " terminate <session-id> | extend <session-id> <hosts|-> [<time>] |"
            " alloc [<uid> <gid> [<seconds> [<time>]]] |"
            " alloc-release <alloc-id> | alloc-extend <alloc-id> <time> |"
            " instantiate-id <session-id> <hosts> <alloc-id|-> |"
            " alloc-naming <alloc-id> | allocprops <alloc-id> | allocnodes [<alloc-id>] |"
            " signal <signum> <nspace:rank|nspace:*>... |"
            " alloc-reqid <req-id> <count> <seconds> |"
            " alloc-extend-reqid <req-id> <time> | spawn-leave <usec> <cmd> [args...]\n",
            name);
}

/* Print the properties PMIX_QUERY_ALLOC_PROPERTIES gives for allocation
 * id, one "ROLE ALLOCPROP <key> <value>" line each, then
 * "ROLE ALLOCPROPS <status>". */
static pmix_status_t print_alloc_props(const char *id)
{
#ifdef PMIX_QUERY_ALLOC_PROPERTIES
    pmix_query_t query;
    pmix_info_t *results = NULL, *props;
    size_t nresults = 0, n, k, nprops;
    pmix_status_t rc;

    PMIX_QUERY_CONSTRUCT(&query);
    query.keys = (char **) calloc(2, sizeof(char *));
    query.keys[0] = strdup(PMIX_QUERY_ALLOC_PROPERTIES);
    PMIX_INFO_CREATE(query.qualifiers, 1);
    query.nqual = 1;
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_ALLOC_ID, id, PMIX_STRING);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    for (n = 0; PMIX_SUCCESS == rc && n < nresults; n++) {
        if (!PMIX_CHECK_KEY(&results[n], PMIX_QUERY_ALLOC_PROPERTIES) ||
            PMIX_DATA_ARRAY != results[n].value.type ||
            NULL == results[n].value.data.darray ||
            PMIX_INFO != results[n].value.data.darray->type) {
            continue;
        }
        props = (pmix_info_t *) results[n].value.data.darray->array;
        nprops = results[n].value.data.darray->size;
        for (k = 0; k < nprops; k++) {
            switch (props[k].value.type) {
            case PMIX_STRING:
                printf("ROLE ALLOCPROP %s %s\n", props[k].key,
                       (NULL == props[k].value.data.string) ? "" : props[k].value.data.string);
                break;
            case PMIX_PROC:
                if (NULL != props[k].value.data.proc) {
                    printf("ROLE ALLOCPROP %s %s:%u\n", props[k].key,
                           props[k].value.data.proc->nspace,
                           (unsigned) props[k].value.data.proc->rank);
                }
                break;
            case PMIX_UINT32:
                printf("ROLE ALLOCPROP %s %u\n", props[k].key, props[k].value.data.uint32);
                break;
            case PMIX_BOOL:
                printf("ROLE ALLOCPROP %s %s\n", props[k].key,
                       props[k].value.data.flag ? "true" : "false");
                break;
            case PMIX_STATUS:
                printf("ROLE ALLOCPROP %s %s\n", props[k].key,
                       PMIx_Error_string(props[k].value.data.status));
                break;
            default:
                printf("ROLE ALLOCPROP %s (%s)\n", props[k].key,
                       PMIx_Data_type_string(props[k].value.type));
                break;
            }
        }
    }
    printf("ROLE ALLOCPROPS %s\n", PMIx_Error_string(rc));
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_QUERY_DESTRUCT(&query);
    return rc;
#else
    (void) id;
    printf("ROLE ALLOCPROPS %s\n", PMIx_Error_string(PMIX_ERR_NOT_SUPPORTED));
    return PMIX_ERR_NOT_SUPPORTED;
#endif
}

/* An EXTEND of the allocation this process requested under req_id, for
 * time seconds: "ROLE REQID-EXTEND <status>" */
static pmix_status_t extend_by_req_id(const char *reqid, const char *time)
{
    pmix_info_t dirs[2];
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&dirs[0], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[1], PMIX_ALLOC_TIME, time, PMIX_STRING);
    rc = PMIx_Allocation_request(PMIX_ALLOC_EXTEND, dirs, 2, &results, &nresults);
    printf("ROLE REQID-EXTEND %s\n", PMIx_Error_string(rc));
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_INFO_DESTRUCT(&dirs[0]);
    PMIX_INFO_DESTRUCT(&dirs[1]);
    return rc;
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc, parent, member;
    pmix_info_t iinfo[4], jinfo[1], dirs[6];
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
        (0 == strcmp(op, "signal") && 5 > argc) ||
        (0 == strcmp(op, "notify") && 4 != argc) ||
        (0 == strcmp(op, "instantiate-for") && 7 != argc) ||
        (0 == strcmp(op, "instantiate-owned") && 7 != argc) ||
        (0 == strcmp(op, "sched-instantiate") && 5 != argc && 7 > argc) ||
        (0 == strcmp(op, "terminate") && 4 != argc) ||
        (0 == strcmp(op, "extend") && 5 != argc && 6 != argc) ||
        (0 == strcmp(op, "alloc") && 3 != argc && 5 != argc && 6 != argc && 7 != argc) ||
        (0 == strcmp(op, "alloc-release") && 4 != argc) ||
        (0 == strcmp(op, "alloc-extend") && 5 != argc) ||
        (0 == strcmp(op, "instantiate-id") && 6 != argc) ||
        (0 == strcmp(op, "alloc-naming") && 4 != argc) ||
        (0 == strcmp(op, "allocprops") && 4 != argc) ||
        (0 == strcmp(op, "allocnodes") && 3 != argc && 4 != argc) ||
        (0 == strcmp(op, "alloc-reqid") && 6 != argc) ||
        (0 == strcmp(op, "alloc-extend-reqid") && 5 != argc) ||
        (0 == strcmp(op, "spawn-leave") && 5 > argc)) {
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
    } else if (0 == strcmp(op, "scheduler") || 0 == strcmp(op, "sched-instantiate")) {
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

    } else if (0 == strcmp(op, "spawn-leave")) {
        /* ask for a job and go, without waiting for the answer or saying
         * goodbye: the request and the departure then race to the master */
        int i;

        PMIX_APP_CONSTRUCT(&app);
        app.cmd = strdup(argv[4]);
        app.argv = (char **) calloc((size_t) (argc - 3), sizeof(char *));
        for (i = 4; i < argc; i++) {
            app.argv[i - 4] = strdup(argv[i]);
        }
        app.maxprocs = 1;
        app.cwd = strdup("/tmp");
        rc = PMIx_Spawn_nb(NULL, 0, &app, 1, leave_cb, NULL);
        printf("ROLE SPAWN-LEAVE %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        usleep((useconds_t) strtoul(argv[3], NULL, 10));
        _exit(0);

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

    } else if (0 == strcmp(op, "instantiate") || 0 == strcmp(op, "instantiate-for") ||
               0 == strcmp(op, "instantiate-owned") || 0 == strcmp(op, "sched-instantiate")) {
        size_t ndirs = 3;
        uint32_t id;
        char allocid[64];

        sid = (uint32_t) strtoul(argv[3], NULL, 10);
        /* the id a scheduler would give the allocation - an instantiation
         * must carry one */
        snprintf(allocid, sizeof(allocid), "roletool.%u", sid);
        PMIX_INFO_LOAD(&dirs[0], PMIX_SESSION_INSTANTIATE, &flag, PMIX_BOOL);
        PMIX_INFO_LOAD(&dirs[1], PMIX_ALLOC_NODE_LIST, argv[4], PMIX_STRING);
        PMIX_INFO_LOAD(&dirs[2], PMIX_ALLOC_ID, allocid, PMIX_STRING);
        if (0 == strcmp(op, "instantiate-for")) {
            PMIX_LOAD_PROCID(&member, argv[5], (pmix_rank_t) strtoul(argv[6], NULL, 10));
            PMIX_INFO_LOAD(&dirs[ndirs], PMIX_REQUESTOR, &member, PMIX_PROC);
            ++ndirs;
        } else if (0 != strcmp(op, "instantiate") && 7 <= argc) {
            id = (uint32_t) strtoul(argv[5], NULL, 10);
            PMIX_INFO_LOAD(&dirs[ndirs], PMIX_ALLOC_UID, &id, PMIX_UINT32);
            ++ndirs;
            id = (uint32_t) strtoul(argv[6], NULL, 10);
            PMIX_INFO_LOAD(&dirs[ndirs], PMIX_ALLOC_GID, &id, PMIX_UINT32);
            ++ndirs;
        }
        if (0 == strcmp(op, "sched-instantiate") && 8 <= argc) {
            pmix_data_array_t apps;
            int i;

            PMIX_APP_CONSTRUCT(&app);
            app.cmd = strdup(argv[7]);
            app.argv = (char **) calloc((size_t) (argc - 6), sizeof(char *));
            for (i = 7; i < argc; i++) {
                app.argv[i - 7] = strdup(argv[i]);
            }
            app.maxprocs = 1;
            /* somewhere whoever the session is for may be */
            app.cwd = strdup("/tmp");
            apps.type = PMIX_APP;
            apps.size = 1;
            apps.array = &app;
            /* the info takes a copy of the array */
            PMIX_INFO_LOAD(&dirs[ndirs], PMIX_SESSION_APP, &apps, PMIX_DATA_ARRAY);
            ++ndirs;
            PMIX_APP_DESTRUCT(&app);
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

    } else if (0 == strcmp(op, "terminate")) {
        sid = (uint32_t) strtoul(argv[3], NULL, 10);
        PMIX_INFO_LOAD(&dirs[0], PMIX_SESSION_TERMINATE, &flag, PMIX_BOOL);
        lock_init(&lock);
        rc = PMIx_Session_control(sid, dirs, 1, ctrl_cb, &lock);
        if (PMIX_SUCCESS != rc) {
            lock_wake(&lock, rc);
        } else {
            lock_wait(&lock);
        }
        rc = lock.status;
        printf("ROLE TERMINATE %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&dirs[0]);

    } else if (0 == strcmp(op, "extend")) {
        size_t ndirs = 0;

        sid = (uint32_t) strtoul(argv[3], NULL, 10);
        PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_SESSION_EXTEND, &flag, PMIX_BOOL);
        if (0 != strcmp(argv[4], "-")) {
            PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_ALLOC_NODE_LIST, argv[4], PMIX_STRING);
        }
        if (6 == argc) {
            PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_ALLOC_TIME, argv[5], PMIX_STRING);
        }
        lock_init(&lock);
        rc = PMIx_Session_control(sid, dirs, ndirs, ctrl_cb, &lock);
        if (PMIX_SUCCESS != rc) {
            lock_wake(&lock, rc);
        } else {
            lock_wait(&lock);
        }
        rc = lock.status;
        printf("ROLE EXTEND %s\n", PMIx_Error_string(rc));
        while (0 < ndirs) {
            --ndirs;
            PMIX_INFO_DESTRUCT(&dirs[ndirs]);
        }

    } else if (0 == strcmp(op, "alloc")) {
        pmix_info_t *results = NULL;
        size_t nresults = 0, ndirs = 0, n;
        uint32_t id;

        if (5 <= argc && 0 != strcmp(argv[3], "-")) {
            id = (uint32_t) strtoul(argv[3], NULL, 10);
            PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_ALLOC_UID, &id, PMIX_UINT32);
        }
        if (5 <= argc && 0 != strcmp(argv[4], "-")) {
            id = (uint32_t) strtoul(argv[4], NULL, 10);
            PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_ALLOC_GID, &id, PMIX_UINT32);
        }
        if (7 == argc) {
            /* the time the allocation is asked for, not how long we hold it */
            PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_ALLOC_TIME, argv[6], PMIX_STRING);
        }
        rc = PMIx_Allocation_request(PMIX_ALLOC_NEW, (0 < ndirs) ? dirs : NULL, ndirs,
                                     &results, &nresults);
        for (n = 0; PMIX_SUCCESS == rc && n < nresults; n++) {
            if (PMIX_CHECK_KEY(&results[n], PMIX_ALLOC_ID) &&
                PMIX_STRING == results[n].value.type) {
                printf("ROLE ALLOCATED %s\n", results[n].value.data.string);
            }
        }
        printf("ROLE ALLOC %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        if (6 <= argc) {
            sleep((unsigned) strtoul(argv[5], NULL, 10));
        }
        while (0 < ndirs) {
            --ndirs;
            PMIX_INFO_DESTRUCT(&dirs[ndirs]);
        }

    } else if (0 == strcmp(op, "instantiate-id")) {
        size_t ndirs = 2;

        sid = (uint32_t) strtoul(argv[3], NULL, 10);
        PMIX_INFO_LOAD(&dirs[0], PMIX_SESSION_INSTANTIATE, &flag, PMIX_BOOL);
        PMIX_INFO_LOAD(&dirs[1], PMIX_ALLOC_NODE_LIST, argv[4], PMIX_STRING);
        if (0 != strcmp(argv[5], "-")) {
            PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_ALLOC_ID, argv[5], PMIX_STRING);
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

    } else if (0 == strcmp(op, "allocnodes")) {
        pmix_query_t query;
        pmix_info_t *results = NULL, *list, *ninfo;
        size_t nresults = 0, n, k;

        /* the nodes of the allocation named, or of the whole DVM */
        PMIX_QUERY_CONSTRUCT(&query);
        query.keys = (char **) calloc(2, sizeof(char *));
        query.keys[0] = strdup(PMIX_QUERY_ALLOCATION);
        if (4 == argc) {
            PMIX_INFO_CREATE(query.qualifiers, 1);
            query.nqual = 1;
            PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_ALLOC_ID, argv[3], PMIX_STRING);
        }
        rc = PMIx_Query_info(&query, 1, &results, &nresults);
        for (n = 0; PMIX_SUCCESS == rc && n < nresults; n++) {
            if (!PMIX_CHECK_KEY(&results[n], PMIX_QUERY_ALLOCATION) ||
                PMIX_DATA_ARRAY != results[n].value.type ||
                NULL == results[n].value.data.darray ||
                PMIX_INFO != results[n].value.data.darray->type) {
                continue;
            }
            list = (pmix_info_t *) results[n].value.data.darray->array;
            for (k = 0; k < results[n].value.data.darray->size; k++) {
                if (!PMIX_CHECK_KEY(&list[k], PMIX_NODE_INFO) ||
                    PMIX_DATA_ARRAY != list[k].value.type ||
                    NULL == list[k].value.data.darray ||
                    0 == list[k].value.data.darray->size) {
                    continue;
                }
                ninfo = (pmix_info_t *) list[k].value.data.darray->array;
                if (PMIX_STRING == ninfo[0].value.type) {
                    printf("ROLE ALLOCNODE %s\n", ninfo[0].value.data.string);
                }
            }
        }
        printf("ROLE ALLOCNODES %s\n", PMIx_Error_string(rc));
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_QUERY_DESTRUCT(&query);

    } else if (0 == strcmp(op, "allocprops")) {
        rc = print_alloc_props(argv[3]);

    } else if (0 == strcmp(op, "alloc-reqid")) {
        pmix_info_t *results = NULL;
        size_t nresults = 0, n;
        unsigned count = (unsigned) strtoul(argv[4], NULL, 10), i;
        char *first = NULL;

        /* count new allocations of no nodes, each under req_id argv[3] */
        PMIX_INFO_LOAD(&dirs[0], PMIX_ALLOC_REQ_ID, argv[3], PMIX_STRING);
        rc = PMIX_SUCCESS;
        for (i = 0; PMIX_SUCCESS == rc && i < count; i++) {
            rc = PMIx_Allocation_request(PMIX_ALLOC_NEW, dirs, 1, &results, &nresults);
            for (n = 0; PMIX_SUCCESS == rc && n < nresults; n++) {
                if (PMIX_CHECK_KEY(&results[n], PMIX_ALLOC_ID) &&
                    PMIX_STRING == results[n].value.type) {
                    printf("ROLE ALLOCATED %s\n", results[n].value.data.string);
                    if (NULL == first) {
                        first = strdup(results[n].value.data.string);
                    }
                }
            }
            if (NULL != results) {
                PMIX_INFO_FREE(results, nresults);
                results = NULL;
            }
        }
        printf("ROLE ALLOC %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&dirs[0]);
        if (PMIX_SUCCESS == rc) {
            /* the extend names the allocation by req_id alone */
            (void) extend_by_req_id(argv[3], "600");
            if (NULL != first) {
                (void) print_alloc_props(first);
            }
        }
        free(first);
        fflush(stdout);
        sleep((unsigned) strtoul(argv[5], NULL, 10));

    } else if (0 == strcmp(op, "alloc-extend-reqid")) {
        rc = extend_by_req_id(argv[3], argv[4]);

    } else if (0 == strcmp(op, "alloc-naming")) {
        pmix_info_t *results = NULL;
        size_t nresults = 0;

        PMIX_INFO_LOAD(&dirs[0], PMIX_ALLOC_ID, argv[3], PMIX_STRING);
        rc = PMIx_Allocation_request(PMIX_ALLOC_NEW, dirs, 1, &results, &nresults);
        printf("ROLE ALLOC %s\n", PMIx_Error_string(rc));
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_INFO_DESTRUCT(&dirs[0]);

    } else if (0 == strcmp(op, "alloc-release")) {
        pmix_info_t *results = NULL;
        size_t nresults = 0;

        PMIX_INFO_LOAD(&dirs[0], PMIX_ALLOC_ID, argv[3], PMIX_STRING);
        rc = PMIx_Allocation_request(PMIX_ALLOC_RELEASE, dirs, 1, &results, &nresults);
        printf("ROLE RELEASE %s\n", PMIx_Error_string(rc));
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_INFO_DESTRUCT(&dirs[0]);

    } else if (0 == strcmp(op, "alloc-extend")) {
        pmix_info_t *results = NULL;
        size_t nresults = 0;

        PMIX_INFO_LOAD(&dirs[0], PMIX_ALLOC_ID, argv[3], PMIX_STRING);
        PMIX_INFO_LOAD(&dirs[1], PMIX_ALLOC_TIME, argv[4], PMIX_STRING);
        rc = PMIx_Allocation_request(PMIX_ALLOC_EXTEND, dirs, 2, &results, &nresults);
        printf("ROLE ALLOC-EXTEND %s\n", PMIx_Error_string(rc));
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_INFO_DESTRUCT(&dirs[0]);
        PMIX_INFO_DESTRUCT(&dirs[1]);

    } else if (0 == strcmp(op, "signal")) {
        pmix_proc_t *tgts;
        int ntgts = argc - 4, k, sig;
        char *colon;

        /* targets are <nspace>:<rank>, "*" for the whole job */
        sig = atoi(argv[3]);
        tgts = (pmix_proc_t *) calloc((size_t) ntgts, sizeof(pmix_proc_t));
        for (k = 0; NULL != tgts && k < ntgts; k++) {
            colon = strrchr(argv[4 + k], ':');
            if (NULL == colon) {
                break;
            }
            *colon = '\0';
            PMIX_LOAD_PROCID(&tgts[k], argv[4 + k],
                             (0 == strcmp(colon + 1, "*")) ? PMIX_RANK_WILDCARD
                                                           : (pmix_rank_t) strtoul(colon + 1, NULL, 10));
        }
        if (NULL == tgts || k < ntgts) {
            rc = PMIX_ERR_BAD_PARAM;
        } else {
            PMIX_INFO_LOAD(&dirs[0], PMIX_JOB_CTRL_SIGNAL, &sig, PMIX_INT);
            lock_init(&lock);
            rc = PMIx_Job_control_nb(tgts, (size_t) ntgts, dirs, 1, ctrl_cb, &lock);
            if (PMIX_SUCCESS != rc) {
                lock_wake(&lock, rc);
            } else {
                lock_wait(&lock);
            }
            rc = lock.status;
            PMIX_INFO_DESTRUCT(&dirs[0]);
        }
        printf("ROLE SIGNAL %s\n", PMIx_Error_string(rc));
        free(tgts);

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
