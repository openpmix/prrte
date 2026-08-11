/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <pmix.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERIFY_TIMEOUT 10
#define REQUEST_TIMEOUT 10
/* Phase one blocks nobody, so it can outlast a queued request. */
#define REQUEST_WATCH_TIMEOUT 300
#define DVM_READY_TIMEOUT 60
#define PRUN_TIMEOUT 10
#define LINE_MAX_LEN 1024
#define MAX_PENDING 8
#define REQID_LEN 128

/* PMIx defines the two-phase completion events together or not at all. */
#if defined(PMIX_DVM_IS_READY) && defined(PMIX_ERR_DVM_MOD)
#    define HAVE_DVM_MOD_EVENTS 1
#else
#    define HAVE_DVM_MOD_EVENTS 0
#endif

static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

/* The verify polls re-query once a second for up to VERIFY_TIMEOUT, which
 * traces enough to bury the result. Only the poll loops set this. */
static int queries_quiet = 0;

static void record_message(const char *fmt, ...);
static int line_seen(char **lines, const char *line);

static void trace_point(const char *msg)
{
    record_message("[tester] %s\n", msg);
}

/* Refcounted: a timeout does not withdraw the request, so the callback may run
 * long after the submitter gave up. Last reference out frees. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int done;
    int refs;
    pmix_status_t status;
} wait_t;

typedef struct {
    char *id;
    char **nodes;
    size_t nnodes;
} alloc_record_t;

typedef struct {
    alloc_record_t *allocs;
    size_t nallocs;
    size_t size;
} alloc_snapshot_t;

static wait_t *wait_new(void)
{
    wait_t *w = calloc(1, sizeof(*w));

    if (NULL == w) {
        return NULL;
    }
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->done = 0;
    w->refs = 2;
    w->status = PMIX_SUCCESS;
    return w;
}

static void wait_release(wait_t *w)
{
    int last;

    pthread_mutex_lock(&w->lock);
    last = (0 == --w->refs);
    pthread_mutex_unlock(&w->lock);
    if (!last) {
        return;
    }
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
    free(w);
}

static int wait_for_callback(wait_t *w, int seconds)
{
    struct timespec ts;

    if (0 != clock_gettime(CLOCK_REALTIME, &ts)) {
        return -1;
    }
    ts.tv_sec += seconds;

    pthread_mutex_lock(&w->lock);
    while (!w->done) {
        int rc = pthread_cond_timedwait(&w->cond, &w->lock, &ts);
        if (ETIMEDOUT == rc) {
            pthread_mutex_unlock(&w->lock);
            return -1;
        }
        if (0 != rc) {
            pthread_mutex_unlock(&w->lock);
            return -2;
        }
    }
    pthread_mutex_unlock(&w->lock);
    return 0;
}

/* Applied to the outcome of both phases, so PMIX_OPERATION_IN_PROGRESS here
 * means phase two could not be observed and the grant stood as the answer. */
static int status_is_accepted(pmix_status_t status)
{
    if (PMIX_SUCCESS == status) {
        return 1;
    }
#ifdef PMIX_OPERATION_SUCCEEDED
    if (PMIX_OPERATION_SUCCEEDED == status) {
        return 1;
    }
#endif
#ifdef PMIX_OPERATION_IN_PROGRESS
    if (PMIX_OPERATION_IN_PROGRESS == status) {
        return 1;
    }
#endif
#if HAVE_DVM_MOD_EVENTS
    if (PMIX_DVM_IS_READY == status) {
        return 1;
    }
#endif
    return 0;
}

/* Three ways to fail, needing different responses: still running, consumed
 * resources, did nothing. */
static const char *outcome_phrase(pmix_status_t rc)
{
    if (PMIX_ERR_TIMEOUT == rc) {
        return "is still outstanding";
    }
#if HAVE_DVM_MOD_EVENTS
    if (PMIX_ERR_DVM_MOD == rc) {
        return "was granted, but the DVM could not be resized";
    }
#endif
    return "was rejected";
}

static void alloc_cbfunc(pmix_status_t status, pmix_info_t *results,
                         size_t nresults, void *cbdata,
                         pmix_release_cbfunc_t release_fn,
                         void *release_cbdata)
{
    wait_t *w = (wait_t *) cbdata;

    record_message("PMIx allocation callback: %s (%zu result%s)\n",
                   PMIx_Error_string(status), nresults,
                   1 == nresults ? "" : "s");
    for (size_t n = 0; n < nresults; n++) {
        if (PMIX_STRING == results[n].value.type) {
            record_message("  %s = %s\n", results[n].key,
                           results[n].value.data.string);
        } else {
            record_message("  %s type=%d\n", results[n].key,
                           (int) results[n].value.type);
        }
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    pthread_mutex_lock(&w->lock);
    w->status = status;
    w->done = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);

    wait_release(w);
}

/* Phase two: PMIX_DVM_IS_READY (or PMIX_ERR_DVM_MOD) once the DVM has resized.
 * Correlated with the request by PMIX_ALLOC_REQ_ID. */
typedef struct {
    char *reqid;         /* NULL when the slot is free */
    char what[64];       /* what was asked for, for the 'pending' listing */
    char allocid[64];    /* the allocation phase two named, if it named one */
    int submitted;       /* phase one has not answered yet */
    int cancellable;     /* only an extend ever is - see pending_report */
    int done;            /* phase two arrived */
    pmix_status_t status;
} pending_t;

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pending_cond = PTHREAD_COND_INITIALIZER;
static pending_t pending[MAX_PENDING];
static int dvm_events_available = 0;

static int pending_register(const char *reqid, const char *what, int cancellable)
{
    int slot = -1;

    pthread_mutex_lock(&pending_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (NULL == pending[i].reqid) {
            pending[i].reqid = strdup(reqid);
            pending[i].done = 0;
            pending[i].submitted = 1;
            pending[i].cancellable = cancellable;
            pending[i].status = PMIX_SUCCESS;
            pending[i].allocid[0] = '\0';
            snprintf(pending[i].what, sizeof(pending[i].what), "%s",
                     NULL == what ? "request" : what);
            slot = NULL == pending[i].reqid ? -1 : i;
            break;
        }
    }
    pthread_mutex_unlock(&pending_lock);

    if (0 > slot) {
        record_message("no free slot to track request %s; its completion "
                       "event will be reported but not waited for\n", reqid);
    }
    return slot;
}

/* The allocation phase two named for this request, empty if none. */
static void pending_allocid(int slot, char *buf, size_t size)
{
    buf[0] = '\0';
    if (0 > slot) {
        return;
    }
    pthread_mutex_lock(&pending_lock);
    snprintf(buf, size, "%s", pending[slot].allocid);
    pthread_mutex_unlock(&pending_lock);
}

