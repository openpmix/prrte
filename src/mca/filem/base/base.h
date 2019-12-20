/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University and Indiana
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
 *                         All rights reserved
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef PRRTE_FILEM_BASE_H
#define PRRTE_FILEM_BASE_H

#include "prrte_config.h"

#include "src/util/printf.h"
#include "src/mca/rml/rml.h"

#include "src/mca/filem/filem.h"

BEGIN_C_DECLS

/*
 * MCA framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_filem_base_framework;
/*
 * Select an available component.
 */
PRRTE_EXPORT int prrte_filem_base_select(void);

/*
 * cmds for base receive
 */
typedef uint8_t prrte_filem_cmd_flag_t;
#define PRRTE_FILEM_CMD  PRRTE_UINT8
#define PRRTE_FILEM_GET_PROC_NODE_NAME_CMD  1
#define PRRTE_FILEM_GET_REMOTE_PATH_CMD     2

/**
 * Globals
 */
PRRTE_EXPORT extern prrte_filem_base_module_t prrte_filem;
PRRTE_EXPORT extern bool prrte_filem_base_is_active;

/**
 * 'None' component functions
 * These are to be used when no component is selected.
 * They just return success, and empty strings as necessary.
 */
int prrte_filem_base_module_init(void);
int prrte_filem_base_module_finalize(void);

PRRTE_EXPORT int prrte_filem_base_none_put(prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_put_nb(prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_get(prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_get_nb(prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_rm( prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_rm_nb( prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_wait( prrte_filem_base_request_t *request);
PRRTE_EXPORT int prrte_filem_base_none_wait_all( prrte_list_t *request_list);
int prrte_filem_base_none_preposition_files(prrte_job_t *jdata,
                                           prrte_filem_completion_cbfunc_t cbfunc,
                                           void *cbdata);
int prrte_filem_base_none_link_local_files(prrte_job_t *jdata,
                                          prrte_app_context_t *app);

/**
 * Some utility functions
 */
/* base comm functions */
PRRTE_EXPORT int prrte_filem_base_comm_start(void);
PRRTE_EXPORT int prrte_filem_base_comm_stop(void);
PRRTE_EXPORT void prrte_filem_base_recv(int status, prrte_process_name_t* sender,
                                        prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                                        void* cbdata);


END_C_DECLS

#endif /* PRRTE_FILEM_BASE_H */
