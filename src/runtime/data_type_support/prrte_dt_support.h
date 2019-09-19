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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_DT_SUPPORT_H
#define PRRTE_DT_SUPPORT_H

/*
 * includes
 */
#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include "src/dss/dss_types.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/plm/plm_types.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/iof/iof_types.h"

#include "src/runtime/prrte_globals.h"


BEGIN_C_DECLS

/** Data type compare functions */
int prrte_dt_compare_std_cntr(prrte_std_cntr_t *value1, prrte_std_cntr_t *value2, prrte_data_type_t type);
int prrte_dt_compare_job(prrte_job_t *value1, prrte_job_t *value2, prrte_data_type_t type);
int prrte_dt_compare_node(prrte_node_t *value1, prrte_node_t *value2, prrte_data_type_t type);
int prrte_dt_compare_proc(prrte_proc_t *value1, prrte_proc_t *value2, prrte_data_type_t type);
int prrte_dt_compare_app_context(prrte_app_context_t *value1, prrte_app_context_t *value2, prrte_data_type_t type);
int prrte_dt_compare_exit_code(prrte_exit_code_t *value1,
                                    prrte_exit_code_t *value2,
                                    prrte_data_type_t type);
int prrte_dt_compare_node_state(prrte_node_state_t *value1,
                                     prrte_node_state_t *value2,
                                     prrte_node_state_t type);
int prrte_dt_compare_proc_state(prrte_proc_state_t *value1,
                                     prrte_proc_state_t *value2,
                                     prrte_proc_state_t type);
int prrte_dt_compare_job_state(prrte_job_state_t *value1,
                                    prrte_job_state_t *value2,
                                    prrte_job_state_t type);
int prrte_dt_compare_map(prrte_job_map_t *value1, prrte_job_map_t *value2, prrte_data_type_t type);
int prrte_dt_compare_tags(prrte_rml_tag_t *value1,
                         prrte_rml_tag_t *value2,
                         prrte_data_type_t type);
int prrte_dt_compare_daemon_cmd(prrte_daemon_cmd_flag_t *value1, prrte_daemon_cmd_flag_t *value2, prrte_data_type_t type);
int prrte_dt_compare_iof_tag(prrte_iof_tag_t *value1, prrte_iof_tag_t *value2, prrte_data_type_t type);
int prrte_dt_compare_attr(prrte_attribute_t *value1, prrte_attribute_t *value2, prrte_data_type_t type);
int prrte_dt_compare_sig(prrte_grpcomm_signature_t *value1, prrte_grpcomm_signature_t *value2, prrte_data_type_t type);

/** Data type copy functions */
int prrte_dt_copy_std_cntr(prrte_std_cntr_t **dest, prrte_std_cntr_t *src, prrte_data_type_t type);
int prrte_dt_copy_job(prrte_job_t **dest, prrte_job_t *src, prrte_data_type_t type);
int prrte_dt_copy_node(prrte_node_t **dest, prrte_node_t *src, prrte_data_type_t type);
int prrte_dt_copy_proc(prrte_proc_t **dest, prrte_proc_t *src, prrte_data_type_t type);
int prrte_dt_copy_app_context(prrte_app_context_t **dest, prrte_app_context_t *src, prrte_data_type_t type);
int prrte_dt_copy_proc_state(prrte_proc_state_t **dest, prrte_proc_state_t *src, prrte_data_type_t type);
int prrte_dt_copy_job_state(prrte_job_state_t **dest, prrte_job_state_t *src, prrte_data_type_t type);
int prrte_dt_copy_node_state(prrte_node_state_t **dest, prrte_node_state_t *src, prrte_data_type_t type);
int prrte_dt_copy_exit_code(prrte_exit_code_t **dest, prrte_exit_code_t *src, prrte_data_type_t type);
int prrte_dt_copy_map(prrte_job_map_t **dest, prrte_job_map_t *src, prrte_data_type_t type);
int prrte_dt_copy_tag(prrte_rml_tag_t **dest,
                           prrte_rml_tag_t *src,
                           prrte_data_type_t type);
int prrte_dt_copy_daemon_cmd(prrte_daemon_cmd_flag_t **dest, prrte_daemon_cmd_flag_t *src, prrte_data_type_t type);
int prrte_dt_copy_iof_tag(prrte_iof_tag_t **dest, prrte_iof_tag_t *src, prrte_data_type_t type);
int prrte_dt_copy_attr(prrte_attribute_t **dest, prrte_attribute_t *src, prrte_data_type_t type);
int prrte_dt_copy_sig(prrte_grpcomm_signature_t **dest, prrte_grpcomm_signature_t *src, prrte_data_type_t type);

/** Data type pack functions */
int prrte_dt_pack_std_cntr(prrte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_job(prrte_buffer_t *buffer, const void *src,
                     int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_node(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_proc(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_app_context(prrte_buffer_t *buffer, const void *src,
                                   int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_exit_code(prrte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_node_state(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_proc_state(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_job_state(prrte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_map(prrte_buffer_t *buffer, const void *src,
                             int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_tag(prrte_buffer_t *buffer,
                           const void *src,
                           int32_t num_vals,
                           prrte_data_type_t type);
int prrte_dt_pack_daemon_cmd(prrte_buffer_t *buffer, const void *src,
                          int32_t num_vals, prrte_data_type_t type);
int prrte_dt_pack_iof_tag(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                         prrte_data_type_t type);
int prrte_dt_pack_attr(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                      prrte_data_type_t type);
int prrte_dt_pack_sig(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                     prrte_data_type_t type);

/** Data type print functions */
int prrte_dt_std_print(char **output, char *prefix, void *src, prrte_data_type_t type);
int prrte_dt_print_job(char **output, char *prefix, prrte_job_t *src, prrte_data_type_t type);
int prrte_dt_print_node(char **output, char *prefix, prrte_node_t *src, prrte_data_type_t type);
int prrte_dt_print_proc(char **output, char *prefix, prrte_proc_t *src, prrte_data_type_t type);
int prrte_dt_print_app_context(char **output, char *prefix, prrte_app_context_t *src, prrte_data_type_t type);
int prrte_dt_print_map(char **output, char *prefix, prrte_job_map_t *src, prrte_data_type_t type);
int prrte_dt_print_attr(char **output, char *prefix, prrte_attribute_t *src, prrte_data_type_t type);
int prrte_dt_print_sig(char **output, char *prefix, prrte_grpcomm_signature_t *src, prrte_data_type_t type);

/** Data type unpack functions */
int prrte_dt_unpack_std_cntr(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_job(prrte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_node(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_proc(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_app_context(prrte_buffer_t *buffer, void *dest,
                                     int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_exit_code(prrte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_node_state(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_proc_state(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_job_state(prrte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_map(prrte_buffer_t *buffer, void *dest,
                               int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_tag(prrte_buffer_t *buffer,
                             void *dest,
                             int32_t *num_vals,
                             prrte_data_type_t type);
int prrte_dt_unpack_daemon_cmd(prrte_buffer_t *buffer, void *dest,
                            int32_t *num_vals, prrte_data_type_t type);
int prrte_dt_unpack_iof_tag(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                           prrte_data_type_t type);
int prrte_dt_unpack_attr(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                        prrte_data_type_t type);
int prrte_dt_unpack_sig(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                       prrte_data_type_t type);

END_C_DECLS

#endif
