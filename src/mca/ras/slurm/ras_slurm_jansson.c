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

/* Reading a Slurm job record through a fixed window. Nothing here knows
 * the shape of a record beyond jobs[0]. */

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
#include "ras_slurm_jansson.h"
#include "src/mca/common/slurm/common_slurm.h"

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
int prte_ras_slurm_json_load(prte_json_window_t *win, const char *what, json_t **out)
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

int prte_ras_slurm_json_keep_member(prte_json_window_t *win, const char *key, void *cbdata)
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
int prte_ras_slurm_json_run(const char *slurm_jobid,
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
int prte_ras_slurm_read_job_fields(const char *slurm_jobid, const char *const *keys,
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

/**
 * Check if we have the Jansson library available in compilation
 */
bool prte_ras_slurm_have_jansson(void)
{
    return true;
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
int prte_ras_slurm_get_json_numobj_value(json_t *job, const char *key, int64_t *out)
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
