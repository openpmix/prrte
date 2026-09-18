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

/* What the jansson readers in this component share: the window they read a
 * record through, and the pieces of a record every one of them needs. */

#ifndef PRTE_RAS_SLURM_JANSSON_H
#define PRTE_RAS_SLURM_JANSSON_H

#include "prte_config.h"

#include <stdint.h>

#if PRTE_TESTBUILD_LAUNCHERS
#    include "src/mca/ras/base/testbuild_jansson.h"
#else
#    include <jansson.h>
#endif

#include "src/util/prte_json_window.h"

BEGIN_C_DECLS

/* The largest value the reader ever parses is one node's socket and core
 * map, and a node past PRTE_SLURM_MAX_CORE_COUNT cores is refused before
 * this limit is reached: Slurm spends about 140 bytes per core, so that
 * ceiling is around 600KB. */
#define PRTE_SLURM_JSON_WINDOW_SIZE (1024 * 1024)
#define PRTE_SLURM_MAX_THREADS_PER_CORE 32
#define PRTE_SLURM_MAX_CORE_COUNT 4096

/* Members a caller asked to keep, and those kept so far. */
typedef struct {
    const char *const *keys;
    json_t *kept;
} prte_ras_slurm_json_keep_t;

/* Parse the value the window is at. what names it in whatever this reports. */
int prte_ras_slurm_json_load(prte_json_window_t *win, const char *what, json_t **out);

/* Keep the members named in a prte_ras_slurm_json_keep_t and skip the rest. */
int prte_ras_slurm_json_keep_member(prte_json_window_t *win, const char *key, void *cbdata);

/* Run "scontrol show job" and hand each member of the job it names to
 * job_handler. */
int prte_ras_slurm_json_run(const char *slurm_jobid,
                            prte_json_window_member_fn_t job_handler, void *job_cbdata);

/* Read the named members of a job's record into an object the caller
 * decrefs. */
int prte_ras_slurm_read_job_fields(const char *slurm_jobid, const char *const *keys,
                                   json_t **job_info_out);

/* Read one Slurm numeric object out of a job record. Absent, unset and
 * infinite all read back as 0. */
int prte_ras_slurm_get_json_numobj_value(json_t *job, const char *key, int64_t *out);

END_C_DECLS

#endif /* PRTE_RAS_SLURM_JANSSON_H */
