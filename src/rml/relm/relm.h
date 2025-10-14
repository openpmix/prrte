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
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file:
 *
 * Reliable Messaging (RELM) framework
 */

#ifndef PRTE_RELM_H
#define PRTE_RELM_H

#include "prte_config.h"
#include "constants.h"

#include "src/rml/rml_types.h"

BEGIN_C_DECLS

/* Initialize/finalize selected module */
typedef void (*prte_relm_base_module_init_fn_t)(void);
typedef void (*prte_relm_base_module_finalize_fn_t)(void);

/* Respond to failed daemons */
typedef void (*prte_relm_base_module_fault_handler_fn_t)(
    const prte_rml_recovery_status_t* status
);

/* Reliably send a non-blocking message to a specific destination.
 *
 * @param[in] dst   Name of receiving process
 * @param[in] buf   Pointer to buffer to be sent (takes ownership)
 * @param[in] tag   User defined tag for matching send/recv
 *
 * @retval PRTE_SUCCESS               The message was successfully started
 * @retval PRTE_ERR_BAD_PARAM         One of the parameters was invalid
 * @retval PRTE_ERR_ADDRESSEE_UNKNOWN Contact information for dst is unavailable
 * @retval PRTE_ERR_NODE_DOWN         Provided dst is believed to have failed
 * @retval PRTE_ERROR                 An unspecified error occurred
 */
typedef int (*prte_relm_base_module_reliable_send_fn_t)(
    pmix_rank_t dest, pmix_data_buffer_t* buf, prte_rml_tag_t tag
);

typedef struct {
    prte_relm_base_module_init_fn_t          init;
    prte_relm_base_module_finalize_fn_t      finalize;
    prte_relm_base_module_fault_handler_fn_t fault_handler;
    prte_relm_base_module_reliable_send_fn_t reliable_send;
} prte_relm_module_t;
PRTE_EXPORT extern prte_relm_module_t prte_relm;

PRTE_EXPORT void prte_relm_register(void);
PRTE_EXPORT void prte_relm_open(void);
PRTE_EXPORT void prte_relm_close(void);

END_C_DECLS

#endif
