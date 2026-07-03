/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Resource Allocation (SLURM)
 */
#ifndef PRTE_RAS_SLURM_H
#define PRTE_RAS_SLURM_H

#include "prte_config.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/ras/ras.h"

#define PRTE_SLURM_ERR_STR_MAX_LEN 256
#define PRTE_SLURM_JOB_ID_MAX_LEN 20
#define PRTE_SLURM_HOSTNAME_MAX_LEN 256

/* Markers to indicate a given Slurm JSON-format number is set or infinite */
#define PRTE_SLURM_UNSET_NUM_MARKER "prte_slurm_unset"
#define PRTE_SLURM_INFINITE_NUM_MARKER "prte_slurm_inf"

BEGIN_C_DECLS

/* To check if Jansson is available in compilation */
bool prte_ras_slurm_have_jansson(void);

/* Features requiring JSON parser */
int prte_ras_slurm_extract_job_fields(pmix_hash_table_t *values_table);
int prte_ras_slurm_add_modified_resources(const char *slurm_jobid, pmix_list_t *node_list);
int prte_ras_slurm_detach_nodes(const char *slurm_jobid, prte_session_t *session, pmix_pointer_array_t *removed_nodes);
int prte_ras_slurm_check_resources(const char *slurm_jobid);

/* Features to serve cancel requests */
int prte_ras_slurm_add_pending_req(const char *request_id, const char *slurm_job_id);
int prte_ras_slurm_remove_pending_req(const char *request_id);
bool prte_ras_slurm_pending_req_exists(const char *request_id);
int prte_ras_slurm_cancel_pending_req(const char *request_id);
int prte_ras_slurm_modify_cancel_init(void);
int prte_ras_slurm_modify_cancel_finalize(void);
int prte_ras_slurm_serve_cancel_req(prte_pmix_server_req_t *req);

/* Features to serve extension requests */
int prte_ras_slurm_serve_extend_req(prte_pmix_server_req_t *req);

/* Features to serve release requests */
int prte_ras_slurm_modify_release_init(void);
int prte_ras_slurm_modify_release_finalize(void);
int prte_ras_slurm_serve_release_req(prte_pmix_server_req_t *req);
void prte_ras_slurm_shrink_complete(prte_shrink_campaign_t *campaign);
int prte_ras_slurm_release_allocation(prte_session_t *session);

/* Common modify extend/release features */
int prte_ras_slurm_kill_job(const char *slurm_jobid, char *err_msg, size_t err_msg_size);
int prte_ras_slurm_token_has_control_chars(const char *s, size_t len, bool *has_control_chars);
int prte_ras_slurm_drain_cmd_output(FILE *fp, char *output, size_t output_size);

/* Common features for the module */
int prte_ras_slurm_validate_jobid(const char *slurm_jobid);
int prte_ras_slurm_validate_hostname(const char *hostname);
int prte_ras_slurm_convert_jobid(const char *slurm_jobid, uint32_t *slurm_jobid_numeric);
int prte_ras_slurm_assign_new_session(const char *slurm_jobid, const char *user_refid,
                                      pmix_list_t *node_list, bool dynamic);
int prte_ras_slurm_tag_node_allocation(const char *slurm_jobid, pmix_list_t *node_list);

typedef struct {
    prte_ras_base_component_t super;
    int max_length;
    bool use_all;
    bool propagate_account;
    bool propagate_partition;
    bool propagate_qos;
    bool propagate_cwd;
    bool propagate_mem_per_cpu;
    bool propagate_mem_per_node;
    bool propagate_time;
    bool propagate_threads_per_core;
} prte_mca_ras_slurm_component_t;
PRTE_EXPORT extern prte_mca_ras_slurm_component_t prte_mca_ras_slurm_component;

PRTE_EXPORT extern prte_ras_base_module_t prte_ras_slurm_module;

/* String-type fields to parse from Slurm JSON,
   add corresponding entries in ras_slurm_modify_utils.c */

enum slurm_str_field {
    STR_ACCOUNT,
    STR_PARTITION,
    STR_QOS,
    STR_CWD,
    STR_FIELD_COUNT
};

extern const char *const str_fields[STR_FIELD_COUNT];

/* Numberic object type fields to parse from Slurm JSON,
   add corresponding entries in ras_slurm_modify_utils.c */

enum slurm_num_obj_field {
    NUM_OBJ_MEMORY_PER_CPU,
    NUM_OBJ_MEMORY_PER_NODE,
    NUM_OBJ_TIME_LIMIT,
    NUM_OBJ_THREADS_PER_CORE,
    NUM_OBJ_FIELD_COUNT
};

extern const char *const num_obj_fields[NUM_OBJ_FIELD_COUNT];

/* Numberic object type fields to parse from Slurm JSON,
   add corresponding entries in ras_slurm_modify_utils.c */

enum slurm_num_obj_subfield {
    NUM_OBJ_SUBFIELD_SET,
    NUM_OBJ_SUBFIELD_INFINITE,
    NUM_OBJ_SUBFIELD_NUMBER,
    NUM_OBJ_SUBFIELD_COUNT
};

extern const char *const num_obj_subfields[NUM_OBJ_SUBFIELD_COUNT];

/* Job fields used to keep track of new allocations */

enum record_job_data_field {
    PRTE_JOB_DATA_NODES,
    PRTE_JOB_DATA_JOB_ID,
    PRTE_JOB_DATA_COUNT
};

/* Stack item type for our session stack */

typedef struct {
    pmix_list_item_t super;
    prte_session_t *session;
    int nodes_in_session;
} prte_session_stack_item_t;
PMIX_CLASS_DECLARATION(prte_session_stack_item_t);

/* Stack to keep track of our Slurm allocations in LIFO order */

extern pmix_list_t *prte_slurm_session_stack;

END_C_DECLS

#endif
