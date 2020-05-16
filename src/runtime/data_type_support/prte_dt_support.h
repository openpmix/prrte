/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_DT_SUPPORT_H
#define PRTE_DT_SUPPORT_H

/*
 * includes
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/dss/dss_types.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/plm/plm_types.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/iof/iof_types.h"

#include "src/runtime/prte_globals.h"


BEGIN_C_DECLS

/** Data type compare functions */
int prte_dt_compare_std_cntr(prte_std_cntr_t *value1, prte_std_cntr_t *value2, prte_data_type_t type);
int prte_dt_compare_job(prte_job_t *value1, prte_job_t *value2, prte_data_type_t type);
int prte_dt_compare_node(prte_node_t *value1, prte_node_t *value2, prte_data_type_t type);
int prte_dt_compare_proc(prte_proc_t *value1, prte_proc_t *value2, prte_data_type_t type);
int prte_dt_compare_app_context(prte_app_context_t *value1, prte_app_context_t *value2, prte_data_type_t type);
int prte_dt_compare_exit_code(prte_exit_code_t *value1,
                                    prte_exit_code_t *value2,
                                    prte_data_type_t type);
int prte_dt_compare_node_state(prte_node_state_t *value1,
                                     prte_node_state_t *value2,
                                     prte_node_state_t type);
int prte_dt_compare_proc_state(prte_proc_state_t *value1,
                                     prte_proc_state_t *value2,
                                     prte_proc_state_t type);
int prte_dt_compare_job_state(prte_job_state_t *value1,
                                    prte_job_state_t *value2,
                                    prte_job_state_t type);
int prte_dt_compare_map(prte_job_map_t *value1, prte_job_map_t *value2, prte_data_type_t type);
int prte_dt_compare_tags(prte_rml_tag_t *value1,
                         prte_rml_tag_t *value2,
                         prte_data_type_t type);
int prte_dt_compare_daemon_cmd(prte_daemon_cmd_flag_t *value1, prte_daemon_cmd_flag_t *value2, prte_data_type_t type);
int prte_dt_compare_iof_tag(prte_iof_tag_t *value1, prte_iof_tag_t *value2, prte_data_type_t type);
int prte_dt_compare_attr(prte_attribute_t *value1, prte_attribute_t *value2, prte_data_type_t type);
int prte_dt_compare_sig(prte_grpcomm_signature_t *value1, prte_grpcomm_signature_t *value2, prte_data_type_t type);

/** Data type copy functions */
int prte_dt_copy_std_cntr(prte_std_cntr_t **dest, prte_std_cntr_t *src, prte_data_type_t type);
int prte_dt_copy_job(prte_job_t **dest, prte_job_t *src, prte_data_type_t type);
int prte_dt_copy_node(prte_node_t **dest, prte_node_t *src, prte_data_type_t type);
int prte_dt_copy_proc(prte_proc_t **dest, prte_proc_t *src, prte_data_type_t type);
int prte_dt_copy_app_context(prte_app_context_t **dest, prte_app_context_t *src, prte_data_type_t type);
int prte_dt_copy_proc_state(prte_proc_state_t **dest, prte_proc_state_t *src, prte_data_type_t type);
int prte_dt_copy_job_state(prte_job_state_t **dest, prte_job_state_t *src, prte_data_type_t type);
int prte_dt_copy_node_state(prte_node_state_t **dest, prte_node_state_t *src, prte_data_type_t type);
int prte_dt_copy_exit_code(prte_exit_code_t **dest, prte_exit_code_t *src, prte_data_type_t type);
int prte_dt_copy_map(prte_job_map_t **dest, prte_job_map_t *src, prte_data_type_t type);
int prte_dt_copy_tag(prte_rml_tag_t **dest,
                           prte_rml_tag_t *src,
                           prte_data_type_t type);
int prte_dt_copy_daemon_cmd(prte_daemon_cmd_flag_t **dest, prte_daemon_cmd_flag_t *src, prte_data_type_t type);
int prte_dt_copy_iof_tag(prte_iof_tag_t **dest, prte_iof_tag_t *src, prte_data_type_t type);
int prte_dt_copy_attr(prte_attribute_t **dest, prte_attribute_t *src, prte_data_type_t type);
int prte_dt_copy_sig(prte_grpcomm_signature_t **dest, prte_grpcomm_signature_t *src, prte_data_type_t type);

/** Data type pack functions */
int prte_dt_pack_std_cntr(prte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_job(prte_buffer_t *buffer, const void *src,
                     int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_node(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_proc(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_app_context(prte_buffer_t *buffer, const void *src,
                                   int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_exit_code(prte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_node_state(prte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_proc_state(prte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_job_state(prte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_map(prte_buffer_t *buffer, const void *src,
                             int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_tag(prte_buffer_t *buffer,
                           const void *src,
                           int32_t num_vals,
                           prte_data_type_t type);
int prte_dt_pack_daemon_cmd(prte_buffer_t *buffer, const void *src,
                          int32_t num_vals, prte_data_type_t type);
int prte_dt_pack_iof_tag(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                         prte_data_type_t type);
int prte_dt_pack_attr(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                      prte_data_type_t type);
int prte_dt_pack_sig(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                     prte_data_type_t type);

/** Data type print functions */
int prte_dt_std_print(char **output, char *prefix, void *src, prte_data_type_t type);
int prte_dt_print_job(char **output, char *prefix, prte_job_t *src, prte_data_type_t type);
int prte_dt_print_node(char **output, char *prefix, prte_node_t *src, prte_data_type_t type);
int prte_dt_print_proc(char **output, char *prefix, prte_proc_t *src, prte_data_type_t type);
int prte_dt_print_app_context(char **output, char *prefix, prte_app_context_t *src, prte_data_type_t type);
int prte_dt_print_map(char **output, char *prefix, prte_job_map_t *src, prte_data_type_t type);
int prte_dt_print_attr(char **output, char *prefix, prte_attribute_t *src, prte_data_type_t type);
int prte_dt_print_sig(char **output, char *prefix, prte_grpcomm_signature_t *src, prte_data_type_t type);

/** Data type unpack functions */
int prte_dt_unpack_std_cntr(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_job(prte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_node(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_proc(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_app_context(prte_buffer_t *buffer, void *dest,
                                     int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_exit_code(prte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_node_state(prte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_proc_state(prte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_job_state(prte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_map(prte_buffer_t *buffer, void *dest,
                               int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_tag(prte_buffer_t *buffer,
                             void *dest,
                             int32_t *num_vals,
                             prte_data_type_t type);
int prte_dt_unpack_daemon_cmd(prte_buffer_t *buffer, void *dest,
                            int32_t *num_vals, prte_data_type_t type);
int prte_dt_unpack_iof_tag(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                           prte_data_type_t type);
int prte_dt_unpack_attr(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                        prte_data_type_t type);
int prte_dt_unpack_sig(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                       prte_data_type_t type);

END_C_DECLS

#endif
