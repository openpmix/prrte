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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef prrte_rml_OOB_RML_OOB_H
#define prrte_rml_OOB_RML_OOB_H

#include "prrte_config.h"

#include "src/dss/dss_types.h"
#include "src/event/event-internal.h"

#include "src/mca/oob/oob.h"

#include "src/mca/rml/base/base.h"

BEGIN_C_DECLS

typedef struct {
    prrte_rml_base_module_t  api;
    prrte_list_t             queued_routing_messages;
    prrte_event_t            *timer_event;
    struct timeval          timeout;
    char                    *routed; // name of routed module to be used
} prrte_rml_oob_module_t;

PRRTE_MODULE_EXPORT extern prrte_rml_component_t prrte_rml_oob_component;

void prrte_rml_oob_fini(struct prrte_rml_base_module_t *mod);

int prrte_rml_oob_send_nb(prrte_process_name_t* peer,
                         struct iovec* msg,
                         int count,
                         prrte_rml_tag_t tag,
                         prrte_rml_callback_fn_t cbfunc,
                         void* cbdata);

int prrte_rml_oob_send_buffer_nb(prrte_process_name_t* peer,
                                prrte_buffer_t* buffer,
                                prrte_rml_tag_t tag,
                                prrte_rml_buffer_callback_fn_t cbfunc,
                                void* cbdata);

END_C_DECLS

#endif