/* The scheduler has acted; a cancel can no longer reach the request. */
static void pending_mark_granted(int slot)
{
    if (0 > slot) {
        return;
    }
    pthread_mutex_lock(&pending_lock);
    pending[slot].submitted = 0;
    pthread_mutex_unlock(&pending_lock);
}

/* Only an extend is ever cancellable: ras/slurm registers a cancellable record
 * on the extend path alone. */
static void pending_report(void)
{
    int found = 0;

    pthread_mutex_lock(&pending_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (NULL == pending[i].reqid) {
            continue;
        }
        if (!found) {
            printf("outstanding requests:\n");
            found = 1;
        }
        printf("  %-24s %-14s %s\n", pending[i].reqid, pending[i].what,
               !pending[i].cancellable
                   ? "not cancellable"
                   : pending[i].submitted
                         ? "queued at the scheduler - cancellable"
                         : "granted, DVM resizing - too late to cancel");
    }
    if (!found) {
        printf("no outstanding requests\n");
    }
    fflush(stdout);
    pthread_mutex_unlock(&pending_lock);
}

/* Reports an unknown name separately, so a typo does not read as "too late". */
static int pending_is_cancellable(const char *reqid, int *known)
{
    int cancellable = 0;

    *known = 0;
    pthread_mutex_lock(&pending_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (NULL != pending[i].reqid && 0 == strcmp(pending[i].reqid, reqid)) {
            *known = 1;
            cancellable = pending[i].cancellable && pending[i].submitted;
            break;
        }
    }
    pthread_mutex_unlock(&pending_lock);

    return cancellable;
}

/* The name 'cancel' means with no argument. Unambiguous only when one request
 * is still cancellable. */
static int pending_sole_cancellable(char *buf, size_t size)
{
    int found = -1;

    pthread_mutex_lock(&pending_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (NULL != pending[i].reqid && pending[i].submitted
            && pending[i].cancellable) {
            if (0 <= found) {
                pthread_mutex_unlock(&pending_lock);
                return -1;
            }
            found = i;
        }
    }
    if (0 <= found) {
        snprintf(buf, size, "%s", pending[found].reqid);
    }
    pthread_mutex_unlock(&pending_lock);

    return 0 <= found ? 0 : -1;
}

static void pending_drop(int slot)
{
    if (0 > slot) {
        return;
    }
    pthread_mutex_lock(&pending_lock);
    free(pending[slot].reqid);
    pending[slot].reqid = NULL;
    pthread_cond_broadcast(&pending_cond);
    pthread_mutex_unlock(&pending_lock);
}

static void pending_forget_reqid(const char *reqid)
{
    pthread_mutex_lock(&pending_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (NULL != pending[i].reqid && 0 == strcmp(pending[i].reqid, reqid)) {
            free(pending[i].reqid);
            pending[i].reqid = NULL;
        }
    }
    pthread_cond_broadcast(&pending_cond);
    pthread_mutex_unlock(&pending_lock);
}

static pmix_status_t pending_wait(int slot, int seconds)
{
    struct timespec deadline;
    pmix_status_t status = PMIX_ERR_TIMEOUT;
    int rc = 0;

    if (0 != clock_gettime(CLOCK_REALTIME, &deadline)) {
        return PMIX_ERROR;
    }
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&pending_lock);
    while (!pending[slot].done && 0 == rc) {
        rc = pthread_cond_timedwait(&pending_cond, &pending_lock, &deadline);
    }
    if (pending[slot].done) {
        status = pending[slot].status;
    }
    pthread_mutex_unlock(&pending_lock);

    /* A slot left registered on timeout is deliberate: the operation is still
     * running, and a late event should still be matched to it. */
    return status;
}

#if HAVE_DVM_MOD_EVENTS
static void dvm_mod_evhandler(size_t evhdlr_registration_id,
                              pmix_status_t status, const pmix_proc_t *source,
                              pmix_info_t info[], size_t ninfo,
                              pmix_info_t results[], size_t nresults,
                              pmix_event_notification_cbfunc_fn_t cbfunc,
                              void *cbdata)
{
    const char *allocid = NULL;
    const char *reqid = NULL;

    (void) evhdlr_registration_id;
    (void) source;
    (void) results;
    (void) nresults;

    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_STRING != info[n].value.type) {
            continue;
        }
        if (PMIx_Check_key(info[n].key, PMIX_ALLOC_ID)) {
            allocid = info[n].value.data.string;
        } else if (PMIx_Check_key(info[n].key, PMIX_ALLOC_REQ_ID)) {
            reqid = info[n].value.data.string;
        }
    }

    record_message("\n"
                   "======================================================\n"
                   "  EVENT %s\n"
                   "  %s\n"
                   "  request    %s\n"
                   "  allocation %s\n"
                   "======================================================\n",
                   PMIx_Error_string(status),
                   PMIX_DVM_IS_READY == status
                       ? "the DVM has finished resizing; the nodes can carry work"
                       : "the scheduler granted, but the DVM could not be resized",
                   NULL == reqid ? "unnamed" : reqid,
                   NULL == allocid ? "unknown" : allocid);

    if (NULL != reqid) {
        pthread_mutex_lock(&pending_lock);
        for (int i = 0; i < MAX_PENDING; i++) {
            if (NULL != pending[i].reqid &&
                0 == strcmp(pending[i].reqid, reqid)) {
                pending[i].status = status;
                pending[i].done = 1;
                if (NULL != allocid) {
                    snprintf(pending[i].allocid, sizeof(pending[i].allocid),
                             "%s", allocid);
                }
                break;
            }
        }
        pthread_cond_broadcast(&pending_cond);
        pthread_mutex_unlock(&pending_lock);
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}
#endif

static void register_dvm_mod_handler(void)
{
#if HAVE_DVM_MOD_EVENTS
    pmix_status_t codes[2] = {PMIX_DVM_IS_READY, PMIX_ERR_DVM_MOD};
    pmix_status_t rc;

    rc = PMIx_Register_event_handler(codes, 2, NULL, 0, dvm_mod_evhandler,
                                     NULL, NULL);
    if (0 > rc) {
        fprintf(stderr, "could not register DVM modification handler: %s\n",
                PMIx_Error_string(rc));
        return;
    }
    dvm_events_available = 1;
    printf("Registered for PMIX_DVM_IS_READY / PMIX_ERR_DVM_MOD\n");
#else
    printf("This PMIx has no DVM modification event codes; allocation "
           "requests will be treated as complete when the scheduler answers\n");
#endif
}

static void snapshot_init(alloc_snapshot_t *snap)
{
    snap->allocs = NULL;
    snap->nallocs = 0;
    snap->size = 0;
}

