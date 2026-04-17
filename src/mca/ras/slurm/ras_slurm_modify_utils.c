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

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "ras_slurm.h"

const char *const str_fields[STR_FIELD_COUNT] = {
    [STR_ACCOUNT]   = "account",
    [STR_PARTITION] = "partition",
    [STR_QOS]       = "qos",
    [STR_CWD]       = "current_working_directory",
};

const char *const num_obj_fields[NUM_OBJ_FIELD_COUNT] = {
    [NUM_OBJ_MEMORY_PER_CPU]   = "memory_per_cpu",
    [NUM_OBJ_MEMORY_PER_NODE]  = "memory_per_node",
    [NUM_OBJ_TIME_LIMIT]       = "time_limit",
    [NUM_OBJ_THREADS_PER_CORE] = "threads_per_core",
};

const char *const num_obj_subfields[NUM_OBJ_SUBFIELD_COUNT] = {
    [NUM_OBJ_SUBFIELD_SET]      = "set",
    [NUM_OBJ_SUBFIELD_INFINITE] = "infinite",
    [NUM_OBJ_SUBFIELD_NUMBER]   = "number",
};

const char *const record_job_data_fields[PRTE_JOB_DATA_COUNT] = {
    [PRTE_JOB_DATA_NODES]  = "nodes",
    [PRTE_JOB_DATA_JOB_ID] = "job_id",
};

const size_t total_fields_len =
    STR_FIELD_COUNT + NUM_OBJ_FIELD_COUNT + PRTE_JOB_DATA_COUNT;

