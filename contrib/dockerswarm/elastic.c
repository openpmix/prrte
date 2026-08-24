/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Elastic-DVM test client.
 *
 * Connects to a running PRRTE DVM as a PMIx tool, registers handlers for the
 * two DVM size-change completion events, and issues a grow or shrink request
 * naming a node list.  It then waits for the directed PMIX_DVM_IS_READY
 * (success) or PMIX_ERR_DVM_MOD (failure) event so the two-phase completion
 * contract can be observed end to end.
 *
 *   elastic grow       <node[:slots],...>   # PMIX_ALLOC_NEW     + NODE_LIST
 *   elastic shrink     <node,...>           # PMIX_ALLOC_RELEASE + NODE_LIST
 *   elastic extend     <num-nodes|node,...> # PMIX_ALLOC_EXTEND  + NUM_NODES/NODE_LIST
 *   elastic new        <num-nodes|node,...> # PMIX_ALLOC_NEW     + NUM_NODES/NODE_LIST
 *   elastic release    <num-nodes>          # PMIX_ALLOC_RELEASE + NUM_NODES
 *   elastic release-id <alloc-id>           # PMIX_ALLOC_RELEASE + ALLOC_ID
 *   elastic cancel     <req-id>             # PMIX_ALLOC_REQ_CANCEL
 *   elastic activate   <host-spec>          # PMIX_ALLOC_ACTIVATE + HOST
 *   elastic spawnalloc <node[:slots],...> -- <cmd>
 *                                          # PMIx_Spawn carrying PMIX_SPAWN_ALLOC
 *
 *   --req-id <id>   request id to send (default "elastic-test"); "cancel"
 *                   names the request to cancel this way too
 *   --hostfile <f>  hostfile to send as PMIX_HOSTFILE (activate only; may be
 *                   given with or instead of the host spec, which is then "")
 *   --alloc-dir <d> directive for the spawn-carried request (spawnalloc only):
 *                   new (default), extend, release, reaquire, or "none" to
 *                   leave the directive out and see the request refused
 *   --wait          wait for the phase-two completion event
 *   --no-wait       do not
 *
 * The last five forms exist for the resource managers that serve a size
 * change by talking to a scheduler -- ras/slurm is the one in tree.  A grow
 * it serves may name a count, and then which nodes it lands on is the
 * scheduler's choice, or the nodes themselves, and then the scheduler queues
 * until they are free.  The two grow directives may or may not differ to an
 * RM; ras/slurm treats them as synonyms.
 *
 * Phase two is waited for by default, but not for "cancel", which is answered
 * in full by phase one.
 *
 * An extend is waited for like the rest.  It did not used to be: its nodes
 * join the general pool rather than a reservation (ras/slurm deliberately
 * leaves node->session NULL), and the directed event is addressed to the
 * requestor recorded on a *session*, so there was nothing to wait for.
 * ras/slurm now records the requestor on the session that tracks the
 * allocation and answers phase one with PMIX_OPERATION_IN_PROGRESS, because
 * a grant is not usable nodes until daemons are up on them -- so the
 * PMIX_DVM_IS_READY is the answer, exactly as it is for a grow.  Outside
 * elastic mode no campaign is recorded and phase one stays terminal; use
 * --no-wait there rather than burning the timeout.
 *
 * "activate" is the odd one out: it asks for nothing new at all, only that a
 * daemon be started on nodes the DVM's allocation ALREADY holds but is not
 * spanning (a --prtemca prte_max_vm_size cap leaves such nodes behind, and so
 * does a shrink).  It is therefore the one size change permitted where a
 * scheduler owns the allocation.  Its nodes are named with PMIX_HOST -- the
 * same syntax "--activate" takes on a command line, including "+all", "+n<K>"
 * and "file=<path>" -- and/or with PMIX_HOSTFILE.  Like an extend it answers
 * phase one as soon as the request is granted and reports the daemons through
 * the phase-two event, so it is waited for the same way.
 *
 * A grow may be followed by "-- <cmd> [args...]", in which case the tool
 * spawns that command INTO THE RESERVATION the grow just created, using the
 * PMIX_ALLOC_ID the request handed back and PMIX_SPAWN_TARGET to name it.
 * That is the only way to place a job on a freshly grown node: grown nodes
 * join the new reservation rather than the default pool, so a plain
 * "prun --host <grown-node>" has no allocation to map onto and fails.
 *
 * "spawnalloc" does that whole sequence as ONE call: the spawn carries the
 * allocation request in PMIX_SPAWN_ALLOC, and the host obtains the
 * allocation, waits for it, points the job at it and launches - or, if the
 * allocation is refused, fails the spawn with PMIX_ERR_JOB_ALLOC_FAILED
 * having launched nothing.  It is the "grow, read the id, spawn into it"
 * dance above with the caller taken out of the middle, so the two are worth
 * comparing: this one has no window in which the resources are held by
 * nobody's job.
 *
 * Build: gcc -o elastic elastic.c -lpmix
 */