static void snapshot_free(alloc_snapshot_t *snap)
{
    for (size_t i = 0; i < snap->nallocs; i++) {
        free(snap->allocs[i].id);
        if (NULL != snap->allocs[i].nodes) {
            PMIx_Argv_free(snap->allocs[i].nodes);
        }
    }
    free(snap->allocs);
    snapshot_init(snap);
}

static alloc_record_t *snapshot_add_alloc(alloc_snapshot_t *snap,
                                          const char *id)
{
    alloc_record_t *tmp;

    if (snap->nallocs == snap->size) {
        size_t newsize = 0 == snap->size ? 4 : 2 * snap->size;

        tmp = realloc(snap->allocs, newsize * sizeof(*snap->allocs));
        if (NULL == tmp) {
            return NULL;
        }
        snap->allocs = tmp;
        snap->size = newsize;
    }

    tmp = &snap->allocs[snap->nallocs++];
    tmp->id = strdup(id);
    tmp->nodes = NULL;
    tmp->nnodes = 0;
    if (NULL == tmp->id) {
        snap->nallocs--;
        return NULL;
    }
    return tmp;
}

static void snapshot_remove_last_alloc(alloc_snapshot_t *snap)
{
    alloc_record_t *rec;

    if (0 == snap->nallocs) {
        return;
    }

    rec = &snap->allocs[snap->nallocs - 1];
    free(rec->id);
    if (NULL != rec->nodes) {
        PMIx_Argv_free(rec->nodes);
    }
    snap->nallocs--;
}

static alloc_record_t *snapshot_find_alloc(const alloc_snapshot_t *snap,
                                           const char *id)
{
    for (size_t i = 0; i < snap->nallocs; i++) {
        if (0 == strcmp(snap->allocs[i].id, id)) {
            return &((alloc_snapshot_t *) snap)->allocs[i];
        }
    }
    return NULL;
}

static size_t snapshot_total_nodes(const alloc_snapshot_t *snap)
{
    size_t total = 0;

    for (size_t i = 0; i < snap->nallocs; i++) {
        total += snap->allocs[i].nnodes;
    }
    return total;
}

static int snapshot_has_node(const alloc_snapshot_t *snap, const char *node)
{
    for (size_t i = 0; i < snap->nallocs; i++) {
        for (size_t j = 0; NULL != snap->allocs[i].nodes &&
                           NULL != snap->allocs[i].nodes[j]; j++) {
            if (0 == strcmp(snap->allocs[i].nodes[j], node)) {
                return 1;
            }
        }
    }
    return 0;
}

static char **snapshot_unique_nodes(const alloc_snapshot_t *snap)
{
    char **nodes = NULL;

    for (size_t i = 0; i < snap->nallocs; i++) {
        for (size_t j = 0; NULL != snap->allocs[i].nodes &&
                           NULL != snap->allocs[i].nodes[j]; j++) {
            int arc;

            if (NULL != nodes && line_seen(nodes, snap->allocs[i].nodes[j])) {
                continue;
            }
            PMIX_ARGV_APPEND(arc, nodes, snap->allocs[i].nodes[j]);
            if (PMIX_SUCCESS != arc) {
                PMIx_Argv_free(nodes);
                return NULL;
            }
        }
    }

    return nodes;
}

/* Watcher threads and the PMIx event handler can all print at once. */
static void record_message(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&output_lock);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&output_lock);
}

static pmix_status_t query_one(const char *key, const char *allocid,
                               pmix_info_t **results, size_t *nresults)
{
    pmix_query_t *query;
    pmix_status_t rc;
    int arc;

    PMIX_QUERY_CREATE(query, 1);
    PMIX_ARGV_APPEND(arc, query[0].keys, key);
    if (PMIX_SUCCESS != arc) {
        PMIX_QUERY_FREE(query, 1);
        return (pmix_status_t) arc;
    }

    if (NULL != allocid) {
        PMIX_QUERY_QUALIFIERS_CREATE(&query[0], 1);
        PMIX_INFO_LOAD(&query[0].qualifiers[0], PMIX_ALLOC_ID, allocid,
                       PMIX_STRING);
    }

    if (!queries_quiet) {
        record_message("[tester] query begin: %s%s%s\n", key,
                       NULL == allocid ? "" : " alloc=",
                       NULL == allocid ? "" : allocid);
    }
    rc = PMIx_Query_info(query, 1, results, nresults);
    if (!queries_quiet) {
        record_message("[tester] query end: %s -> %s (%zu result%s)\n", key,
                       PMIx_Error_string(rc), NULL == nresults ? 0 : *nresults,
                       NULL != nresults && 1 == *nresults ? "" : "s");
    }
    PMIX_QUERY_FREE(query, 1);
    return rc;
}

static int append_node(char ***nodes, const char *node)
{
    int rc;

    if (NULL == node) {
        return 0;
    }
    PMIX_ARGV_APPEND(rc, *nodes, node);
    return PMIX_SUCCESS == rc ? 0 : -1;
}

static int fill_alloc_nodes(alloc_record_t *rec)
{
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    rc = query_one(PMIX_QUERY_ALLOCATION, rec->id, &results, &nresults);
    if (PMIX_ERR_NOT_FOUND == rc) {
        printf("PMIx query %s for %s: expected not found; allocation is gone\n",
               PMIX_QUERY_ALLOCATION, rec->id);
        return 1;
    }
    if (PMIX_SUCCESS != rc && PMIX_ERR_PARTIAL_SUCCESS != rc) {
        fprintf(stderr, "PMIx query %s for %s failed: %s\n",
                PMIX_QUERY_ALLOCATION, rec->id, PMIx_Error_string(rc));
        return -1;
    }

    for (size_t i = 0; i < nresults; i++) {
        pmix_data_array_t *darray;
        pmix_info_t *info;

        if (!PMIx_Check_key(results[i].key, PMIX_QUERY_ALLOCATION) ||
            PMIX_DATA_ARRAY != results[i].value.type) {
            continue;
        }

        darray = results[i].value.data.darray;
        if (NULL == darray || PMIX_INFO != darray->type) {
            continue;
        }

        info = (pmix_info_t *) darray->array;
        for (size_t j = 0; j < darray->size; j++) {
            pmix_data_array_t *nodearray;
            pmix_info_t *nodeinfo;

            if (!PMIx_Check_key(info[j].key, PMIX_NODE_INFO) ||
                PMIX_DATA_ARRAY != info[j].value.type) {
                continue;
            }

            nodearray = info[j].value.data.darray;
            if (NULL == nodearray || PMIX_INFO != nodearray->type) {
                continue;
            }

            nodeinfo = (pmix_info_t *) nodearray->array;
            for (size_t k = 0; k < nodearray->size; k++) {
                if (PMIx_Check_key(nodeinfo[k].key, PMIX_HOSTNAME) &&
                    PMIX_STRING == nodeinfo[k].value.type) {
                    if (0 != append_node(&rec->nodes,
                                         nodeinfo[k].value.data.string)) {
                        PMIX_INFO_FREE(results, nresults);
                        return -1;
                    }
                    rec->nnodes++;
                    break;
                }
            }
        }
    }

    PMIX_INFO_FREE(results, nresults);
    return 0;
}

