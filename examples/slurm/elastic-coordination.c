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
#define PRUN_TIMEOUT 10
#define LINE_MAX_LEN 1024

static void record_message(const char *fmt, ...);
static int line_seen(char **lines, const char *line);

static void trace_point(const char *msg)
{
    printf("[tester] %s\n", msg);
    fflush(stdout);
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int done;
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

static void wait_init(wait_t *w)
{
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->done = 0;
    w->status = PMIX_SUCCESS;
}

static void wait_destroy(wait_t *w)
{
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
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
    return 0;
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

static void record_message(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
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

    printf("[tester] query begin: %s%s%s\n", key,
           NULL == allocid ? "" : " alloc=", NULL == allocid ? "" : allocid);
    fflush(stdout);
    rc = PMIx_Query_info(query, 1, results, nresults);
    printf("[tester] query end: %s -> %s (%zu result%s)\n", key,
           PMIx_Error_string(rc), NULL == nresults ? 0 : *nresults,
           NULL != nresults && 1 == *nresults ? "" : "s");
    fflush(stdout);
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
        trace_point("show_state: running reachability probe");
        run_prun_hostname(&snap);
        snapshot_free(&snap);
    } else {
        printf("\nPRRTE allocation query failed\n");
    }
    trace_point("show_state: end");
}

static pmix_status_t submit_alloc_request(pmix_alloc_directive_t directive,
                                          pmix_info_t *info, size_t ninfo,
                                          const char *description)
{
    pmix_status_t rc;
    wait_t w;

    wait_init(&w);
    record_message("\nSubmitting %s\n", description);
    trace_point("allocation request: submit");
    rc = PMIx_Allocation_request_nb(directive, info, ninfo, alloc_cbfunc, &w);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Allocation_request_nb failed immediately: %s\n",
                PMIx_Error_string(rc));
        wait_destroy(&w);
        return rc;
    }

    trace_point("allocation request: waiting for callback");
    if (0 != wait_for_callback(&w, REQUEST_TIMEOUT)) {
        fprintf(stderr, "timed out waiting for PMIx allocation callback\n");
        wait_destroy(&w);
        return PMIX_ERR_TIMEOUT;
    }
    trace_point("allocation request: callback received");

    rc = w.status;
    wait_destroy(&w);
    return rc;
}

static void make_reqid(char *buf, size_t size, const char *prefix)
{
    static unsigned int next_id = 1;

    snprintf(buf, size, "%s-%ld-%u", prefix, (long) getpid(), next_id++);
}

static int wait_until_alloc_count_at_least(size_t min_count)
{
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int ok = 0;

        if (0 == query_snapshot(&snap)) {
            ok = snapshot_total_nodes(&snap) >= min_count;
            snapshot_free(&snap);
        }
        if (ok) {
            return 0;
        }
        sleep(1);
    }
    return -1;
}

static int wait_until_alloc_removed(const char *allocid)
{
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int gone = 0;

        if (0 == query_snapshot(&snap)) {
            gone = NULL == snapshot_find_alloc(&snap, allocid);
            snapshot_free(&snap);
        }
        if (gone) {
            return 0;
        }
        sleep(1);
    }
    return -1;
}

static int wait_until_nodes_absent(char **nodes)
{
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
            return 0;
        }
        sleep(1);
    }
    return -1;
}

static int wait_until_total_nodes_at_most(size_t max_count)
{
    for (int i = 0; i < VERIFY_TIMEOUT; i++) {
        alloc_snapshot_t snap;
        int ok = 0;

        if (0 == query_snapshot(&snap)) {
            ok = snapshot_total_nodes(&snap) <= max_count;
            snapshot_free(&snap);
        }
        if (ok) {
            return 0;
        }
        sleep(1);
    }
    return -1;
}

static void handle_extend_count(uint64_t count)
{
    alloc_snapshot_t before;
    size_t before_nodes = 0;
    pmix_info_t *info;
    pmix_status_t rc;
    char reqid[128];

    if (0 == query_snapshot(&before)) {
        before_nodes = snapshot_total_nodes(&before);
        snapshot_free(&before);
    }

    make_reqid(reqid, sizeof(reqid), "coord-extend");
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NUM_NODES, &count, PMIX_UINT64);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    rc = submit_alloc_request(PMIX_ALLOC_EXTEND, info, 2, "extend count");
    PMIX_INFO_FREE(info, 2);

    if (status_is_accepted(rc)) {
        size_t expected = before_nodes + (size_t) count;

        if (0 == wait_until_alloc_count_at_least(expected)) {
            record_message("VERIFY extend count: PASS (PRRTE knows at least %zu nodes)\n",
                           expected);
        } else {
            record_message("VERIFY extend count: WARN expected at least %zu PRRTE nodes\n",
                           expected);
        }
    } else {
        record_message("VERIFY extend count: request was rejected with %s\n",
                       PMIx_Error_string(rc));
    }
}