#include <pmix.h>
#include <pmix_tool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* tiny lock so the main thread can block on async callbacks */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    volatile int active;
    pmix_status_t status;
} lock_t;

static void lock_init(lock_t *l) {
    pthread_mutex_init(&l->mtx, NULL);
    pthread_cond_init(&l->cond, NULL);
    l->active = 1;
    l->status = PMIX_SUCCESS;
}
static void lock_wait(lock_t *l) {
    pthread_mutex_lock(&l->mtx);
    while (l->active) {
        pthread_cond_wait(&l->cond, &l->mtx);
    }
    pthread_mutex_unlock(&l->mtx);
}
static void lock_wake(lock_t *l, pmix_status_t st) {
    pthread_mutex_lock(&l->mtx);
    l->status = st;
    l->active = 0;
    pthread_cond_signal(&l->cond);
    pthread_mutex_unlock(&l->mtx);
}

static lock_t complete_lock;     /* fired by the DVM_IS_READY / ERR_DVM_MOD handler */
static lock_t jobend_lock;       /* fired by the PMIX_EVENT_JOB_END handler */
static pmix_proc_t myproc;
/* PMIX_ALLOC_ID of the reservation the grow created - the handle a later
 * spawn needs in order to target those nodes */
static char alloc_id[PMIX_MAX_KEYLEN + 1];

/* handler registration callback */
static void reg_cb(pmix_status_t status, size_t evref, void *cbdata) {
    lock_t *l = (lock_t *) cbdata;
    (void) evref;
    lock_wake(l, status);
}

/* allocation-request response (phase one: acceptance) */
static void alloc_cb(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                     void *cbdata, pmix_release_cbfunc_t release_fn, void *release_cbdata) {
    lock_t *l = (lock_t *) cbdata;
    size_t n;
    fprintf(stderr, ">>> PHASE 1 (acceptance): allocation request returned %s\n",
            PMIx_Error_string(status));
    for (n = 0; n < ninfo; n++) {
        if (PMIX_STRING == info[n].value.type) {
            fprintf(stderr, "      info[%zu] %s = %s\n", n, info[n].key,
                    info[n].value.data.string);
            /* remember the reservation handle so we can spawn into it */
            if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_ID) &&
                NULL != info[n].value.data.string) {
                snprintf(alloc_id, sizeof(alloc_id), "%s",
                         info[n].value.data.string);
                /* the info keys print as their PMIx attribute strings
                 * ("pmix.alloc.id"), so name it once in a stable form a
                 * caller can grep for */
                fprintf(stderr, ">>> ALLOC_ID %s\n", alloc_id);
            }
        } else {
            fprintf(stderr, "      info[%zu] key=%s\n", n, info[n].key);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    lock_wake(l, status);
}