static int query_snapshot(alloc_snapshot_t *snap)
{
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    snapshot_init(snap);

    rc = query_one(PMIX_QUERY_ALLOC_IDS, NULL, &results, &nresults);
    if (PMIX_SUCCESS != rc && PMIX_ERR_PARTIAL_SUCCESS != rc) {
        fprintf(stderr, "PMIx query %s failed: %s\n", PMIX_QUERY_ALLOC_IDS,
                PMIx_Error_string(rc));
        return -1;
    }

    for (size_t i = 0; i < nresults; i++) {
        pmix_data_array_t *darray;
        pmix_info_t *info;

        if (!PMIx_Check_key(results[i].key, PMIX_QUERY_ALLOC_IDS) ||
            PMIX_DATA_ARRAY != results[i].value.type) {
            continue;
        }

        darray = results[i].value.data.darray;
        if (NULL == darray || PMIX_INFO != darray->type) {
            continue;
        }

        info = (pmix_info_t *) darray->array;
        for (size_t j = 0; j < darray->size; j++) {
            alloc_record_t *rec;

            if (!PMIx_Check_key(info[j].key, PMIX_ALLOC_ID) ||
                PMIX_STRING != info[j].value.type) {
                continue;
            }

            rec = snapshot_add_alloc(snap, info[j].value.data.string);
            if (NULL == rec) {
                PMIX_INFO_FREE(results, nresults);
                snapshot_free(snap);
                return -1;
            }
            int fill_rc = fill_alloc_nodes(rec);
            if (0 < fill_rc) {
                snapshot_remove_last_alloc(snap);
                continue;
            }
            if (0 > fill_rc) {
                PMIX_INFO_FREE(results, nresults);
                snapshot_free(snap);
                return -1;
            }
        }
    }

    PMIX_INFO_FREE(results, nresults);
    return 0;
}

static void print_snapshot(const alloc_snapshot_t *snap)
{
    printf("\nPRRTE allocations: %zu allocation%s, %zu node%s total\n",
           snap->nallocs, 1 == snap->nallocs ? "" : "s",
           snapshot_total_nodes(snap),
           1 == snapshot_total_nodes(snap) ? "" : "s");
    for (size_t i = 0; i < snap->nallocs; i++) {
        printf("  alloc %s: %zu node%s", snap->allocs[i].id,
               snap->allocs[i].nnodes, 1 == snap->allocs[i].nnodes ? "" : "s");
        for (size_t j = 0; NULL != snap->allocs[i].nodes &&
                           NULL != snap->allocs[i].nodes[j]; j++) {
            printf(" %s", snap->allocs[i].nodes[j]);
        }
        printf("\n");
    }
}

static int run_command(const char *title, const char *cmd)
{
    FILE *fp;
    char full_cmd[2 * LINE_MAX_LEN];
    char line[LINE_MAX_LEN];
    int rc;

    printf("\n== %s ==\n$ %s\n", title, cmd);
    printf("(deduplicating parsed rank/pid/host output)\n");
    fflush(stdout);
    snprintf(full_cmd, sizeof(full_cmd), "timeout -k 5s %ds %s </dev/null 2>&1",
             PRUN_TIMEOUT, cmd);
    fp = popen(full_cmd, "r");
    if (NULL == fp) {
        fprintf(stderr, "failed to run command: %s\n", strerror(errno));
        return -1;
    }

    while (NULL != fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
    }

    rc = pclose(fp);
    if (-1 == rc) {
        fprintf(stderr, "failed to close command: %s\n", strerror(errno));
        return -1;
    }
    return rc;
}

static void run_prun_command(const char *cmd)
{
    char full_cmd[2 * LINE_MAX_LEN];
    int rc;

    printf("\n$ %s\n", cmd);
    fflush(stdout);
    snprintf(full_cmd, sizeof(full_cmd), "(%s) >/dev/null 2>&1", cmd);
    rc = system(full_cmd);
    if (-1 == rc) {
        fprintf(stderr, "failed to run prun: %s\n", strerror(errno));
    }
}

static int all_digits(const char *s)
{
    if (NULL == s || '\0' == s[0]) {
        return 0;
    }
    for (size_t i = 0; '\0' != s[i]; i++) {
        if (!isdigit((unsigned char) s[i])) {
            return 0;
        }
    }
    return 1;
}

static int squeue_job_exists(const char *jobid)
{
    char cmd[256];
    char line[LINE_MAX_LEN];
    FILE *fp;
    int exists = 0;

    if (!all_digits(jobid)) {
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "squeue -h -j %s 2>/dev/null", jobid);
    fp = popen(cmd, "r");
    if (NULL == fp) {
        return -1;
    }
    if (NULL != fgets(line, sizeof(line), fp)) {
        exists = 1;
    }
    pclose(fp);
    return exists;
}

static void print_squeue(void)
{
    const char *user = getenv("USER");
    char cmd[512];

    if (NULL != user && '\0' != user[0]) {
        snprintf(cmd, sizeof(cmd),
                 "squeue -u %s -o '%%.18i %%.9T %%.12j %%.40N'", user);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "squeue -o '%%.18i %%.9T %%.12j %%.40N'");
    }
    run_command("Slurm queue", cmd);
}

static int line_seen(char **lines, const char *line)
{
    for (int i = 0; NULL != lines && NULL != lines[i]; i++) {
        if (0 == strcmp(lines[i], line)) {
            return 1;
        }
    }
    return 0;
}

static void make_prun_output_key(const char *line, char *key, size_t keylen)
{
    char rank[64];
    char pid[64];
    char host[256];
    const char *start;

    start = strstr(line, "rank=");
    if (NULL != start &&
        3 == sscanf(start, "rank=%63[0-9] pid=%63[0-9] host=%255[A-Za-z0-9_.-]",
                    rank, pid, host)) {
        snprintf(key, keylen, "rank=%s pid=%s host=%s", rank, pid, host);
        return;
    }

    snprintf(key, keylen, "%s", line);
}

