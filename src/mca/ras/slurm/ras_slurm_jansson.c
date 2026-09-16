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

/* This file contains logic which relies on the Jansson
 * library to interpet Slurm JSON output */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>

#if PRTE_TESTBUILD_LAUNCHERS
#    include "src/mca/ras/base/testbuild_jansson.h"
#else
#    include <jansson.h>
#endif

#include "src/mca/errmgr/errmgr.h"
#include "src/util/pmix_output.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/prte_json_window.h"

#include "ras_slurm.h"
#include "src/mca/common/slurm/common_slurm.h"

#define PRTE_SLURM_JOB_INFO_MAX_SIZE (1 * 1024 * 1024)
/* The largest value the reader ever parses is one node's socket and core
 * map, and a node past PRTE_SLURM_MAX_CORE_COUNT cores is refused before
 * this limit is reached: Slurm spends about 140 bytes per core, so that
 * ceiling is around 600KB. */
#define PRTE_SLURM_JSON_WINDOW_SIZE (1024 * 1024)
#define PRTE_SLURM_MAX_THREADS_PER_CORE 32
#define PRTE_SLURM_MAX_CORE_COUNT 4096

/*
 * Local functions
 */
static int prte_ras_slurm_get_json_numobj_field(json_t *job, const char *key, pmix_hash_table_t *values_table);
static int prte_ras_slurm_get_json_numobj_value(json_t *job, const char *key, int64_t *out);
static int prte_ras_slurm_get_jobinfo_json(const char *slurm_jobid, json_t **job_info_out);
static size_t prte_ras_slurm_jansson_cbfunc(void *buffer, size_t buflen, void *data);

/* Bounded reader for Slurm JSON output */
typedef struct {
    FILE *fp;
    size_t remaining;
    bool truncated;
    bool io_error;
} jansson_limited_reader_t;

/*
 * Parse a numeric-object field from JSON and store it as a string in a hash table.
 *
 * If the field is absent or null, returns PRTE_ERR_NOT_FOUND so optional job
 * attributes can be omitted. Otherwise, expects the JSON object at key to have
 * "set", "infinite", and "number" fields. Stores the result
 * in values_table:
 * - unset -> string determined by PRTE_SLURM_UNSET_NUM_MARKER
 * - infinite -> string determined by PRTE_SLURM_INFINITE_NUM_MARKER
 * - otherwise -> numeric value as string
 *
 * @param[in]  job           JSON job object.
 * @param[in]  key           Field name to extract.
 * @param[out] values_table  Destination hash table.
 */
