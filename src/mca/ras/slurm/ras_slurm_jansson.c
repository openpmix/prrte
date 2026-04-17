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

#include <jansson.h>

#include "src/mca/errmgr/errmgr.h"
#include "src/util/pmix_output.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_slurm.h"

#define PRTE_SLURM_JOB_INFO_MAX_SIZE (1 * 1024 * 1024)
#define PRTE_SLURM_MAX_THREADS_PER_CORE 32
#define PRTE_SLURM_MAX_CORE_COUNT 4096

/*
 * Local functions
 */
static int prte_ras_slurm_get_json_numobj_field(json_t *job, const char *key, pmix_hash_table_t *values_table);
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
 * Expects the the JSON object at key in job to have
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
    if (NULL == field || !json_is_object(field)) {
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
    json_t *jobs_arr = json_object_get(parent_json, jobs_field);
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
 * Extract selected Slurm job fields using JSON and populate a PMIx hash table.
 *
 * Retrieves the SLURM job ID from the environment, queries job information,
 * parses the returned JSON using Jansson, and inserts selected numeric and
 * string fields into the provided hash table.
 *
 * String fields are validated to ensure they do not contain control characters.
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
    if (NULL == (slurm_jobid = getenv("SLURM_JOBID"))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* Read JSON from stream and extract the first and only job 
       in the "jobs" array, taking ownership of the returned json. */
    err = prte_ras_slurm_get_jobinfo_json(slurm_jobid, &job);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    /* We've extracted a valid "jobs" section. now extract the complex numeric
    * fields that have "set", "infinite", and "number" subfields */
    for(size_t i = 0; i < NUM_OBJ_SUBFIELD_COUNT; i++) {
        err = prte_ras_slurm_get_json_numobj_field(job, num_obj_fields[i], values_table);
        if (PRTE_SUCCESS != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    /* Find the string fields and add them to our values table */

    for(size_t i = 0; i < STR_FIELD_COUNT; i++) {

        json_t *str_field = json_object_get(job, str_fields[i]);

        if(NULL == str_field || !json_is_string(str_field)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup; 
        }

        const char *str = json_string_value(str_field);
        size_t str_len = json_string_length(str_field);
        bool has_control_chars; 

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
int prte_ras_slurm_add_modified_resources(const char *slurm_jobid, pmix_list_t *node_list) 
{
    if(NULL == slurm_jobid || NULL == node_list) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    uint32_t jobid_val;

    err = prte_ras_slurm_convert_jobid(slurm_jobid, &jobid_val);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    int threads_per_core = 1;

    json_t *root = NULL;

    err = prte_ras_slurm_get_jobinfo_json(slurm_jobid, &root);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    json_t *tpc_obj = json_object_get(root, "threads_per_core");

    if(NULL == tpc_obj || !json_is_object(tpc_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_t *tpc_set_flag = json_object_get(tpc_obj, "set");

    if(NULL == tpc_set_flag || !json_is_boolean(tpc_set_flag)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_t *tpc_inf_flag = json_object_get(tpc_obj, "infinite");

    if(NULL == tpc_inf_flag || !json_is_boolean(tpc_inf_flag)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_t *tpc_val = json_object_get(tpc_obj, "number");

    if(NULL == tpc_val || !json_is_integer(tpc_val)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    json_int_t tpc = json_integer_value(tpc_val);

    if(json_is_true(tpc_set_flag) && json_is_false(tpc_inf_flag)) {
        if (tpc > 0 && tpc <= PRTE_SLURM_MAX_THREADS_PER_CORE) {
            threads_per_core = (int)tpc;
        } else if (tpc == 0) {
            /* Slurm could set to 0 in some cases */
            threads_per_core = 1;
        } else {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
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

    size_t node_idx;
    json_t *node_obj;

    /* Retrieve node names and allocated slots */
    json_array_foreach(allocation, node_idx, node_obj) {
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

        json_t *cpu_info = json_object_get(node_obj, "cpus");

        if (NULL == cpu_info || !json_is_object(cpu_info)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        json_t *cpu_count_obj = json_object_get(cpu_info, "count");

        if (NULL == cpu_count_obj || !json_is_integer(cpu_count_obj)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        json_int_t cpu_count_num = json_integer_value(cpu_count_obj);

        if (0 >= cpu_count_num || INT_MAX < cpu_count_num) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        int cpu_max_count = (int)cpu_count_num;

        int core_count = 0;

        json_t *sockets = json_object_get(node_obj, "sockets");

        if(NULL == sockets || !json_is_array(sockets)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup; 
        }

        size_t socket_idx;
        json_t *socket_obj;

        json_array_foreach(sockets, socket_idx, socket_obj) {

            if (!json_is_object(socket_obj)) {
                err = PRTE_ERR_JSON_PARSE_FAILURE;
                PRTE_ERROR_LOG(err);
                goto cleanup;
            }

            json_t *cores = json_object_get(socket_obj, "cores");

            if (NULL == cores || !json_is_array(cores)) {
                err = PRTE_ERR_JSON_PARSE_FAILURE;
                PRTE_ERROR_LOG(err);
                goto cleanup;
            }

            size_t core_idx;
            json_t *core_obj;

            json_array_foreach(cores, core_idx, core_obj) {

                if(!json_is_object(core_obj)) {
                    err = PRTE_ERR_JSON_PARSE_FAILURE;
                    PRTE_ERROR_LOG(err);
                    goto cleanup;
                }

                json_t *statuses = json_object_get(core_obj, "status");
                if (NULL == statuses || !json_is_array(statuses)) {
                    err = PRTE_ERR_JSON_PARSE_FAILURE;
                    PRTE_ERROR_LOG(err);
                    goto cleanup;
                }

                size_t status_idx;
                json_t *status_obj;

                json_array_foreach(statuses, status_idx, status_obj) {

                    if(!json_is_string(status_obj)) {
                        err = PRTE_ERR_JSON_PARSE_FAILURE;
                        PRTE_ERROR_LOG(err);
                        goto cleanup;
                    }

                    if (0 == strcmp(json_string_value(status_obj), "ALLOCATED")) {
                        core_count++;

                        if(PRTE_SLURM_MAX_CORE_COUNT < core_count) {
                            err = PRTE_ERR_JSON_PARSE_FAILURE;
                            PRTE_ERROR_LOG(err);
                            goto cleanup;
                        }
                        
                        break;
                    }
                }
            }
        }

        if(0 >= core_count) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        if (core_count > INT_MAX / threads_per_core) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        char *nodename_dyn = strdup(nodename_string);

        if (NULL == nodename_dyn) {
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;  
        }

        /* 
        The "cpus" field represents the number of hardware
        threads available. To respect the preferences of the
        original job, we calculate (cores * threads_per_core),
        ensuring it does not exceed the available count as provided
        by the "cpus" field. Note that if threads_per_core is
        unset, infinite, or out of expected bounds, we default to 1.
        If threads_per_core is missing entirely, we error out.
        */

        int slots = core_count * threads_per_core;
        
        if(slots > cpu_max_count) {
            slots = cpu_max_count;
        }

        prte_node_t *node = PMIX_NEW(prte_node_t);
        
        node->state = PRTE_NODE_STATE_UP;
        node->name = nodename_dyn;
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = slots;

        pmix_list_append(node_list, &node->super);

        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
        "%s ras:slurm:add_modified_resources: discovered node %s with "
        "%d slots",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, node->slots)); 
    }

    cleanup:

    if(NULL != root) {
        json_decref(root);
    }

    return err;
}

/*
 * Wait until a Slurm job contains the RUNNING state.
 *
 * Polls Slurm job information once per second and inspects
 * the "job_state" JSON field until the job transitions 
 * out of PENDING. The function returns success only if the job
 * reaches RUNNING.
 *
 * @param[in] slurm_jobid SLURM job ID to monitor.
 */
int prte_ras_slurm_wait_resources(const char *slurm_jobid)
{    
    int err = PRTE_SUCCESS;

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        return PRTE_ERR_BAD_PARAM;
    }

    json_t *job_info = NULL;

    bool running;
    bool pending;

    do {
        running = false;
        pending = false;
        err = prte_ras_slurm_get_jobinfo_json(slurm_jobid, &job_info);

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
        }

        json_decref(job_info);
        job_info = NULL;

        /* Should be mutually exclusive */
        if (running && pending) {
            err = PRTE_ERR_SLURM_BAD_JOB_STATUS;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        /* Avoid overloading Slurm with requests */
        if(pending) {
            sleep(1);
        }

    } while (pending);

    if(!running) {
        err = PRTE_ERR_SLURM_BAD_JOB_STATUS;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    cleanup:

    if(NULL != job_info) {
        json_decref(job_info);
    }

    return err;
}