static int run_unique_command(const char *title, const char *cmd)
{
    FILE *fp;
    char full_cmd[2 * LINE_MAX_LEN];
    char line[LINE_MAX_LEN];
    char key[LINE_MAX_LEN];
    char **seen = NULL;
    int suppressed = 0;
    int unique = 0;
    int rc;

    printf("\n== %s ==\n$ %s\n", title, cmd);
    fflush(stdout);
    snprintf(full_cmd, sizeof(full_cmd), "timeout -k 5s %ds %s </dev/null 2>&1",
             PRUN_TIMEOUT, cmd);
    fp = popen(full_cmd, "r");
    if (NULL == fp) {
        fprintf(stderr, "failed to run command: %s\n", strerror(errno));
        return -1;
    }

    while (NULL != fgets(line, sizeof(line), fp)) {
        int arc;

        make_prun_output_key(line, key, sizeof(key));
        if (line_seen(seen, key)) {
            suppressed++;
            continue;
        }
        PMIX_ARGV_APPEND(arc, seen, key);
        if (PMIX_SUCCESS == arc) {
            unique++;
        }
    }

    rc = pclose(fp);
    printf("reachability: %d unique rank/pid/host line%s",
           unique, 1 == unique ? "" : "s");
    if (0 < suppressed) {
        printf(" (%d duplicate IOF line%s)",
               suppressed, 1 == suppressed ? "" : "s");
    }
    printf("\n");
    if (NULL != seen) {
        PMIx_Argv_free(seen);
    }
    if (-1 == rc) {
        fprintf(stderr, "failed to close command: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(rc) && 124 == WEXITSTATUS(rc)) {
        printf("reachability probe timed out after %d seconds\n", PRUN_TIMEOUT);
    }
    return rc;
}

static void run_prun_hostname(const alloc_snapshot_t *snap)
{
    char cmd[512];
    char **nodes;
    int nnodes;

    nodes = snapshot_unique_nodes(snap);
    nnodes = PMIx_Argv_count(nodes);
    if (0 == nnodes) {
        printf("\n== prun hostname ==\n(no known PRRTE nodes)\n");
        PMIx_Argv_free(nodes);
        return;
    }

    printf("\n== prun hostname across DVM ==\n");
    snprintf(cmd, sizeof(cmd),
             "prun -np %d --map-by ppr:1:node sh -c "
             "'echo rank=${PMIX_RANK:-?} pid=$$ host=$(hostname)'",
             nnodes);
    run_unique_command("prun hostname", cmd);
    PMIx_Argv_free(nodes);
}

static void show_state(void)
{
    alloc_snapshot_t snap;

    trace_point("show_state: begin");
    print_squeue();
    trace_point("show_state: querying PRRTE snapshot");
    if (0 == query_snapshot(&snap)) {
        trace_point("show_state: printing PRRTE snapshot");
        print_snapshot(&snap);
        snapshot_free(&snap);
    } else {
        printf("\nPRRTE allocation query failed\n");
    }
    trace_point("show_state: end");
}

static void handle_dvm(void)
{
    alloc_snapshot_t snap;

    trace_point("dvm: querying PRRTE snapshot");
    if (0 == query_snapshot(&snap)) {
        trace_point("dvm: running reachability probe");
        run_prun_hostname(&snap);
        snapshot_free(&snap);
    } else {
        printf("\nPRRTE allocation query failed\n");
    }
}

/* For single-phase requests. A cancel is answered from the daemon's own
 * records, so nothing follows it. */
static pmix_status_t submit_and_wait(pmix_alloc_directive_t directive,
                                     pmix_info_t *info, size_t ninfo,
                                     const char *description)
{
    pmix_status_t rc;
    wait_t *w;

    w = wait_new();
    if (NULL == w) {
        return PMIX_ERR_NOMEM;
    }

    record_message("\nSubmitting %s\n", description);
    rc = PMIx_Allocation_request_nb(directive, info, ninfo, alloc_cbfunc, w);
    if (PMIX_SUCCESS != rc) {
        record_message("PMIx_Allocation_request_nb failed immediately: %s\n",
                       PMIx_Error_string(rc));
        /* The callback will not run, so drop its reference here too. */
        wait_release(w);
        wait_release(w);
        return rc;
    }

    if (0 != wait_for_callback(w, REQUEST_TIMEOUT)) {
        record_message("timed out waiting for the PMIx allocation callback\n");
        wait_release(w);
        return PMIX_ERR_TIMEOUT;
    }

    rc = w->status;
    wait_release(w);

    return rc;
}

/* A size change is watched on its own thread so the prompt returns at once.
 * ras/slurm drops a request's cancellable record when the scheduler grants,
 * and phase one does not answer until then, so blocking the command reader
 * across that wait would hold the prompt for the whole cancellable window. */
typedef enum {
    VERIFY_NONE = 0,
    VERIFY_NODES_AT_LEAST,
    VERIFY_NODES_AT_MOST,
    VERIFY_ALLOC_GONE,
    VERIFY_NODES_ABSENT
} verify_kind_t;

typedef struct {
    pmix_info_t *info;
    size_t ninfo;
    char reqid[REQID_LEN];
    char description[64];
    int slot;
    wait_t *w;
    verify_kind_t verify;
    size_t target;
    size_t requested;    /* nodes this request asked for */
    char subject[256];   /* allocation id or node list, per verify kind */
    int was_in_squeue;
} watch_t;

static int wait_until_alloc_count_at_least(size_t min_count);
static int wait_until_total_nodes_at_most(size_t max_count);
static int wait_until_alloc_removed(const char *allocid);

/* Nodes PRRTE has under one allocation. Polls, since the snapshot may lag the
 * event by a moment. Returns -1 if the allocation never appears. */
static ssize_t wait_for_alloc_nodes(const char *allocid)
{
    ssize_t nnodes = -1;

    queries_quiet = 1;
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;

        if (0 == query_snapshot(&snap)) {
            alloc_record_t *rec = snapshot_find_alloc(&snap, allocid);

            if (NULL != rec) {
                nnodes = (ssize_t) rec->nnodes;
            }
            snapshot_free(&snap);
        }
        if (0 <= nnodes) {
            break;
        }
        sleep(1);
    }
    queries_quiet = 0;

    return nnodes;
}

/* What PRRTE currently reports, so a verify can say what it found and not just
 * what it wanted. Returns 0 if the query fails. */
static size_t current_total_nodes(void)
{
    alloc_snapshot_t snap;
    size_t total = 0;

    queries_quiet = 1;
    if (0 == query_snapshot(&snap)) {
        total = snapshot_total_nodes(&snap);
        snapshot_free(&snap);
    }
    queries_quiet = 0;

    return total;
}
static int wait_until_nodes_absent(char **nodes);
static int squeue_job_exists(const char *jobid);