/* phase two: the directed completion event */
static void completion_evh(size_t evhdlr_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata) {
    size_t n;
    (void) evhdlr_id; (void) source; (void) results; (void) nresults;

    fprintf(stderr, "\n>>> PHASE 2 (completion): received event %s (%d)\n",
            PMIx_Error_string(status), status);
    for (n = 0; n < ninfo; n++) {
        fprintf(stderr, "      payload[%zu] key=%s\n", n, info[n].key);
    }
    if (PMIX_DVM_IS_READY == status) {
        fprintf(stderr, ">>> SUCCESS: the DVM now reflects the requested size\n");
    } else if (PMIX_ERR_DVM_MOD == status) {
        fprintf(stderr, ">>> FAILURE: the DVM modification did not happen\n");
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    lock_wake(&complete_lock, status);
}

/* the spawned job finished */
static void jobend_evh(size_t evhdlr_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t results[], size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata) {
    (void) evhdlr_id; (void) source; (void) info; (void) ninfo;
    (void) results; (void) nresults;
    fprintf(stderr, ">>> spawned job ended: %s\n", PMIx_Error_string(status));
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    lock_wake(&jobend_lock, status);
}

/* Spawn a job onto the nodes of the reservation this tool just created.
 * Returns 0 on success. The reservation is named by PMIX_SPAWN_TARGET; the
 * HNP validates that we own it, resolves it to the session, and maps only
 * onto its nodes - so a proc landing at all proves those nodes are usable
 * (in particular, that they have a topology). */
static int spawn_into_reservation(char **cmd) {
    pmix_app_t app;
    pmix_info_t jinfo[2];
    pmix_nspace_t nspace;
    pmix_status_t rc, code = PMIX_EVENT_JOB_END;
    lock_t reglock;
    bool flag = true;
    int n;

    if ('\0' == alloc_id[0]) {
        fprintf(stderr, ">>> FAILURE: the grow returned no PMIX_ALLOC_ID to spawn into\n");
        return 1;
    }

    lock_init(&jobend_lock);
    lock_init(&reglock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, jobend_evh, reg_cb, &reglock);
    lock_wait(&reglock);

    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup(cmd[0]);
    for (n = 0; NULL != cmd[n]; n++) {
        PMIx_Argv_append_nosize(&app.argv, cmd[n]);
    }
    app.maxprocs = 1;

    PMIX_INFO_LOAD(&jinfo[0], PMIX_SPAWN_TARGET, alloc_id, PMIX_STRING);
    PMIX_INFO_LOAD(&jinfo[1], PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);

    fprintf(stderr, "spawning [%s] into reservation %s ...\n", cmd[0], alloc_id);
    rc = PMIx_Spawn(jinfo, 2, &app, 1, nspace);
    PMIX_APP_DESTRUCT(&app);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, ">>> FAILURE: spawn into %s returned %s\n",
                alloc_id, PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, ">>> SPAWNED %s into reservation %s\n", nspace, alloc_id);

    /* bounded wait for the job to finish so a caller can inspect its work */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;
        pthread_mutex_lock(&jobend_lock.mtx);
        while (jobend_lock.active) {
            if (ETIMEDOUT == pthread_cond_timedwait(&jobend_lock.cond,
                                                    &jobend_lock.mtx, &ts)) {
                fprintf(stderr, ">>> TIMEOUT: spawned job did not end within 60s\n");
                break;
            }
        }
        pthread_mutex_unlock(&jobend_lock.mtx);
    }
    return 0;
}

/* Spawn a job whose allocation request rides along on the spawn itself.
 *
 * The whole request goes into PMIX_SPAWN_ALLOC as an array of pmix_info_t:
 * the directive first (PMIX_ALLOC_REQ_DIRECTIVE - the allocation API takes
 * it as a parameter, and there is no parameter here), then exactly the info
 * a standalone request would have carried.  The host does the rest, and
 * either the spawn succeeds on resources it obtained, or it fails with
 * PMIX_ERR_JOB_ALLOC_FAILED having allocated nothing.
 *
 * Returns 0 on success.
 */