static void handle_shrink_count(uint64_t count)
{
    alloc_snapshot_t before;
    size_t before_nodes = 0;
    pmix_info_t *info;
    pmix_status_t rc;
    char reqid[128];

    if (0 == query_snapshot(&before)) {
        before_nodes = snapshot_total_nodes(&before);
        snapshot_free(&before);
    }

    make_reqid(reqid, sizeof(reqid), "coord-shrink-count");
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NUM_NODES, &count, PMIX_UINT64);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    rc = submit_alloc_request(PMIX_ALLOC_RELEASE, info, 2, "shrink count");
    PMIX_INFO_FREE(info, 2);

    if (status_is_accepted(rc)) {
        size_t expected_max = count >= before_nodes ? 0 : before_nodes - count;

        if (0 == wait_until_total_nodes_at_most(expected_max)) {
            record_message("VERIFY shrink count: PASS (PRRTE total nodes <= %zu)\n",
                           expected_max);
        } else {
            record_message("VERIFY shrink count: WARN expected PRRTE total nodes <= %zu\n",
                           expected_max);
        }
    } else {
        record_message("VERIFY shrink count: request was rejected with %s\n",
                       PMIx_Error_string(rc));
    }
}

static void handle_shrink_alloc(const char *allocid)
{
    pmix_info_t *info;
    pmix_status_t rc;
    int before_squeue;
    char reqid[128];

    before_squeue = squeue_job_exists(allocid);
    make_reqid(reqid, sizeof(reqid), "coord-shrink-alloc");
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_ID, allocid, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    rc = submit_alloc_request(PMIX_ALLOC_RELEASE, info, 2, "shrink alloc");
    PMIX_INFO_FREE(info, 2);

    if (status_is_accepted(rc)) {
        int after_squeue;

        if (0 == wait_until_alloc_removed(allocid)) {
            record_message("VERIFY shrink alloc: PASS (PRRTE allocation %s gone)\n",
                           allocid);
        } else {
            record_message("VERIFY shrink alloc: WARN PRRTE allocation %s still visible\n",
                           allocid);
        }

        after_squeue = squeue_job_exists(allocid);
        if (1 == before_squeue && 0 == after_squeue) {
            record_message("VERIFY shrink alloc: PASS (squeue job %s disappeared)\n",
                           allocid);
        } else if (0 == before_squeue) {
            record_message("VERIFY shrink alloc: INFO job %s was not visible in squeue before request\n",
                           allocid);
        } else {
            record_message("VERIFY shrink alloc: WARN job %s still visible in squeue\n",
                           allocid);
        }
    } else {
        record_message("VERIFY shrink alloc: request was rejected with %s\n",
                       PMIx_Error_string(rc));
    }
}

static void handle_shrink_list(const char *list)
{
    pmix_info_t *info;
    pmix_status_t rc;
    char **nodes;
    char reqid[128];

    nodes = PMIx_Argv_split(list, ',');
    make_reqid(reqid, sizeof(reqid), "coord-shrink-list");
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NODE_LIST, list, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_REQ_ID, reqid, PMIX_STRING);

    rc = submit_alloc_request(PMIX_ALLOC_RELEASE, info, 2, "shrink list");
    PMIX_INFO_FREE(info, 2);

    if (status_is_accepted(rc)) {
        if (NULL != nodes && 0 == wait_until_nodes_absent(nodes)) {
            record_message("VERIFY shrink list: PASS (requested nodes absent from PRRTE)\n");
        } else {
            record_message("VERIFY shrink list: WARN requested nodes still visible in PRRTE\n");
        }
    } else {
        record_message("VERIFY shrink list: request was rejected with %s\n",
                       PMIx_Error_string(rc));
    }

    if (NULL != nodes) {
        PMIx_Argv_free(nodes);
    }
}

static void usage(const char *argv0)
{
    printf("Usage: %s\n\n", argv0);
    printf("Run inside a Slurm job and under PRRTE/PMIx, then enter commands:\n");
    printf("  extend count N\n");
    printf("  shrink count N\n");
    printf("  shrink alloc SLURM_JOB_ID\n");
    printf("  shrink list NODE1,NODE2\n");
    printf("  prun [ARGS...]\n");
    printf("  refresh\n");
    printf("  quit\n");
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
        fprintf(stderr, "This program must run inside a Slurm job (SLURM_JOB_ID is unset).\n");
        return 1;
    }

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    printf("PMIx client %s:%u inside Slurm job %s\n",
           myproc.nspace, myproc.rank, slurm_jobid);
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
        if (0 == strcmp(cmd, "extend")) {
            uint64_t count;

            type = strtok(NULL, " \t");
            arg = strtok(NULL, " \t");
            if (NULL == type || NULL == arg || 0 != strcmp(type, "count") ||
                0 != parse_u64(arg, &count)) {
                printf("usage: extend count N\n");
                continue;
            }
            handle_extend_count(count);
            show_state();
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
            show_state();
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
