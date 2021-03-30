/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef prte_rml_OOB_RML_OOB_H
#define prte_rml_OOB_RML_OOB_H

#include "prte_config.h"

#include "src/event/event-internal.h"
#include "src/mca/oob/oob.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/rml/base/base.h"

BEGIN_C_DECLS

typedef struct {
    prte_rml_base_module_t api;
    prte_list_t queued_routing_messages;
    prte_event_t *timer_event;
    struct timeval timeout;
    char *routed; // name of routed module to be used
} prte_rml_oob_module_t;

PRTE_MODULE_EXPORT extern prte_rml_component_t prte_rml_oob_component;

void prte_rml_oob_fini(struct prte_rml_base_module_t *mod);

int prte_rml_oob_send_buffer_nb(pmix_proc_t *peer, pmix_data_buffer_t *buffer, prte_rml_tag_t tag,
                                prte_rml_buffer_callback_fn_t cbfunc, void *cbdata);

END_C_DECLS

#endif