static int spawn_with_alloc(const char *nodes, const char *req_id,
                            const char *dirname, char **cmd) {
    pmix_app_t app;
    pmix_info_t jinfo[2], *areq;
    pmix_data_array_t darray;
    pmix_nspace_t nspace;
    pmix_alloc_directive_t directive = PMIX_ALLOC_NEW;
    pmix_status_t rc, code = PMIX_EVENT_JOB_END;
    lock_t reglock;
    bool flag = true;
    size_t nareq = 3;
    int n;

    lock_init(&jobend_lock);
    lock_init(&reglock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, jobend_evh, reg_cb, &reglock);
    lock_wait(&reglock);

    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup(cmd[0]);
    for (n = 0; NULL != cmd[n]; n++) {
        PMIx_Argv_append_nosize(&app.argv, cmd[n]);
    }
    app.maxprocs = 1;

    /* --alloc-dir names the directive, so the two ways a request can be
     * refused are reachable: one no module will serve, and one that names no
     * directive at all (which is not an allocation failure but a malformed
     * request, and answers differently) */
    if (NULL != dirname) {
        if (0 == strcmp(dirname, "extend")) {
            directive = PMIX_ALLOC_EXTEND;
        } else if (0 == strcmp(dirname, "release")) {
            directive = PMIX_ALLOC_RELEASE;
        } else if (0 == strcmp(dirname, "reaquire")) {
            directive = PMIX_ALLOC_REAQUIRE;
        } else if (0 == strcmp(dirname, "none")) {
            nareq = 2;      /* omit the directive element entirely */
        } else if (0 != strcmp(dirname, "new")) {
            fprintf(stderr, "unknown --alloc-dir '%s'\n", dirname);
            return 1;
        }
    }

    PMIX_INFO_CREATE(areq, nareq);
    n = 0;
    if (3 == nareq) {
        PMIX_INFO_LOAD(&areq[n++], PMIX_ALLOC_REQ_DIRECTIVE, &directive, PMIX_ALLOC_DIRECTIVE);
    }
    PMIX_INFO_LOAD(&areq[n++], PMIX_ALLOC_NODE_LIST, nodes, PMIX_STRING);
    PMIX_INFO_LOAD(&areq[n], PMIX_ALLOC_REQ_ID, req_id, PMIX_STRING);
    darray.type = PMIX_INFO;
    darray.size = nareq;
    darray.array = areq;
    PMIX_INFO_LOAD(&jinfo[0], PMIX_SPAWN_ALLOC, &darray, PMIX_DATA_ARRAY);
    PMIX_INFO_FREE(areq, nareq);
    PMIX_INFO_LOAD(&jinfo[1], PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);

    fprintf(stderr, "spawning [%s] with an allocation request for [%s] ...\n",
            cmd[0], nodes);
    rc = PMIx_Spawn(jinfo, 2, &app, 1, nspace);
    PMIX_APP_DESTRUCT(&app);
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, ">>> SPAWN FAILED: %s\n", PMIx_Error_string(rc));
        if (PMIX_ERR_JOB_ALLOC_FAILED == rc) {
            /* the outcome this attribute exists to make tellable apart: the
             * allocation was refused, so nothing was launched */
            fprintf(stderr, ">>> ALLOCATION REFUSED\n");
        } else if (PMIX_ERR_BAD_PARAM == rc) {
            /* the request itself was malformed - nothing was asked of any
             * allocator, which is a different thing from being refused */
            fprintf(stderr, ">>> REQUEST REJECTED\n");
        }
        return 1;
    }
    fprintf(stderr, ">>> SPAWNED %s on its own allocation\n", nspace);

    /* bounded wait for the job to finish so a caller can inspect its work */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;
        pthread_mutex_lock(&jobend_lock.mtx);
        while (jobend_lock.active) {
            if (ETIMEDOUT == pthread_cond_timedwait(&jobend_lock.cond,
                                                    &jobend_lock.mtx, &ts)) {
                fprintf(stderr, ">>> TIMEOUT: spawned job did not end within 60s\n");
                break;
            }
        }
        pthread_mutex_unlock(&jobend_lock.mtx);
    }
    return 0;
}

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s <op> <arg> [--req-id <id>] [--wait|--no-wait] [-- <cmd> ...]\n"
            "  grow       <node[:slots],...>   PMIX_ALLOC_NEW     + NODE_LIST\n"
            "  shrink     <node,...>           PMIX_ALLOC_RELEASE + NODE_LIST\n"
            "  extend     <num-nodes|node,...> PMIX_ALLOC_EXTEND  + NUM_NODES/NODE_LIST\n"
            "  new        <num-nodes|node,...> PMIX_ALLOC_NEW     + NUM_NODES/NODE_LIST\n"
            "  release    <num-nodes>          PMIX_ALLOC_RELEASE + NUM_NODES\n"
            "  release-id <alloc-id>           PMIX_ALLOC_RELEASE + ALLOC_ID\n"
            "  cancel     <req-id>             PMIX_ALLOC_REQ_CANCEL\n"
            "  activate   <host-spec>          PMIX_ALLOC_ACTIVATE + HOST\n"
            "  spawnalloc <node[:slots],...>   PMIx_Spawn + PMIX_SPAWN_ALLOC\n"
            "                                  (needs \"-- <cmd>\")\n"
            "\n"
            "Every op but 'cancel' waits for the phase-two completion event;\n"
            "--no-wait is what a DVM outside elastic mode needs.\n", me);
}

