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
 *   elastic grow   <node[:slots],...>   # grow the DVM onto these nodes
 *   elastic shrink <node,...>           # shrink the DVM by these nodes
 *
 * (extend/release are accepted as aliases for grow/shrink.)
 *
 * A grow may be followed by "-- <cmd> [args...]", in which case the tool
 * spawns that command INTO THE RESERVATION the grow just created, using the
 * PMIX_ALLOC_ID the request handed back and PMIX_SPAWN_TARGET to name it.
 * That is the only way to place a job on a freshly grown node: grown nodes
 * join the new reservation rather than the default pool, so a plain
 * "prun --host <grown-node>" has no allocation to map onto and fails.
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

int main(int argc, char **argv) {
    pmix_status_t rc;
    pmix_info_t *info;
    pmix_alloc_directive_t directive;
    pmix_status_t codes[2];
    lock_t reglock;
    lock_t alloclock;
    const char *op, *nodelist;
    char **spawn_cmd = NULL;
    int rcexit = 0, i;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <grow|shrink> <node1,node2,...> [-- <cmd> [args...]]\n",
                argv[0]);
        return 1;
    }
    op = argv[1];
    nodelist = argv[2];
    /* anything after "--" is a command to run on the grown nodes */
    for (i = 3; i < argc; i++) {
        if (0 == strcmp(argv[i], "--")) {
            if (i + 1 < argc) {
                spawn_cmd = &argv[i + 1];
            }
            break;
        }
    }
    /* A grow is a NEW reservation that names the nodes to add: PRRTE adds them
     * to the pool and extends the DVM.  PMIX_ALLOC_EXTEND only adds to an
     * already-existing reservation, so it is not the right trigger here. */
    if (0 == strcmp(op, "grow") || 0 == strcmp(op, "extend")) {
        directive = PMIX_ALLOC_NEW;
    } else if (0 == strcmp(op, "shrink") || 0 == strcmp(op, "release")) {
        directive = PMIX_ALLOC_RELEASE;
    } else {
        fprintf(stderr, "unknown op '%s' (want grow|shrink)\n", op);
        return 1;
    }

    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, NULL, 0))) {
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

    /* issue the size-change request naming the node list */
    lock_init(&alloclock);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NODE_LIST, nodelist, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, "elastic-test", PMIX_STRING);
    fprintf(stderr, "requesting %s of nodes [%s] ...\n", op, nodelist);
    rc = PMIx_Allocation_request_nb(directive, info, 2, alloc_cb, &alloclock);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "PMIx_Allocation_request_nb failed immediately: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }
    lock_wait(&alloclock);
    PMIX_INFO_FREE(info, 2);

    fprintf(stderr, "waiting for phase-two completion event (60s) ...\n");
    /* crude bounded wait so the tool can't hang forever in a test */
    {
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