static void watch_finish(watch_t *ctx, pmix_status_t rc)
{
    if (!status_is_accepted(rc)) {
        record_message("VERIFY %s: request %s (%s)\n", ctx->description,
                       outcome_phrase(rc), PMIx_Error_string(rc));
    } else {
        switch (ctx->verify) {
        case VERIFY_NODES_AT_LEAST: {
            char allocid[64];

            /* Check the allocation this request was granted, not the DVM
             * total. A total is a snapshot taken before submitting, and
             * anything else that resizes the DVM meanwhile invalidates it -
             * concurrent requests are the point of this program. */
            pending_allocid(ctx->slot, allocid, sizeof(allocid));
            if ('\0' != allocid[0]) {
                ssize_t got = wait_for_alloc_nodes(allocid);

                if (0 > got) {
                    record_message("VERIFY %s: WARN (PRRTE has no allocation "
                                   "%s)\n", ctx->description, allocid);
                } else {
                    record_message("VERIFY %s: %s (allocation %s holds %zd "
                                   "nodes, asked for %zu)\n", ctx->description,
                                   (size_t) got >= ctx->requested
                                       ? "PASS" : "WARN",
                                   allocid, got, ctx->requested);
                }
                break;
            }
            /* No event named an allocation, so the total is all there is. */
            record_message("VERIFY %s: %s (PRRTE knows %zu nodes, expected at "
                           "least %zu)\n", ctx->description,
                           0 == wait_until_alloc_count_at_least(ctx->target)
                               ? "PASS" : "WARN",
                           current_total_nodes(), ctx->target);
            break;
        }
        case VERIFY_NODES_AT_MOST:
            if (0 == wait_until_total_nodes_at_most(ctx->target)) {
                record_message("VERIFY %s: PASS (PRRTE knows %zu nodes, "
                               "expected at most %zu)\n", ctx->description,
                               current_total_nodes(), ctx->target);
            } else {
                record_message("VERIFY %s: WARN (PRRTE knows %zu nodes, "
                               "expected at most %zu)\n", ctx->description,
                               current_total_nodes(), ctx->target);
            }
            break;
        case VERIFY_ALLOC_GONE: {
            int gone = (0 == wait_until_alloc_removed(ctx->subject));

            record_message("VERIFY %s: %s (PRRTE allocation %s %s)\n",
                           ctx->description, gone ? "PASS" : "WARN",
                           ctx->subject, gone ? "gone" : "still present");
            if (0 == ctx->was_in_squeue) {
                record_message("VERIFY %s: INFO job %s was not visible in "
                               "squeue before the request\n", ctx->description,
                               ctx->subject);
            } else {
                record_message("VERIFY %s: %s (squeue job %s)\n",
                               ctx->description,
                               squeue_job_exists(ctx->subject) ? "WARN" : "PASS",
                               ctx->subject);
            }
            break;
        }
        case VERIFY_NODES_ABSENT: {
            char **nodes = PMIx_Argv_split(ctx->subject, ',');
            int gone = (NULL != nodes && 0 == wait_until_nodes_absent(nodes));

            record_message("VERIFY %s: %s (requested nodes %s in PRRTE)\n",
                           ctx->description, gone ? "PASS" : "WARN",
                           gone ? "absent" : "still present");
            if (NULL != nodes) {
                PMIx_Argv_free(nodes);
            }
            break;
        }
        default:
            record_message("VERIFY %s: PASS (%s)\n", ctx->description,
                           PMIx_Error_string(rc));
            break;
        }
    }

    show_state();

    PMIX_INFO_FREE(ctx->info, ctx->ninfo);
    pending_drop(ctx->slot);
    free(ctx);
}

static void *request_watcher(void *arg)
{
    watch_t *ctx = (watch_t *) arg;
    pmix_status_t rc;

    if (0 != wait_for_callback(ctx->w, REQUEST_WATCH_TIMEOUT)) {
        record_message("\n[%s] no answer within %d seconds; giving up on it\n",
                       ctx->reqid, REQUEST_WATCH_TIMEOUT);
        wait_release(ctx->w);
        watch_finish(ctx, PMIX_ERR_TIMEOUT);
        return NULL;
    }

    rc = ctx->w->status;
    wait_release(ctx->w);

    if (PMIX_OPERATION_IN_PROGRESS == rc) {
        /* Phase one says only that the scheduler acted. The DVM has not
         * resized yet, so the nodes named by the allocation cannot carry work
         * - and the request is now past cancelling: the resources are held,
         * and giving them back is a shrink, not a cancel. */
        pending_mark_granted(ctx->slot);
        record_message("\n[%s] scheduler has acted; waiting for the DVM to "
                       "resize\n", ctx->reqid);

        if (0 <= ctx->slot && dvm_events_available) {
            rc = pending_wait(ctx->slot, DVM_READY_TIMEOUT);
            if (PMIX_ERR_TIMEOUT == rc) {
                record_message("[%s] no DVM completion event within %d "
                               "seconds\n", ctx->reqid, DVM_READY_TIMEOUT);
            }
        } else {
            record_message("[%s] no completion event can be matched to this "
                           "request; treating the grant as the answer\n",
                           ctx->reqid);
        }
    }

    watch_finish(ctx, rc);
    return NULL;
}

/* Takes ownership of info either way, so the caller must not free it. */
static void submit_watched(pmix_alloc_directive_t directive, pmix_info_t *info,
                           size_t ninfo, const char *description,
                           const char *reqid, verify_kind_t verify,
                           size_t target, size_t requested, const char *subject,
                           int was_in_squeue, int cancellable)
{
    watch_t *ctx;
    pthread_t tid;
    pmix_status_t rc;

    ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    ctx->info = info;
    ctx->ninfo = ninfo;
    ctx->verify = verify;
    ctx->target = target;
    ctx->requested = requested;
    ctx->was_in_squeue = was_in_squeue;
    snprintf(ctx->reqid, sizeof(ctx->reqid), "%s", reqid);
    snprintf(ctx->description, sizeof(ctx->description), "%s", description);
    snprintf(ctx->subject, sizeof(ctx->subject), "%s",
             NULL == subject ? "" : subject);

    ctx->w = wait_new();
    if (NULL == ctx->w) {
        PMIX_INFO_FREE(info, ninfo);
        free(ctx);
        return;
    }

    /* Claim the correlation slot before submitting. Nothing on this side
     * orders the phase-two event after the phase-one callback. */
    ctx->slot = pending_register(reqid, description, cancellable);

    if (cancellable) {
        record_message("\nSubmitting %s as request id %s\n"
                       "  while the scheduler still has it queued, withdraw "
                       "it with:  cancel %s\n",
                       description, reqid, reqid);
    } else {
        record_message("\nSubmitting %s as request id %s\n",
                       description, reqid);
    }

    rc = PMIx_Allocation_request_nb(directive, info, ninfo, alloc_cbfunc,
                                    ctx->w);
    if (PMIX_SUCCESS != rc) {
        record_message("PMIx_Allocation_request_nb failed immediately: %s\n",
                       PMIx_Error_string(rc));
        pending_drop(ctx->slot);
        /* The callback will not run, so drop its reference here too. */
        wait_release(ctx->w);
        wait_release(ctx->w);
        PMIX_INFO_FREE(info, ninfo);
        free(ctx);
        return;
    }

    if (0 != pthread_create(&tid, NULL, request_watcher, ctx)) {
        record_message("could not start a watcher thread; waiting inline "
                       "(the prompt will not return until this finishes)\n");
        request_watcher(ctx);
        return;
    }
    pthread_detach(tid);
}