static int prte_ras_slurm_get_json_numobj_field(json_t *job, const char *key, pmix_hash_table_t *values_table)
{
    if (NULL == job || NULL == key || NULL == values_table) {
        return PRTE_ERR_BAD_PARAM;
    }

    int pmix_err = PMIX_SUCCESS;

    json_t *field = json_object_get(job, key);
    if (NULL == field || json_is_null(field)) {
        return PRTE_ERR_NOT_FOUND;
    }
    if (!json_is_object(field)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    json_t *set_flag = json_object_get(field, num_obj_subfields[NUM_OBJ_SUBFIELD_SET]);
    if (NULL == set_flag || !json_is_boolean(set_flag)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    if (!json_is_true(set_flag)) {
        char *unset_dyn = strdup(PRTE_SLURM_UNSET_NUM_MARKER);
        if (NULL == unset_dyn) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        pmix_err = pmix_hash_table_set_value_ptr(values_table, key, strlen(key), unset_dyn);
        if (PMIX_SUCCESS != pmix_err) {
            free(unset_dyn);
            return prte_pmix_convert_status(pmix_err);
        }
        return PRTE_SUCCESS;
    }

    json_t *inf_flag = json_object_get(field, num_obj_subfields[NUM_OBJ_SUBFIELD_INFINITE]);
    if (NULL == inf_flag || !json_is_boolean(inf_flag)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    if (json_is_true(inf_flag)) {
        char *inf_dyn = strdup(PRTE_SLURM_INFINITE_NUM_MARKER);
        if (NULL == inf_dyn) return PRTE_ERR_OUT_OF_RESOURCE;

        pmix_err = pmix_hash_table_set_value_ptr(values_table, key, strlen(key), inf_dyn);
        if (PMIX_SUCCESS != pmix_err) {
            free(inf_dyn);
            return prte_pmix_convert_status(pmix_err);
        }
        return PRTE_SUCCESS;
    }

    json_t *num_field = json_object_get(field, num_obj_subfields[NUM_OBJ_SUBFIELD_NUMBER]);
    if (NULL == num_field || !json_is_integer(num_field)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    json_int_t num = json_integer_value(num_field);
    if (num < 0) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    char *num_dyn = NULL;
    if (-1 == asprintf(&num_dyn, "%" JSON_INTEGER_FORMAT, num)) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    pmix_err = pmix_hash_table_set_value_ptr(values_table, key, strlen(key), num_dyn);
    if (PMIX_SUCCESS != pmix_err) {
        free(num_dyn);
        return prte_pmix_convert_status(pmix_err);
    }

    return PRTE_SUCCESS;
}

/*
 * Jansson input callback with read size limiting.
 *
 * Reads data from a FILE stream into the provided buffer, enforcing a
 * maximum total number of bytes that can be consumed. If the limit is
 * reached, the reader is marked as truncated and no further data is read.
 * Intended for use with json_load_callback().
 *
 * @param[out] buffer
 *     Destination buffer for read data.
 * @param[in] buflen
 *     Maximum number of bytes to read into the buffer.
 * @param[in,out] data
 *     Pointer to a jansson_limited_reader_t structure containing the FILE
 *     stream, remaining byte budget, and truncation and error flags.
 *
 * @return Number of bytes read into buffer. Returns 0 when no more data
 *         should be read (EOF or limit reached).
 */
static size_t prte_ras_slurm_jansson_cbfunc(void *buffer, size_t buflen, void *data)
{
    jansson_limited_reader_t *reader = data;

    if (reader->remaining == 0) {
        reader->truncated = 1;
        return 0;
    }

    if (buflen > reader->remaining) {
        buflen = reader->remaining;
    }

    size_t len = fread(buffer, 1, buflen, reader->fp);

    if (0 == len && ferror(reader->fp)) {
        reader->io_error = true;
    }

    reader->remaining -= len;
    return len;
}

/*
 * Query Slurm job information and return the job object as Jansson JSON.
 *
 * Executes `scontrol show job <jobid> --json`, parses the resulting JSON,
 * and returns the single job object contained in the response. On success,
 * the returned JSON object is referenced for the caller, who becomes responsible
 * for releasing it with json_decref().
 *
 * @param[in] slurm_jobid
 *     SLURM job ID to query.
 * @param[out] job_info_out
 *     Output pointer receiving the parsed JSON object for the job. Set to
 *     NULL on entry and on failure.
 */
static int prte_ras_slurm_get_jobinfo_json(const char *slurm_jobid, json_t **job_info_out)
{
    if(NULL == slurm_jobid || NULL == job_info_out) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    *job_info_out = NULL;

    int err = PRTE_SUCCESS;

    /* Make sure the job ID given is within constraints */
    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    static const char *cmd_format = "scontrol show job %s --json";

    json_error_t json_err;

    json_t *parent_json = NULL;

    FILE *fp = NULL;

    char *cmd = NULL;

    if(0 > asprintf(&cmd, cmd_format, slurm_jobid)) {
        cmd = NULL;
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    fp = popen(cmd, "r");

    if(NULL == fp) {
        err = PRTE_ERR_FILE_OPEN_FAILURE;
        goto cleanup;
    }

    jansson_limited_reader_t lr = {
        .fp = fp,
        .remaining = PRTE_SLURM_JOB_INFO_MAX_SIZE,
        .truncated = false,
        .io_error = false
    };

    parent_json = json_load_callback(
        prte_ras_slurm_jansson_cbfunc,
        &lr,
        JSON_REJECT_DUPLICATES,
        &json_err
    );

    int status = pclose(fp);
    fp = NULL;

    if (-1 == status) {
        pmix_output(0, "ras:slurm:get_jobinfo_json: pclose failed: %s.", strerror(errno));
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:get_jobinfo_json: non-zero exit code (%d) from scontrol command.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
            WIFEXITED(status) ? WEXITSTATUS(status) : -1));
        err = PRTE_ERR_SLURM_QUERY_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if(!parent_json) {

        if(lr.io_error) {
            err = PRTE_ERR_FILE_READ_FAILURE;
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:get_jobinfo_json: error reading from stream.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        } else if(lr.truncated) {
            err = PRTE_ERR_MEM_LIMIT_EXCEEDED;
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:get_jobinfo_json: job info JSON was truncated.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        } else {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:get_jobinfo_json: job info JSON parse failed.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        }

        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    /* Jobs array: we expect and require exactly one job in the result  */
    json_t *jobs_arr = json_object_get(parent_json, "jobs");
    if (NULL == jobs_arr || !json_is_array(jobs_arr) || 1 != json_array_size(jobs_arr)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_t *job = json_array_get(jobs_arr, 0);
    if (NULL == job || !json_is_object(job)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    /* Ensure job information is not destroyed */
    json_incref(job);

    *job_info_out = job;

    cleanup:

    if(NULL != fp) {
        pclose(fp);
    }

    if(NULL != parent_json) {
        json_decref(parent_json);
    }

    free(cmd);

    return err;
}

/*
 * A job record read through a fixed window.
 *
 * Slurm prints every socket and every core of every allocated node, so the
 * record grows with the total core count of the job's nodes and reaches
 * hundreds of megabytes on a large one. Parsing it whole costs several times
 * that again as a jansson DOM. prte_json_window walks it instead, and jansson
 * only ever sees the bytes of a member this file asks for.
 */
typedef struct {
    FILE *fp;
    bool io_error;
} prte_ras_slurm_json_src_t;

static size_t prte_ras_slurm_json_read(char *dest, size_t len, void *cbdata)
{
    prte_ras_slurm_json_src_t *src = (prte_ras_slurm_json_src_t *) cbdata;
    size_t got = fread(dest, 1, len, src->fp);

    if (got < len && 0 != ferror(src->fp)) {
        src->io_error = true;
    }

    return got;
}

/*
 * Parse the value the window is at.
 *
 * JSON_DECODE_ANY is needed because a member's value stands on its own here,
 * and most of the ones this file wants are numbers or strings. what names the
 * value in whatever this reports.
 */
static int prte_ras_slurm_json_load(prte_json_window_t *win, const char *what, json_t **out)
{
    json_error_t json_err;
    const char *bytes = NULL;
    size_t len = 0;
    int err = prte_json_window_take(win, &bytes, &len);

    *out = NULL;

    if (PRTE_ERR_MEM_LIMIT_EXCEEDED == err) {
        pmix_output(0, "ras:slurm:read_job: %s in the record does not fit the"
                       " %lu bytes this DVM reads a field into.",
                    what, (unsigned long) PRTE_SLURM_JSON_WINDOW_SIZE);
        return err;
    }

    if (PRTE_SUCCESS != err) {
        return err;
    }

    *out = json_loadb(bytes, len, JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &json_err);

    if (NULL == *out) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:read_job: %s did not parse: %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), what, json_err.text));
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    return PRTE_SUCCESS;
}

/*
 * The listed name matching key, or NULL.
 *
 * What comes back is the list's own pointer, which outlives the window's view
 * of the name.
 */
static const char *prte_ras_slurm_json_wanted(const char *const *keys, const char *key)
{
    size_t i;

    for (i = 0; NULL != keys[i]; i++) {
        if (0 == strcmp(keys[i], key)) {
            return keys[i];
        }
    }

    return NULL;
}

/* Members a caller asked to keep, and those kept so far. */
typedef struct {
    const char *const *keys;
    json_t *kept;
} prte_ras_slurm_json_keep_t;

static int prte_ras_slurm_json_keep_member(prte_json_window_t *win, const char *key, void *cbdata)
{
    prte_ras_slurm_json_keep_t *keep = (prte_ras_slurm_json_keep_t *) cbdata;
    const char *wanted = prte_ras_slurm_json_wanted(keep->keys, key);
    json_t *value = NULL;
    int err;

    if (NULL == wanted) {
        return prte_json_window_skip(win);
    }

    if (NULL != json_object_get(keep->kept, wanted)) {
        /* Two of a member we act on leaves no way to say which one the record
         * meant. */
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    err = prte_ras_slurm_json_load(win, wanted, &value);

    if (PRTE_SUCCESS == err && 0 != json_object_set_new(keep->kept, wanted, value)) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
    }

    return err;
}

/*
 * How a job's allocated nodes are reported.
 *
 * The nodes sit three levels below the job, and each level skips every member
 * it was not asked for:
 *
 *     jobs[0]
 *       job_resources
 *         nodes
 *           count         what Slurm says the array holds
 *           allocation[]  one element per node, each carrying a bit for
 *                         every socket and every core of that node
 *
 * That is one handler per level, and then one call per node:
 *
 *     job_nodes_member  keeps threads_per_core, descends into job_resources
 *     resources_member  descends into nodes
 *     nodes_member      takes count, walks allocation
 *     alloc_element     parses one node and hands it to node_cb
 *
 * count_cb runs once with the count, before the first node, and may refuse.
 * node_cb runs once per node with that node's record, which is released as
 * soon as it returns.
 */
typedef int (*prte_ras_slurm_alloc_count_fn_t)(size_t node_count, void *cbdata);
typedef int (*prte_ras_slurm_alloc_node_fn_t)(size_t index, json_t *node_obj, void *cbdata);

typedef struct {
    prte_ras_slurm_alloc_count_fn_t count_cb;
    prte_ras_slurm_alloc_node_fn_t node_cb;
    void *cbdata;
    prte_ras_slurm_json_keep_t keep;
    size_t stated;              /* nodes/count, when the record gave one  */
    size_t seen;                /* elements handed to node_cb             */
    bool have_count;
    bool seen_resources;
    bool seen_allocation;
} prte_ras_slurm_alloc_walk_t;

static int prte_ras_slurm_json_alloc_element(prte_json_window_t *win, size_t index, void *cbdata)
{
    prte_ras_slurm_alloc_walk_t *walk = (prte_ras_slurm_alloc_walk_t *) cbdata;
    json_t *node_obj = NULL;
    int err = prte_ras_slurm_json_load(win, "an allocated node", &node_obj);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    walk->seen++;
    err = walk->node_cb(index, node_obj, walk->cbdata);
    json_decref(node_obj);
    return err;
}

/*
 * Take the count Slurm states for the array.
 *
 * This is the only thing that lets a caller size anything up front. It is
 * Slurm's claim about the array and not a fact about it, so the walk checks
 * it against the elements it actually reads.
 */
static int prte_ras_slurm_json_take_node_count(prte_json_window_t *win,
                                               prte_ras_slurm_alloc_walk_t *walk)
{
    json_t *value = NULL;
    int err;

    if (walk->have_count) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    /* Named as a literal: key points into the window, and loading the value
     * is what lets the window reuse the room the name sits in. */
    err = prte_ras_slurm_json_load(win, "the node count", &value);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    if (!json_is_integer(value) || 0 > json_integer_value(value)) {
        json_decref(value);
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    walk->stated = (size_t) json_integer_value(value);
    walk->have_count = true;
    json_decref(value);

    if (NULL != walk->count_cb) {
        return walk->count_cb(walk->stated, walk->cbdata);
    }

    return PRTE_SUCCESS;
}

/* Walk the array of allocated nodes, one element at a time. */
static int prte_ras_slurm_json_walk_allocation(prte_json_window_t *win,
                                               prte_ras_slurm_alloc_walk_t *walk)
{
    if (walk->seen_allocation) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    /* A caller that sizes something from the count needs it before the first
     * element, and Slurm prints it first. */
    if (NULL != walk->count_cb && !walk->have_count) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    walk->seen_allocation = true;
    return prte_json_window_walk_array(win, prte_ras_slurm_json_alloc_element, walk);
}

static int prte_ras_slurm_json_nodes_member(prte_json_window_t *win, const char *key, void *cbdata)
{
    prte_ras_slurm_alloc_walk_t *walk = (prte_ras_slurm_alloc_walk_t *) cbdata;

    if (0 == strcmp(key, "count")) {
        return prte_ras_slurm_json_take_node_count(win, walk);
    }

    if (0 == strcmp(key, "allocation")) {
        return prte_ras_slurm_json_walk_allocation(win, walk);
    }

    return prte_json_window_skip(win);
}

static int prte_ras_slurm_json_resources_member(prte_json_window_t *win, const char *key,
                                                void *cbdata)
{
    if (0 == strcmp(key, "nodes")) {
        return prte_json_window_walk_object(win, prte_ras_slurm_json_nodes_member, cbdata);
    }

    return prte_json_window_skip(win);
}

static int prte_ras_slurm_json_job_nodes_member(prte_json_window_t *win, const char *key,
                                                void *cbdata)
{
    prte_ras_slurm_alloc_walk_t *walk = (prte_ras_slurm_alloc_walk_t *) cbdata;

    if (0 == strcmp(key, "job_resources")) {
        if (walk->seen_resources) {
            return PRTE_ERR_JSON_PARSE_FAILURE;
        }

        walk->seen_resources = true;
        return prte_json_window_walk_object(win, prte_ras_slurm_json_resources_member, cbdata);
    }

    return prte_ras_slurm_json_keep_member(win, key, &walk->keep);
}

/*
 * How a record is read.
 *
 * "scontrol show job <id> --json" answers with one document, shaped so:
 *
 *     { "meta": {...}, "errors": [...], "jobs": [ { ...the job... } ] }
 *
 * Only "jobs" is read. Every other member of the record is skipped, as is
 * every member of the job the caller did not ask for. Reading the record
 * therefore takes a handler per level, each one calling into the next:
 *
 *     prte_ras_slurm_json_walk_record
 *       record_member  once per member of the record, skips all but "jobs"
 *       only_job       once per element of "jobs", refuses a second element
 *       job_handler    once per member of the job, supplied by the caller
 *
 * What this takes the record to be:
 *
 * - "jobs" holds exactly one job. The query names a single job id, so a
 *   second element means the answer is not to the question this file asked,
 *   and every field read out of it would be ambiguous.
 * - Member order does not matter here. A caller that needs an order says so
 *   for itself.
 * - A skipped member is measured and discarded, never parsed, so a malformed
 *   value inside one goes unnoticed. Only what a caller keeps is parsed.
 */
typedef struct {
    prte_json_window_member_fn_t job_handler;
    void *job_cbdata;
    bool saw_jobs;   /* the record carried a "jobs" member */
    bool saw_job;    /* that member carried a job          */
} prte_ras_slurm_json_record_t;

static int prte_ras_slurm_json_only_job(prte_json_window_t *win, size_t index, void *cbdata)
{
    prte_ras_slurm_json_record_t *record = (prte_ras_slurm_json_record_t *) cbdata;

    if (0 != index) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    record->saw_job = true;
    return prte_json_window_walk_object(win, record->job_handler, record->job_cbdata);
}

static int prte_ras_slurm_json_record_member(prte_json_window_t *win, const char *key,
                                             void *cbdata)
{
    prte_ras_slurm_json_record_t *record = (prte_ras_slurm_json_record_t *) cbdata;

    if (0 != strcmp(key, "jobs")) {
        return prte_json_window_skip(win);
    }

    if (record->saw_jobs) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    record->saw_jobs = true;
    return prte_json_window_walk_array(win, prte_ras_slurm_json_only_job, record);
}

/* Walk the record's one job, handing each of its members to job_handler. */
static int prte_ras_slurm_json_walk_record(prte_json_window_t *win,
                                           prte_json_window_member_fn_t job_handler,
                                           void *job_cbdata)
{
    prte_ras_slurm_json_record_t record = {job_handler, job_cbdata, false, false};
    int err = prte_json_window_walk_object(win, prte_ras_slurm_json_record_member, &record);

    if (PRTE_SUCCESS == err && !record.saw_job) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    return err;
}

/*
 * Run "scontrol show job" and walk the record it prints.
 *
 * Owns the command and the pipe. What the record contains is
 * prte_ras_slurm_json_walk_record's business.
 *
 * @param[in] slurm_jobid  Slurm job ID to query.
 * @param[in] job_handler  handler for each member of the job's record.
 * @param[in] job_cbdata   passed to job_handler.
 */
static int prte_ras_slurm_json_run(const char *slurm_jobid,
                                   prte_json_window_member_fn_t job_handler, void *job_cbdata)
{
    static const char *cmd_format = "scontrol show job %s --json";

    prte_ras_slurm_json_src_t src;
    prte_json_window_t win;
    char *buf = NULL;
    char *cmd = NULL;
    int status;
    int err;

    if (NULL == slurm_jobid || NULL == job_handler) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    if (0 > asprintf(&cmd, cmd_format, slurm_jobid)) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    buf = malloc(PRTE_SLURM_JSON_WINDOW_SIZE);

    if (NULL == buf) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    src.io_error = false;
    src.fp = popen(cmd, "r");

    if (NULL == src.fp) {
        err = PRTE_ERR_FILE_OPEN_FAILURE;
        goto cleanup;
    }

    prte_json_window_init(&win, prte_ras_slurm_json_read, &src, buf,
                          PRTE_SLURM_JSON_WINDOW_SIZE);
    err = prte_ras_slurm_json_walk_record(&win, job_handler, job_cbdata);
    prte_json_window_drain(&win);

    status = pclose(src.fp);
    src.fp = NULL;

    if (-1 == status) {
        pmix_output(0, "ras:slurm:read_job: pclose failed: %s.", strerror(errno));
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    /* The record is read to the end even when a member is refused, so scontrol
     * is never left writing into a closed pipe and its status means what it
     * says. Its status is checked ahead of the walk's own error, which a
     * scontrol that failed would explain. */
    if (src.io_error) {
        err = PRTE_ERR_FILE_READ_FAILURE;
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:read_job: error reading from stream.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (!WIFEXITED(status)) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:read_job: scontrol died on signal %d.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), WTERMSIG(status)));
        err = PRTE_ERR_SLURM_QUERY_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (0 != WEXITSTATUS(status)) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:read_job: non-zero exit code (%d) from scontrol command.",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), WEXITSTATUS(status)));
        err = PRTE_ERR_SLURM_QUERY_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
    }

    cleanup:

    if (NULL != src.fp) {
        pclose(src.fp);
    }

    free(buf);
    free(cmd);

    return err;
}

/*
 * Read the named members of a job's record.
 *
 * What comes back is an object holding whichever of keys the record carried,
 * so the helpers that read a whole record work on it unchanged.
 *
 * @param[in]  slurm_jobid   Slurm job ID to query.
 * @param[in]  keys          NULL-terminated member names to keep.
 * @param[out] job_info_out  New reference the caller decrefs.
 */
static int prte_ras_slurm_read_job_fields(const char *slurm_jobid, const char *const *keys,
                                          json_t **job_info_out)
{
    prte_ras_slurm_json_keep_t keep;
    int err;

    if (NULL == keys || NULL == job_info_out) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    *job_info_out = NULL;
    keep.keys = keys;
    keep.kept = json_object();

    if (NULL == keep.kept) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    err = prte_ras_slurm_json_run(slurm_jobid, prte_ras_slurm_json_keep_member, &keep);

    if (PRTE_SUCCESS != err) {
        json_decref(keep.kept);
        return err;
    }

    *job_info_out = keep.kept;
    return PRTE_SUCCESS;
}

/*
 * Report a job's allocated nodes without holding its record.
 *
 * Each node's record is handed to node_cb and released before the next is
 * read, so the cost is one node rather than the job. count_cb, where given,
 * runs first with the count Slurm states, and the array is then required to
 * hold exactly that many.
 *
 * threads_per_core is a member of the job rather than of a node, and Slurm
 * prints it after the array, so it is only settled once this returns. It
 * reads back as 1 where the record leaves it unset or infinite.
 *
 * @param[in]  slurm_jobid       Slurm job ID to query.
 * @param[in]  count_cb          called once with nodes/count; may be NULL.
 * @param[in]  node_cb           called once per allocated node.
 * @param[in]  cbdata            passed to both callbacks.
 * @param[out] threads_per_core  the job's threads_per_core; may be NULL.
 */
static int prte_ras_slurm_walk_alloc_nodes(const char *slurm_jobid,
                                           prte_ras_slurm_alloc_count_fn_t count_cb,
                                           prte_ras_slurm_alloc_node_fn_t node_cb, void *cbdata,
                                           int *threads_per_core)
{
    static const char *const keys[] = {"threads_per_core", NULL};

    prte_ras_slurm_alloc_walk_t walk = {0};
    int64_t threads = 0;
    int err;

    if (NULL == node_cb) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    walk.count_cb = count_cb;
    walk.node_cb = node_cb;
    walk.cbdata = cbdata;
    walk.keep.keys = keys;
    walk.keep.kept = json_object();

    if (NULL == walk.keep.kept) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    err = prte_ras_slurm_json_run(slurm_jobid, prte_ras_slurm_json_job_nodes_member, &walk);

    if (PRTE_SUCCESS == err && !walk.seen_allocation) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
    }

    if (PRTE_SUCCESS == err && walk.have_count && walk.seen != walk.stated) {
        pmix_output(0, "ras:slurm:walk_alloc_nodes: job %s says it holds %lu nodes"
                       " but listed %lu.",
                    slurm_jobid, (unsigned long) walk.stated, (unsigned long) walk.seen);
        err = PRTE_ERR_JSON_PARSE_FAILURE;
    }

    if (PRTE_SUCCESS == err && NULL != threads_per_core) {
        err = prte_ras_slurm_get_json_numobj_value(walk.keep.kept, "threads_per_core", &threads);

        /* Unset, infinite and out of range all read back as one thread. */
        if (PRTE_SUCCESS == err) {
            *threads_per_core = (0 < threads && PRTE_SLURM_MAX_THREADS_PER_CORE >= threads)
                                    ? (int) threads
                                    : 1;
        }
    }

    json_decref(walk.keep.kept);
    return err;
}

/**
 * Check if we have the Jansson library available in compilation
 */
bool prte_ras_slurm_have_jansson(void)
{
    return true;
}

/*
 * Extract selected Slurm job fields using JSON and populate a PMIx hash table.
 *
 * Retrieves the SLURM job ID from the environment, queries job information,
 * parses the returned JSON using Jansson, and inserts selected numeric and
 * string fields into the provided hash table.
 *
 * Missing, null, and empty optional fields are skipped. String fields that are
 * present are validated to ensure they do not contain control characters.
 *
 * @param[in,out] values_table Pointer to a PMIx hash table to populate with extracted values.

 * Note: On failure, values_table may be partially populated.
 */
int prte_ras_slurm_extract_job_fields(pmix_hash_table_t *values_table)
{
    if(NULL == values_table) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;
    int pmix_err = PMIX_SUCCESS;

    json_t *job = NULL;

    char *slurm_jobid;
    if (NULL == (slurm_jobid = prte_common_slurm_jobid())) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* Built from the tables the loops below walk, so the two cannot drift. */
    const char *keys[STR_FIELD_COUNT + NUM_OBJ_FIELD_COUNT + 1];
    size_t nkeys = 0;

    for(size_t i = 0; i < NUM_OBJ_FIELD_COUNT; i++) {
        keys[nkeys++] = num_obj_fields[i];
    }

    for(size_t i = 0; i < STR_FIELD_COUNT; i++) {
        keys[nkeys++] = str_fields[i];
    }

    keys[nkeys] = NULL;

    err = prte_ras_slurm_read_job_fields(slurm_jobid, keys, &job);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    /* We've extracted a valid "jobs" section. now extract the complex numeric
    * fields that have "set", "infinite", and "number" subfields */
    for(size_t i = 0; i < NUM_OBJ_FIELD_COUNT; i++) {
        err = prte_ras_slurm_get_json_numobj_field(job, num_obj_fields[i], values_table);
        if (PRTE_ERR_NOT_FOUND == err) {
            err = PRTE_SUCCESS;
            continue;
        }
        if (PRTE_SUCCESS != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    /* Find the string fields and add them to our values table */

    for(size_t i = 0; i < STR_FIELD_COUNT; i++) {

        json_t *str_field = json_object_get(job, str_fields[i]);

        if(NULL == str_field || json_is_null(str_field)) {
            continue;
        }
        if(!json_is_string(str_field)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        const char *str = json_string_value(str_field);
        size_t str_len = json_string_length(str_field);
        bool has_control_chars;

        if (0 == str_len) {
            continue;
        }

        /* Do not accept string if contains control characters */
        err = prte_ras_slurm_token_has_control_chars(str, str_len, &has_control_chars);

        if(PRTE_SUCCESS == err && has_control_chars) {
            err = PRTE_ERR_BAD_PARAM;
        }

        if(PRTE_SUCCESS != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        char *str_dup = strdup(str);

        if(NULL == str_dup) {
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        pmix_err = pmix_hash_table_set_value_ptr(values_table, str_fields[i],
                    strlen(str_fields[i]), str_dup);

        if(PMIX_SUCCESS != pmix_err) {
            free(str_dup);
            err = prte_pmix_convert_status(pmix_err);
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

    }

    cleanup:

    if(NULL != job) {
        json_decref(job);
    }

    return err;
}

/*
 * One node as the allocation array describes it.
 *
 * The list is built whole before any prte_node_t is, so a record that fails
 * partway adds nothing to the caller's list.
 */
typedef struct {
    char *name;
    int core_count;
    int cpu_count;
} prte_ras_slurm_alloc_node_t;

typedef struct {
    prte_ras_slurm_alloc_node_t *nodes;
    size_t count;
    size_t cap;
} prte_ras_slurm_alloc_list_t;

static void prte_ras_slurm_alloc_list_destruct(prte_ras_slurm_alloc_list_t *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        free(list->nodes[i].name);
    }

    free(list->nodes);
    list->nodes = NULL;
    list->count = 0;
    list->cap = 0;
}

static int prte_ras_slurm_alloc_list_add(prte_ras_slurm_alloc_list_t *list, const char *name,
                                         int core_count, int cpu_count)
{
    char *name_dup;

    if (list->count == list->cap) {
        size_t cap = (0 == list->cap) ? 16 : list->cap * 2;
        prte_ras_slurm_alloc_node_t *grown = realloc(list->nodes, cap * sizeof(*grown));

        if (NULL == grown) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        list->nodes = grown;
        list->cap = cap;
    }

    name_dup = strdup(name);

    if (NULL == name_dup) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    list->nodes[list->count].name = name_dup;
    list->nodes[list->count].core_count = core_count;
    list->nodes[list->count].cpu_count = cpu_count;
    list->count++;

    return PRTE_SUCCESS;
}

/*
 * Turn a collected allocation into nodes the DVM can be grown onto.
 *
 * "cpus" is the number of hardware threads the node offers. To respect what
 * the original job asked for, slots are (allocated cores * threads_per_core)
 * held to that ceiling.
 */
static int prte_ras_slurm_alloc_list_to_nodes(prte_ras_slurm_alloc_list_t *list,
                                              int threads_per_core, pmix_list_t *node_list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        prte_ras_slurm_alloc_node_t *entry = &list->nodes[i];
        prte_node_t *node;
        int slots;

        if (entry->core_count > INT_MAX / threads_per_core) {
            PRTE_ERROR_LOG(PRTE_ERR_JSON_PARSE_FAILURE);
            return PRTE_ERR_JSON_PARSE_FAILURE;
        }

        slots = entry->core_count * threads_per_core;

        if (slots > entry->cpu_count) {
            slots = entry->cpu_count;
        }

        node = PMIX_NEW(prte_node_t);

        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* These nodes are being added to a DVM that is already running, so
         * they must carry the mark the DVM extension selects on: a grow
         * launches daemons on the nodes in PRTE_NODE_STATE_ADDED and only
         * those (prte_plm_base_setup_virtual_machine). Handing them over as
         * plain UP - the state an initial discovery uses - left every node
         * Slurm granted us sitting in the pool with no daemon on it, so the
         * extend added resources the DVM could never use. The other producers
         * of a grow (ras/hosts, the no-scheduler insert path, and the
         * reused-node branch of this component) all mark ADDED for exactly
         * this reason. */
        node->state = PRTE_NODE_STATE_ADDED;
        node->name = entry->name;
        entry->name = NULL;
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = slots;
        /* derived from the allocation Slurm just granted - authoritative */
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);

        pmix_list_append(node_list, &node->super);

        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
            "%s ras:slurm:add_modified_resources: discovered node %s with %d slots",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, node->slots));
    }

    return PRTE_SUCCESS;
}

/*
 * Fetch and parse Slurm job resource JSON and add allocated nodes and slots.
 *
 * Given a Slurm job ID, this function retrieves the job resource description,
 * validates the expected JSON structure, and creates one node entry for
 * each allocated node in the job.
 *
 * Slot calculation is based on the number of allocated cores found in the
 * socket/core status data, multiplied by the effective threads-per-core value,
 * and capped by cpus.count:
 *
 * slots = min(allocated_cores * threads_per_core, cpus.count)
 *
 * The resulting nodes are inserted into the provided node list.
 *
 * @param[in] slurm_jobid Slurm job ID.
 * @param[in,out] node_list. A pmix_list_t to add nodes to.
 */
/* One element of the allocation array: its name, and how many of its cores
 * Slurm marked allocated. */
static int prte_ras_slurm_collect_alloc_node(size_t index, json_t *node_obj, void *cbdata)
{
    prte_ras_slurm_alloc_list_t *collected = (prte_ras_slurm_alloc_list_t *) cbdata;
    int err = PRTE_SUCCESS;

    PRTE_HIDE_UNUSED_PARAMS(index);

    if (!json_is_object(node_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_t *nodename = json_object_get(node_obj, "name");

    if (NULL == nodename || !json_is_string(nodename)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    const char *nodename_string = json_string_value(nodename);

    if (NULL == nodename_string || '\0' == nodename_string[0]) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_t *cpu_info = json_object_get(node_obj, "cpus");

    if (NULL == cpu_info || !json_is_object(cpu_info)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_t *cpu_count_obj = json_object_get(cpu_info, "count");

    if (NULL == cpu_count_obj || !json_is_integer(cpu_count_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_int_t cpu_count_num = json_integer_value(cpu_count_obj);

    if (0 >= cpu_count_num || INT_MAX < cpu_count_num) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    int cpu_max_count = (int)cpu_count_num;

    int core_count = 0;

    json_t *sockets = json_object_get(node_obj, "sockets");

    if(NULL == sockets || !json_is_array(sockets)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    size_t socket_idx;
    json_t *socket_obj;

    json_array_foreach(sockets, socket_idx, socket_obj) {

        if (!json_is_object(socket_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
        }

        json_t *cores = json_object_get(socket_obj, "cores");

        if (NULL == cores || !json_is_array(cores)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
        }

        size_t core_idx;
        json_t *core_obj;

        json_array_foreach(cores, core_idx, core_obj) {

        if(!json_is_object(core_obj)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            return err;
        }

        json_t *statuses = json_object_get(core_obj, "status");
        if (NULL == statuses || !json_is_array(statuses)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            return err;
        }

        size_t status_idx;
        json_t *status_obj;

        json_array_foreach(statuses, status_idx, status_obj) {

            if(!json_is_string(status_obj)) {
                err = PRTE_ERR_JSON_PARSE_FAILURE;
                PRTE_ERROR_LOG(err);
                return err;
            }

            if (0 == strcmp(json_string_value(status_obj), "ALLOCATED")) {
                core_count++;

                if(PRTE_SLURM_MAX_CORE_COUNT < core_count) {
                err = PRTE_ERR_JSON_PARSE_FAILURE;
                PRTE_ERROR_LOG(err);
                return err;
                }

                break;
            }
        }
        }
    }

    if(0 >= core_count) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    return prte_ras_slurm_alloc_list_add(collected, nodename_string, core_count,
                                        cpu_max_count);
}

int prte_ras_slurm_add_modified_resources(const char *slurm_jobid, pmix_list_t *node_list)
{
    prte_ras_slurm_alloc_list_t collected = {NULL, 0, 0};
    int threads_per_core = 1;
    uint32_t jobid_val;
    int err;

    if(NULL == slurm_jobid || NULL == node_list) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    err = prte_ras_slurm_convert_jobid(slurm_jobid, &jobid_val);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    err = prte_ras_slurm_walk_alloc_nodes(slurm_jobid, NULL, prte_ras_slurm_collect_alloc_node,
                                          &collected, &threads_per_core);

    if(PRTE_SUCCESS == err) {
        err = prte_ras_slurm_alloc_list_to_nodes(&collected, threads_per_core, node_list);
    }

    prte_ras_slurm_alloc_list_destruct(&collected);

    return err;
}

/**
 * Synchronize a session's node list with a reduced Slurm allocation.
 *
 * Nodes no longer present in the allocation are appended to
 * removed_nodes.
 *
 * @param[in] slurm_jobid Slurm job identifier.
 * @param[in,out] session Session to update.
 * @param[out] removed_nodes Receives detached nodes; must be empty.
 */
int prte_ras_slurm_detach_nodes(const char *slurm_jobid, prte_session_t *session, pmix_pointer_array_t *removed_nodes)
{
    if (NULL == slurm_jobid || NULL == removed_nodes ||
        NULL == session || NULL == session->nodes || 
        0 != removed_nodes->size) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    pmix_pointer_array_t matched_nodes, unmatched_nodes;
    PMIX_CONSTRUCT(&matched_nodes, pmix_pointer_array_t);
    PMIX_CONSTRUCT(&unmatched_nodes, pmix_pointer_array_t);

    json_t *root = NULL;

    err = prte_ras_slurm_get_jobinfo_json(slurm_jobid, &root);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    json_t *job_resources = json_object_get(root, "job_resources");

    if(NULL == job_resources || !json_is_object(job_resources)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_t *nodes = json_object_get(job_resources, "nodes");

    if(NULL == nodes || !json_is_object(nodes)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_t *allocation = json_object_get(nodes, "allocation");

    if (NULL == allocation || !json_is_array(allocation)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    size_t new_alloc_size = json_array_size(allocation);
    size_t session_nodes_size = 0;

    for (int i = 0; i < session->nodes->size; i++) {
        prte_node_t *node_ptr = (prte_node_t *)pmix_pointer_array_get_item(session->nodes, i);

        if (NULL != node_ptr) {
            pmix_pointer_array_add(&unmatched_nodes, node_ptr);
            session_nodes_size++;
        }
    }

    /* sanity checks, as we rely on these assumptions implicitly later */
    if (0 == new_alloc_size || 0 == session_nodes_size 
        || new_alloc_size >= session_nodes_size) {
        err = PRTE_ERR_BAD_PARAM;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    /* retrieve surviving nodes in Slurm-provided order */

    for (size_t i = 0; i < new_alloc_size; i++) {
        json_t *node_obj = json_array_get(allocation, i);

        if (!json_is_object(node_obj)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        json_t *nodename = json_object_get(node_obj, "name");

        if (NULL == nodename || !json_is_string(nodename)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        const char *nodename_string = json_string_value(nodename);

        if (NULL == nodename_string || '\0' == nodename_string[0]) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        int session_node_idx = (int)i;
        bool found = false;

        /* find the equivalent node in the list of unmatched nodes. 
         * We expect (but do not strictly require) the ordering here
         * to be favorable; i.e. node at index i should be the
         * one we are looking for */
        for (int j = 0; j < unmatched_nodes.size; j++) {
            prte_node_t *curr = (prte_node_t *)pmix_pointer_array_get_item(&unmatched_nodes, (int)session_node_idx);
            
            if (NULL != curr && NULL != curr->name &&
                0 == strcmp(curr->name, nodename_string)) {
                found = true;
                pmix_pointer_array_add(&matched_nodes, curr);
                pmix_pointer_array_set_item(&unmatched_nodes, session_node_idx, NULL);
                break;
            }
            
            if(++session_node_idx >= unmatched_nodes.size) {
                session_node_idx = 0;
            }
        }

        if(!found) {
            err = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    /* success, clear out old entries and reconstruct the node list*/

    pmix_pointer_array_remove_all(session->nodes);

    for (int i = 0; i < matched_nodes.size; i++) {
        prte_node_t *curr = (prte_node_t *)pmix_pointer_array_get_item(&matched_nodes, i);
        if(NULL != curr) {
            pmix_pointer_array_set_item(session->nodes, i, curr);
        }
    }

    pmix_pointer_array_remove_all(&matched_nodes);

    for (int i = 0; i < unmatched_nodes.size; i++) {
        prte_node_t *curr = (prte_node_t *)pmix_pointer_array_get_item(&unmatched_nodes, i);
        if(NULL != curr) {
            pmix_pointer_array_add(removed_nodes, curr);
        }
    }

    pmix_pointer_array_remove_all(&unmatched_nodes);

cleanup:

    if(NULL != root) {
        json_decref(root);
    }

    PMIX_DESTRUCT(&matched_nodes);
    PMIX_DESTRUCT(&unmatched_nodes);

    return err;
}

/*
 * Check the state of a Slurm job against RUNNING and PENDING.
 *
 * Polls Slurm job information and inspects the "job_state" JSON
 * field for PENDING and RUNNING. The function returns PRTE_SUCCESS
 * if the job reaches RUNNING.
 *
 * @param[in] slurm_jobid SLURM job ID to monitor.
 */
int prte_ras_slurm_check_resources(const char *slurm_jobid)
{
    int err = PRTE_SUCCESS;

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        return PRTE_ERR_BAD_PARAM;
    }

    json_t *job_info = NULL;

    bool running = false;
    bool pending = false;
    bool cancelled = false;

    static const char *const keys[] = {"job_state", NULL};

    err = prte_ras_slurm_read_job_fields(slurm_jobid, keys, &job_info);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    json_t *job_states = json_object_get(job_info, "job_state");

    /* A job can have multiple states in Slurm */
    if (NULL == job_states || !json_is_array(job_states)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(PRTE_ERR_JSON_PARSE_FAILURE);
        goto cleanup;
    }

    size_t i;
    json_t *state_val;

    json_array_foreach(job_states, i, state_val) {

        if(!json_is_string(state_val)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        const char *state = json_string_value(state_val);

        if (strcmp(state, "RUNNING") == 0) {
            running = true;
        }

        else if (strcmp(state, "PENDING") == 0) {
            pending = true;
        }

        else if (strcmp(state, "CANCELLED") == 0) {
            cancelled = true;
        }
    }

    json_decref(job_info);
    job_info = NULL;

    /* Exactly one recognized Slurm state is expected here. */
    int recognized_states = (running ? 1 : 0) + (pending ? 1 : 0) + (cancelled ? 1 : 0);

    if (1 != recognized_states) {
        err = PRTE_ERR_SLURM_BAD_JOB_STATUS;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if(cancelled) {
        err = PRTE_ERR_JOB_CANCELLED;
    } else if(!running) {
        err = PRTE_ERR_RESOURCE_BUSY;
    }

    cleanup:

    if(NULL != job_info) {
        json_decref(job_info);
    }

    return err;
}

/*
 * Read the value of one Slurm numeric object out of a job record.
 *
 * A field that is absent, unset or infinite reads back as 0; only a field of
 * the wrong shape fails.
 *
 * @param[in]  job JSON job object.
 * @param[in]  key Field name to read.
 * @param[out] out The number, or 0.
 */
static int prte_ras_slurm_get_json_numobj_value(json_t *job, const char *key, int64_t *out)
{
    if (NULL == job || NULL == key || NULL == out) {
        return PRTE_ERR_BAD_PARAM;
    }

    *out = 0;

    json_t *field = json_object_get(job, key);
    if (NULL == field || json_is_null(field)) {
        return PRTE_SUCCESS;
    }
    if (!json_is_object(field)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    json_t *set_flag = json_object_get(field, num_obj_subfields[NUM_OBJ_SUBFIELD_SET]);
    if (NULL == set_flag || !json_is_boolean(set_flag)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }
    if (!json_is_true(set_flag)) {
        return PRTE_SUCCESS;
    }

    json_t *inf_flag = json_object_get(field, num_obj_subfields[NUM_OBJ_SUBFIELD_INFINITE]);
    if (NULL == inf_flag || !json_is_boolean(inf_flag)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }
    if (json_is_true(inf_flag)) {
        return PRTE_SUCCESS;
    }

    json_t *num_field = json_object_get(field, num_obj_subfields[NUM_OBJ_SUBFIELD_NUMBER]);
    if (NULL == num_field || !json_is_integer(num_field)) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    json_int_t num = json_integer_value(num_field);
    if (0 > num) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    *out = (int64_t) num;

    return PRTE_SUCCESS;
}

/*
 * Read a job's start and end times, in seconds since the epoch.
 *
 * Either output may be NULL. A time Slurm does not report comes back as 0, as
 * does the end of a job with no time limit: Slurm prints an end_time for one
 * anyway - its start plus a year - which is a placeholder, not a deadline.
 *
 * end_time is the start plus the CURRENT time limit, so it answers "when does
 * this allocation end" only once the job is running.
 *
 * @param[in]  slurm_jobid Slurm job ID to query.
 * @param[out] start_time  Job start time, or 0.
 * @param[out] end_time    Job end time, or 0 if it has none.
 */
int prte_ras_slurm_get_job_times(const char *slurm_jobid, time_t *start_time, time_t *end_time)
{
    int err = PRTE_SUCCESS;
    json_t *job_info = NULL;
    int64_t time_limit = 0;
    int64_t start = 0;
    int64_t end = 0;

    if (NULL == slurm_jobid) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    static const char *const keys[] = {"start_time", "end_time", "time_limit", NULL};

    err = prte_ras_slurm_read_job_fields(slurm_jobid, keys, &job_info);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    err = prte_ras_slurm_get_json_numobj_value(job_info, "start_time", &start);

    if (PRTE_SUCCESS == err) {
        err = prte_ras_slurm_get_json_numobj_value(job_info, "end_time", &end);
    }

    if (PRTE_SUCCESS == err) {
        err = prte_ras_slurm_get_json_numobj_value(job_info,
                                                   num_obj_fields[NUM_OBJ_TIME_LIMIT],
                                                   &time_limit);
    }

    json_decref(job_info);

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    if (0 == time_limit) {
        end = 0;
    }

    if (NULL != start_time) {
        *start_time = (time_t) start;
    }

    if (NULL != end_time) {
        *end_time = (time_t) end;
    }

    return PRTE_SUCCESS;
}
