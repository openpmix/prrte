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

/* A job's top-level fields: the ones PRRTE records, its state, and its
 * start and end times. */

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
 * Read a job's start and end times out of a record already in hand.
 *
 * Either output may be NULL. Times Slurm does not report come back as 0, and
 * so does the end of a job with no time limit, since the end_time Slurm
 * prints for one is just its start plus a year.
 *
 * Slurm derives end_time from the job's current time limit, so it describes
 * the end of the allocation only once the job is running.
 *
 * @param[in]  job        Members read from a Slurm job record.
 * @param[out] start_time Job start time, or 0.
 * @param[out] end_time   Job end time, or 0 if it has none.
 */
static int prte_ras_slurm_read_job_times(json_t *job, time_t *start_time, time_t *end_time)
{
    int64_t time_limit = 0;
    int64_t start = 0;
    int64_t end = 0;
    int err;

    if (NULL == job) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    err = prte_ras_slurm_get_json_numobj_value(job, "start_time", &start);

    if (PRTE_SUCCESS == err) {
        err = prte_ras_slurm_get_json_numobj_value(job, "end_time", &end);
    }

    if (PRTE_SUCCESS == err) {
        err = prte_ras_slurm_get_json_numobj_value(job, num_obj_fields[NUM_OBJ_TIME_LIMIT],
                                                   &time_limit);
    }

    if (PRTE_SUCCESS != err) {
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
int prte_ras_slurm_extract_job_fields(pmix_hash_table_t *values_table, time_t *start_time,
                                      time_t *end_time)
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
    const char *keys[STR_FIELD_COUNT + NUM_OBJ_FIELD_COUNT + 3];
    size_t nkeys = 0;

    for(size_t i = 0; i < NUM_OBJ_FIELD_COUNT; i++) {
        keys[nkeys++] = num_obj_fields[i];
    }

    for(size_t i = 0; i < STR_FIELD_COUNT; i++) {
        keys[nkeys++] = str_fields[i];
    }

    keys[nkeys++] = "start_time";
    keys[nkeys++] = "end_time";
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

    /* Record the job end time so we know what to trim the job to without
     * paying for another query. */
    err = prte_ras_slurm_read_job_times(job, start_time, end_time);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    cleanup:

    if(NULL != job) {
        json_decref(job);
    }

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
int prte_ras_slurm_check_resources(const char *slurm_jobid, time_t *start_time,
                                   time_t *end_time)
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

    static const char *const keys[] = {"job_state", "start_time", "end_time", "time_limit", NULL};

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

    /* Before the record goes: a time Slurm has not settled reads back as 0,
     * which is what the caller already has to handle. */
    err = prte_ras_slurm_read_job_times(job_info, start_time, end_time);

    json_decref(job_info);
    job_info = NULL;

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

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
 * Read a job's start and end times, in seconds since the epoch.
 *
 * @param[in]  slurm_jobid Slurm job ID to query.
 * @param[out] start_time  Job start time, or 0.
 * @param[out] end_time    Job end time, or 0 if it has none.
 */
int prte_ras_slurm_get_job_times(const char *slurm_jobid, time_t *start_time, time_t *end_time)
{
    static const char *const keys[] = {"start_time", "end_time", "time_limit", NULL};

    json_t *job_info = NULL;
    int err;

    if (NULL == slurm_jobid) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    err = prte_ras_slurm_read_job_fields(slurm_jobid, keys, &job_info);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    err = prte_ras_slurm_read_job_times(job_info, start_time, end_time);
    json_decref(job_info);

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
    }

    return err;
}