/* A name has to be short enough to type before the scheduler grants. A
 * caller-supplied one is used verbatim; the generated form counts requests. */
static void make_reqid(char *buf, size_t size, const char *prefix,
                       const char *name)
{
    static unsigned int next_id = 1;

    if (NULL != name && '\0' != name[0]) {
        snprintf(buf, size, "%s", name);
        return;
    }
    snprintf(buf, size, "%s-%u", prefix, next_id++);
}

static int wait_until_alloc_count_at_least(size_t min_count)
{
    queries_quiet = 1;
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int ok = 0;

        if (0 == query_snapshot(&snap)) {
            ok = snapshot_total_nodes(&snap) >= min_count;
            snapshot_free(&snap);
        }
        if (ok) {
            queries_quiet = 0;
            return 0;
        }
        sleep(1);
    }
    queries_quiet = 0;
    return -1;
}

static int wait_until_alloc_removed(const char *allocid)
{
    queries_quiet = 1;
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int gone = 0;

        if (0 == query_snapshot(&snap)) {
            gone = NULL == snapshot_find_alloc(&snap, allocid);
            snapshot_free(&snap);
        }
        if (gone) {
            queries_quiet = 0;
            return 0;
        }
        sleep(1);
    }
    queries_quiet = 0;
    return -1;
}

static int wait_until_nodes_absent(char **nodes)
{
    queries_quiet = 1;
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int absent = 1;

        if (0 == query_snapshot(&snap)) {
            for (int n = 0; NULL != nodes[n]; n++) {
                if (snapshot_has_node(&snap, nodes[n])) {
                    absent = 0;
                    break;
                }
            }
            snapshot_free(&snap);
        } else {
            absent = 0;
        }
        if (absent) {
            queries_quiet = 0;
            return 0;
        }
        sleep(1);
    }
    queries_quiet = 0;
    return -1;
}

static int wait_until_total_nodes_at_most(size_t max_count)
{
    queries_quiet = 1;
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int ok = 0;

        if (0 == query_snapshot(&snap)) {
            ok = snapshot_total_nodes(&snap) <= max_count;
            snapshot_free(&snap);
        }
        if (ok) {
            queries_quiet = 0;
            return 0;
        }
        sleep(1);
    }
    queries_quiet = 0;
    return -1;
}

static void handle_extend_count(uint64_t count, const char *name)
{
    alloc_snapshot_t before;
    size_t before_nodes = 0;
    pmix_info_t *info;
    char reqid[REQID_LEN];

    if (0 == query_snapshot(&before)) {
        before_nodes = snapshot_total_nodes(&before);
        snapshot_free(&before);
    }

    make_reqid(reqid, sizeof(reqid), "extend", name);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NUM_NODES, &count, PMIX_UINT64);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    submit_watched(PMIX_ALLOC_EXTEND, info, 2, "extend count", reqid,
                   VERIFY_NODES_AT_LEAST, before_nodes + (size_t) count,
                   (size_t) count, NULL, 0, 1);
}

static void handle_shrink_count(uint64_t count)
{
    alloc_snapshot_t before;
    size_t before_nodes = 0;
    pmix_info_t *info;
    char reqid[REQID_LEN];

    if (0 == query_snapshot(&before)) {
        before_nodes = snapshot_total_nodes(&before);
        snapshot_free(&before);
    }

    make_reqid(reqid, sizeof(reqid), "shrink", NULL);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NUM_NODES, &count, PMIX_UINT64);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    submit_watched(PMIX_ALLOC_RELEASE, info, 2, "shrink count", reqid,
                   VERIFY_NODES_AT_MOST,
                   count >= before_nodes ? 0 : before_nodes - count,
                   (size_t) count, NULL, 0, 0);
}

static void handle_shrink_alloc(const char *allocid)
{
    pmix_info_t *info;
    int before_squeue;
    char reqid[REQID_LEN];

    before_squeue = squeue_job_exists(allocid);
    make_reqid(reqid, sizeof(reqid), "shrink", NULL);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_ID, allocid, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    submit_watched(PMIX_ALLOC_RELEASE, info, 2, "shrink alloc", reqid,
                   VERIFY_ALLOC_GONE, 0, 0, allocid, before_squeue, 0);
}

static void handle_shrink_list(const char *list)
{
    pmix_info_t *info;
    char reqid[REQID_LEN];

    make_reqid(reqid, sizeof(reqid), "shrink", NULL);
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NODE_LIST, list, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    submit_watched(PMIX_ALLOC_RELEASE, info, 2, "shrink list", reqid,
                   VERIFY_NODES_ABSENT, 0, 0, list, 0, 0);
}

/* Withdraw an extend the scheduler has not yet granted. After the grant the
 * resources are held and handing them back is a shrink. A shrink is never
 * cancellable, so refuse locally rather than send a request that can only come
 * back PMIX_ERR_NOT_FOUND. */
static void handle_cancel(const char *reqid)
{
    pmix_info_t *info;
    pmix_status_t rc;
    char description[256];
    int known;

    if (!pending_is_cancellable(reqid, &known) && known) {
        record_message("VERIFY cancel: %s is not cancellable; only an extend "
                       "the scheduler has not yet granted can be withdrawn\n",
                       reqid);
        return;
    }

    snprintf(description, sizeof(description), "cancel of %s", reqid);
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    rc = submit_and_wait(PMIX_ALLOC_REQ_CANCEL, info, 1, description);
    PMIX_INFO_FREE(info, 1);

    if (status_is_accepted(rc)) {
        /* No completion event can follow a request that no longer exists. The
         * cancelled request is answered separately, on its own callback. */
        pending_forget_reqid(reqid);
        record_message("VERIFY cancel: PASS (request %s withdrawn)\n", reqid);
    } else if (PMIX_ERR_NOT_FOUND == rc) {
        record_message("VERIFY cancel: no pending request %s. It was never "
                       "submitted, or the scheduler has already granted it - "
                       "a granted request can only be given back with a "
                       "shrink.\n", reqid);
    } else {
        record_message("VERIFY cancel: request was rejected with %s\n",
                       PMIx_Error_string(rc));
    }
}

