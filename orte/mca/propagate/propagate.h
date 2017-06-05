/*
 * Copyright (c) 2017-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The OpenRTE propagator
 *
 * The OpenRTE Propagator framework provides error propagating services
 * through daemons.
 */

#ifndef MCA_PROPAGATE_H
#define MCA_PROPAGATE_H

/*
 * includes
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"

#include "orte/mca/mca.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_bitmap.h"
#include "opal/dss/dss_types.h"
#include "orte/mca/state/state.h"
#include "orte/mca/rml/rml_types.h"

BEGIN_C_DECLS

/* define a callback function to be invoked upon
 * collective completion */
typedef void (*orte_propagate_cbfunc_t)(int status, opal_buffer_t *buf, void *cbdata);

typedef int (*orte_propagate_rbcast_cb_t)(opal_buffer_t* buffer);

/*
 * Component functions - all MUST be provided!
 */

/* initialize the selected module */
typedef int (*orte_propagate_base_module_init_fn_t)(void);

/* finalize the selected module */
typedef int (*orte_propagate_base_module_finalize_fn_t)(void);

typedef int (*orte_propagate_base_module_prp_fn_t)(orte_jobid_t *job,
        orte_process_name_t *source,
        orte_process_name_t *sickproc,
        orte_proc_state_t state);

typedef int (*orte_propagate_base_module_registercb_fn_t)(void);

/*
 * Module Structure
 */
struct orte_propagate_base_module_2_3_0_t {
    orte_propagate_base_module_init_fn_t          init;
    orte_propagate_base_module_finalize_fn_t      finalize;
    orte_propagate_base_module_prp_fn_t           prp;
    orte_propagate_base_module_registercb_fn_t    register_cb;
};

typedef struct orte_propagate_base_module_2_3_0_t orte_propagate_base_module_2_3_0_t;
typedef orte_propagate_base_module_2_3_0_t orte_propagate_base_module_t;

ORTE_DECLSPEC extern orte_propagate_base_module_t orte_propagate;

/*
 *Propagate Component
 */
struct orte_propagate_base_component_3_0_0_t {
    /** MCA base component */
    mca_base_component_t base_version;
    /** MCA base data */
    mca_base_component_data_t base_data;

    /** Verbosity Level */
    int verbose;
    /** Output Handle for opal_output */
    int output_handle;
    /** Default Priority */
    int priority;
};

typedef struct orte_propagate_base_component_3_0_0_t orte_propagate_base_component_3_0_0_t;
typedef orte_propagate_base_component_3_0_0_t orte_propagate_base_component_t;

/*
 * Macro for use in components that are of type propagate v3.0.0
 */
#define ORTE_PROPAGATE_BASE_VERSION_3_0_0 \
    /* propagate v3.0 is chained to MCA v2.0 */ \
    ORTE_MCA_BASE_VERSION_2_1_0("propagate", 3, 0, 0)

END_C_DECLS

#endif