int main(int argc, char **argv) {
    pmix_status_t rc;
    pmix_info_t *info;
    size_t ninfo = 2;
    pmix_alloc_directive_t directive;
    pmix_status_t codes[2];
    lock_t reglock;
    lock_t alloclock;
    const char *op, *arg, *req_id = "elastic-test";
    const char *hostfile = NULL;
    const char *alloc_dir = NULL;
    char **spawn_cmd = NULL;
    uint64_t num_nodes = 0;
    bool by_count = false;
    bool wait_phase2 = true;
    int wait_opt = -1;          /* an explicit --wait/--no-wait, if given */
    int rcexit = 0, i;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    op = argv[1];
    arg = argv[2];
    for (i = 3; i < argc; i++) {
        /* anything after "--" is a command to run on the grown nodes */
        if (0 == strcmp(argv[i], "--")) {
            if (i + 1 < argc) {
                spawn_cmd = &argv[i + 1];
            }
            break;
        }
        if (0 == strcmp(argv[i], "--req-id") && i + 1 < argc) {
            req_id = argv[++i];
        } else if (0 == strcmp(argv[i], "--hostfile") && i + 1 < argc) {
            hostfile = argv[++i];
        } else if (0 == strcmp(argv[i], "--alloc-dir") && i + 1 < argc) {
            alloc_dir = argv[++i];
        } else if (0 == strcmp(argv[i], "--wait")) {
            wait_opt = 1;
        } else if (0 == strcmp(argv[i], "--no-wait")) {
            wait_opt = 0;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* A grow is a NEW reservation that names the nodes to add: PRRTE adds them
     * to the pool and extends the DVM.  "extend" and "new" name either a count
     * for the resource manager to fill or the nodes themselves, whichever the
     * argument looks like. */
    if (0 == strcmp(op, "grow")) {
        directive = PMIX_ALLOC_NEW;
    } else if (0 == strcmp(op, "shrink")) {
        directive = PMIX_ALLOC_RELEASE;
    } else if (0 == strcmp(op, "extend") || 0 == strcmp(op, "new")) {
        directive = (0 == strcmp(op, "new")) ? PMIX_ALLOC_NEW : PMIX_ALLOC_EXTEND;
        by_count = (strspn(arg, "0123456789") == strlen(arg));
        if (by_count) {
            num_nodes = strtoull(arg, NULL, 10);
        }
    } else if (0 == strcmp(op, "release")) {
        directive = PMIX_ALLOC_RELEASE;
        by_count = true;
        num_nodes = strtoull(arg, NULL, 10);
    } else if (0 == strcmp(op, "release-id")) {
        directive = PMIX_ALLOC_RELEASE;
    } else if (0 == strcmp(op, "cancel")) {
        directive = PMIX_ALLOC_REQ_CANCEL;
        req_id = arg;
        wait_phase2 = false;
    } else if (0 == strcmp(op, "activate")) {
        directive = PMIX_ALLOC_ACTIVATE;
    } else if (0 == strcmp(op, "spawnalloc")) {
        /* no allocation request is issued from here at all - the request
         * rides on the spawn, which is the point */
        directive = PMIX_ALLOC_NEW;
        wait_phase2 = false;
        if (NULL == spawn_cmd) {
            fprintf(stderr, "spawnalloc needs a command: %s spawnalloc <nodes> -- <cmd>\n",
                    argv[0]);
            return 1;
        }
    } else {
        fprintf(stderr, "unknown op '%s'\n", op);
        usage(argv[0]);
        return 1;
    }
    /* an explicit --wait/--no-wait wins over the per-op default */
    if (0 <= wait_opt) {
        wait_phase2 = (1 == wait_opt);
    }

    /* Name the server explicitly when told to.  Rendezvous discovery finds
     * every tool's handle as well as the DVM's, so a case that leaves a prun
     * running while it grows or shrinks cannot connect at all - PMIx sees
     * two candidates and refuses to guess.  PRTE_DVM_URI is this harness's
     * spelling of what --dvm-uri does for a PRRTE tool; leave it unset and
     * discovery works exactly as before. */
    {
        char *duri = getenv("PRTE_DVM_URI");
        pmix_info_t tinfo;
        if (NULL != duri) {
            PMIX_INFO_LOAD(&tinfo, PMIX_SERVER_URI, duri, PMIX_STRING);
            rc = PMIx_tool_init(&myproc, &tinfo, 1);
            PMIX_INFO_DESTRUCT(&tinfo);
        } else {
            rc = PMIx_tool_init(&myproc, NULL, 0);
        }
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, "tool %s:%d connected to the DVM\n", myproc.nspace, myproc.rank);

    /* register for BOTH completion codes in one handler */
    lock_init(&complete_lock);
    lock_init(&reglock);
    codes[0] = PMIX_DVM_IS_READY;
    codes[1] = PMIX_ERR_DVM_MOD;
    PMIx_Register_event_handler(codes, 2, NULL, 0, completion_evh, reg_cb, &reglock);
    lock_wait(&reglock);
    fprintf(stderr, "registered for PMIX_DVM_IS_READY / PMIX_ERR_DVM_MOD\n");

    /* the spawn-carried form never issues a request of its own */
    if (0 == strcmp(op, "spawnalloc")) {
        rcexit = spawn_with_alloc(arg, req_id, alloc_dir, spawn_cmd);
        goto done;
    }

    /* issue the size-change request.  Exactly one selector goes with the
     * request id: the node list, the node count, or the allocation id -- the
     * RM modules reject a request that carries more than one. */
    lock_init(&alloclock);
    if (0 == strcmp(op, "activate")) {
        /* the nodes are named as hosts, not as an allocation request: they
         * are already allocated, and nothing is being asked of a scheduler */
        ninfo = (NULL != hostfile) ? 3 : 2;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_HOST, arg, PMIX_STRING);
        if (NULL != hostfile) {
            PMIX_INFO_LOAD(&info[1], PMIX_HOSTFILE, hostfile, PMIX_STRING);
        }
    } else if (0 == strcmp(op, "cancel")) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
    } else {
        PMIX_INFO_CREATE(info, ninfo);
        if (0 == strcmp(op, "release-id")) {
            PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_ID, arg, PMIX_STRING);
        } else if (by_count) {
            PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NUM_NODES, &num_nodes, PMIX_UINT64);
        } else {
            PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NODE_LIST, arg, PMIX_STRING);
        }
    }
    PMIX_INFO_LOAD(&info[ninfo - 1], PMIX_ALLOC_REQ_ID, req_id, PMIX_STRING);
    fprintf(stderr, "requesting %s [%s] (req-id %s) ...\n", op, arg, req_id);
    rc = PMIx_Allocation_request_nb(directive, info, ninfo, alloc_cb, &alloclock);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "PMIx_Allocation_request_nb failed immediately: %s\n",
                PMIx_Error_string(rc));
        rcexit = 1;
        goto done;
    }
    lock_wait(&alloclock);
    PMIX_INFO_FREE(info, ninfo);

    /* A phase one that failed is the end of it: nothing was started, so no
     * completion event is coming and waiting would just burn the timeout. */
    if (PMIX_SUCCESS != alloclock.status &&
        PMIX_OPERATION_SUCCEEDED != alloclock.status &&
        PMIX_OPERATION_IN_PROGRESS != alloclock.status) {
        fprintf(stderr, ">>> REJECTED: %s\n", PMIx_Error_string(alloclock.status));
        rcexit = 1;
        wait_phase2 = false;
    }

    if (wait_phase2) {
        fprintf(stderr, "waiting for phase-two completion event (60s) ...\n");
        /* crude bounded wait so the tool can't hang forever in a test */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;
        pthread_mutex_lock(&complete_lock.mtx);
        while (complete_lock.active) {
            if (ETIMEDOUT == pthread_cond_timedwait(&complete_lock.cond,
                                                    &complete_lock.mtx, &ts)) {
                fprintf(stderr, ">>> TIMEOUT: no completion event within 60s\n");
                break;
            }
        }
        pthread_mutex_unlock(&complete_lock.mtx);
    }

    /* only meaningful once the DVM actually reflects the new nodes */
    if (NULL != spawn_cmd) {
        if (PMIX_DVM_IS_READY == complete_lock.status) {
            rcexit = spawn_into_reservation(spawn_cmd);
        } else {
            fprintf(stderr, ">>> skipping spawn: the grow did not complete\n");
            rcexit = 1;
        }
    }

done:
    PMIx_tool_finalize();
    return rcexit;
}