static void usage(const char *argv0)
{
    printf("Usage: %s\n\n", argv0);
    printf("Run inside a Slurm job and under PRRTE/PMIx, then enter commands:\n");
    printf("  extend count N [as REQUEST_NAME]\n");
    printf("  shrink count N\n");
    printf("  shrink alloc SLURM_JOB_ID\n");
    printf("  shrink list NODE1,NODE2\n");
    printf("  cancel [REQUEST_NAME]\n");
    printf("  pending\n");
    printf("  prun [ARGS...]\n");
    printf("  dvm\n");
    printf("  refresh\n");
    printf("  quit\n");
}

/* Consume a trailing "as NAME", continuing the caller's strtok walk. */
static const char *parse_as_name(void)
{
    char *keyword = strtok(NULL, " \t");
    char *name;

    if (NULL == keyword || 0 != strcmp(keyword, "as")) {
        return NULL;
    }
    name = strtok(NULL, " \t");

    return (NULL != name && '\0' != name[0]) ? name : NULL;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (NULL == s || '\0' == s[0]) {
        return -1;
    }
    errno = 0;
    value = strtoull(s, &end, 10);
    if (0 != errno || NULL == end || '\0' != *end) {
        return -1;
    }
    *out = (uint64_t) value;
    return 0;
}

/* Elastic mode cannot be checked: it has no PMIx query, and PRRTE strips
 * PRTE_* from a launched process's environment. */
static void note_elastic_mode_requirement(void)
{
    fprintf(stderr,
            "NOTE: remember to toggle prte_elastic_mode, for example by\n"
            "exporting PRTE_MCA_prte_elastic_mode=1, if you didn't already.\n"
            "\n");
}

/* If PRRTE does not know this job, the DVM never discovered the allocation.
 * Checked once: the allocation set changes on purpose after startup. */
static int check_jobid_known(const char *slurm_jobid)
{
    alloc_snapshot_t snap;
    int known;

    if (0 != query_snapshot(&snap)) {
        fprintf(stderr, "Could not query PRRTE for its allocations; the DVM is "
                        "not answering.\n");
        return -1;
    }

    known = (NULL != snapshot_find_alloc(&snap, slurm_jobid));
    if (!known) {
        fprintf(stderr,
                "PRRTE does not know Slurm job %s. The DVM did not discover "
                "this\nallocation, so it cannot grow or shrink it.\n",
                slurm_jobid);
        if (0 == snap.nallocs) {
            fprintf(stderr, "PRRTE reports no allocations at all.\n");
        } else {
            fprintf(stderr, "PRRTE reports these instead:");
            for (size_t i = 0; i < snap.nallocs; i++) {
                fprintf(stderr, " %s", snap.allocs[i].id);
            }
            fprintf(stderr, "\n");
        }
    }
    snapshot_free(&snap);

    return known ? 0 : -1;
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    char line[LINE_MAX_LEN];
    const char *slurm_jobid;

    if (1 < argc && 0 == strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }

    slurm_jobid = getenv("SLURM_JOB_ID");
    if (NULL == slurm_jobid || '\0' == slurm_jobid[0]) {
        fprintf(stderr, "SLURM_JOB_ID is unset: this program must run inside "
                        "a Slurm job.\n");
        return 1;
    }

    note_elastic_mode_requirement();

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    printf("PMIx client %s:%u inside Slurm job %s\n",
           myproc.nspace, myproc.rank, slurm_jobid);
    register_dvm_mod_handler();

    if (0 != check_jobid_known(slurm_jobid)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    usage(argv[0]);
    show_state();

    while (1) {
        char *cmd;
        char *input;
        char *type;
        char *arg;

        printf("\ncoord> ");
        fflush(stdout);
        if (NULL == fgets(line, sizeof(line), stdin)) {
            break;
        }
        line[strcspn(line, "\n")] = '\0';

        input = line;
        while (isspace((unsigned char) *input)) {
            input++;
        }
        if (0 == strncmp(input, "prun", 4) &&
            ('\0' == input[4] || isspace((unsigned char) input[4]))) {
            run_prun_command(input);
            continue;
        }

        cmd = strtok(input, " \t");
        if (NULL == cmd) {
            continue;
        }

        if (0 == strcmp(cmd, "quit") || 0 == strcmp(cmd, "exit")) {
            break;
        }
        if (0 == strcmp(cmd, "refresh")) {
            show_state();
            continue;
        }
        if (0 == strcmp(cmd, "dvm")) {
            handle_dvm();
            continue;
        }
        if (0 == strcmp(cmd, "pending")) {
            pending_report();
            continue;
        }
        if (0 == strcmp(cmd, "cancel")) {
            char sole[REQID_LEN];

            arg = strtok(NULL, " \t");
            if (NULL == arg) {
                /* Typing the name is the hard part of cancelling in time. */
                if (0 != pending_sole_cancellable(sole, sizeof(sole))) {
                    printf("usage: cancel REQUEST_NAME   ('pending' lists them; "
                           "the bare form works only when exactly one request "
                           "is still cancellable)\n");
                    continue;
                }
                arg = sole;
                printf("cancelling the only cancellable request: %s\n", arg);
            }
            handle_cancel(arg);
            show_state();
            continue;
        }
        if (0 == strcmp(cmd, "extend")) {
            uint64_t count;

            type = strtok(NULL, " \t");
            arg = strtok(NULL, " \t");
            if (NULL == type || NULL == arg || 0 != strcmp(type, "count") ||
                0 != parse_u64(arg, &count)) {
                printf("usage: extend count N [as REQUEST_NAME]\n");
                continue;
            }
            handle_extend_count(count, parse_as_name());
            continue;
        }
        if (0 == strcmp(cmd, "shrink")) {
            type = strtok(NULL, " \t");
            arg = strtok(NULL, " \t");
            if (NULL == type || NULL == arg) {
                printf("usage: shrink count N | shrink alloc ID | shrink list NODE1,NODE2\n");
                continue;
            }
            if (0 == strcmp(type, "count")) {
                uint64_t count;

                if (0 != parse_u64(arg, &count)) {
                    printf("usage: shrink count N\n");
                    continue;
                }
                handle_shrink_count(count);
            } else if (0 == strcmp(type, "alloc")) {
                handle_shrink_alloc(arg);
            } else if (0 == strcmp(type, "list")) {
                handle_shrink_list(arg);
            } else {
                printf("usage: shrink count N | shrink alloc ID | shrink list NODE1,NODE2\n");
                continue;
            }
            continue;
        }

        printf("unknown command: %s\n", cmd);
        usage(argv[0]);
    }

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Finalize failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    return 0;
}
