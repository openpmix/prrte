/*
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_ROUTED_BASE_H
#define MCA_ROUTED_BASE_H

#include "prte_config.h"

#include "src/mca/mca.h"

#include "src/class/prte_pointer_array.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRTE_EXPORT extern prte_mca_base_framework_t prte_routed_base_framework;
/* select a component */
PRTE_EXPORT int prte_routed_base_select(void);

typedef struct {
    bool routing_enabled;
} prte_routed_base_t;
PRTE_EXPORT extern prte_routed_base_t prte_routed_base;

/* specialized support functions */
PRTE_EXPORT void prte_routed_base_xcast_routing(prte_list_t *coll, prte_list_t *my_children);

PRTE_EXPORT int prte_routed_base_process_callback(pmix_nspace_t job, pmix_data_buffer_t *buffer);
PRTE_EXPORT void prte_routed_base_update_hnps(pmix_data_buffer_t *buf);

END_C_DECLS

#endif /* MCA_ROUTED_BASE_H */
