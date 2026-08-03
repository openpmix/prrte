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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef PRTE_FILEM_BASE_H
#define PRTE_FILEM_BASE_H

#include "prte_config.h"

#include "src/mca/filem/filem.h"
#include "src/rml/rml.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_printf.h"

BEGIN_C_DECLS

/*
 * MCA framework
 */
PRTE_EXPORT extern pmix_mca_base_framework_t prte_filem_base_framework;
/*
 * Select an available component.
 */
PRTE_EXPORT int prte_filem_base_select(void);

/**
 * Globals
 */
PRTE_EXPORT extern prte_filem_base_module_t prte_filem;

/**
 * 'None' component functions
 * These are to be used when no component is selected.
 * They just return success, and empty strings as necessary.
 */
int prte_filem_base_module_init(void);
int prte_filem_base_module_finalize(void);

void prte_filem_base_none_fault_handler(const prte_rml_recovery_status_t *status);
PRTE_EXPORT int prte_filem_base_none_put(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_put_nb(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_get(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_get_nb(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_rm(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_rm_nb(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_wait(prte_filem_base_request_t *request);
PRTE_EXPORT int prte_filem_base_none_wait_all(pmix_list_t *request_list);
int prte_filem_base_none_preposition_files(prte_job_t *jdata, prte_filem_completion_cbfunc_t cbfunc,
                                           void *cbdata);
int prte_filem_base_none_link_local_files(prte_job_t *jdata, prte_app_context_t *app);

END_C_DECLS

#endif /* PRTE_FILEM_BASE_H */
