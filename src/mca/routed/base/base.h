/*
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_ROUTED_BASE_H
#define MCA_ROUTED_BASE_H

#include "prrte_config.h"

#include "src/mca/mca.h"

#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss_types.h"

#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_routed_base_framework;
/* select a component */
PRRTE_EXPORT    int prrte_routed_base_select(void);

typedef struct {
    bool routing_enabled;
} prrte_routed_base_t;
PRRTE_EXPORT extern prrte_routed_base_t prrte_routed_base;


/* specialized support functions */
PRRTE_EXPORT void prrte_routed_base_xcast_routing(prrte_list_t *coll,
                                                  prrte_list_t *my_children);

PRRTE_EXPORT int prrte_routed_base_process_callback(prrte_jobid_t job,
                                                    prrte_buffer_t *buffer);
PRRTE_EXPORT void prrte_routed_base_update_hnps(prrte_buffer_t *buf);

END_C_DECLS

#endif /* MCA_ROUTED_BASE_H */
