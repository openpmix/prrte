/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The PRTE propagator
 *
 * The PRTE Propagator framework provides error propagating services
 * through daemons.
 */

#ifndef MCA_PROPAGATE_H
#define MCA_PROPAGATE_H

/*
 * includes
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_object.h"
#include "src/mca/mca.h"
#include "src/rml/rml_types.h"
#include "src/mca/state/state.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/* define a callback function to be invoked upon
 * collective completion */
typedef void (*prte_propagate_cbfunc_t)(int status, pmix_data_buffer_t *buf, void *cbdata);

typedef int (*prte_propagate_rbcast_cb_t)(pmix_data_buffer_t *buffer);

/*
 * Component functions - all MUST be provided!
 */

/* initialize the selected module */
typedef int (*prte_propagate_base_module_init_fn_t)(void);

/* finalize the selected module */
typedef int (*prte_propagate_base_module_finalize_fn_t)(void);

typedef int (*prte_propagate_base_module_prp_fn_t)(const pmix_nspace_t job,
                                                   const pmix_proc_t *source,
                                                   const pmix_proc_t *sickproc,
                                                   prte_proc_state_t state);

typedef int (*prte_propagate_base_module_registercb_fn_t)(void);

/*
 * Module Structure
 */
struct prte_propagate_base_module_2_3_0_t {
    prte_propagate_base_module_init_fn_t init;
    prte_propagate_base_module_finalize_fn_t finalize;
    prte_propagate_base_module_prp_fn_t prp;
    prte_propagate_base_module_registercb_fn_t register_cb;
};

typedef struct prte_propagate_base_module_2_3_0_t prte_propagate_base_module_2_3_0_t;
typedef prte_propagate_base_module_2_3_0_t prte_propagate_base_module_t;

PRTE_EXPORT extern prte_propagate_base_module_t prte_propagate;

/*
 *Propagate Component
 */
struct prte_propagate_base_component_3_0_0_t {
    /** MCA base component */
    prte_mca_base_component_t base_version;
    /** MCA base data */
    prte_mca_base_component_data_t base_data;

    /** Verbosity Level */
    int verbose;
    /** Output Handle for prte_output */
    int output_handle;
    /** Default Priority */
    int priority;
};

typedef struct prte_propagate_base_component_3_0_0_t prte_propagate_base_component_3_0_0_t;
typedef prte_propagate_base_component_3_0_0_t prte_propagate_base_component_t;

/*
 * Macro for use in components that are of type propagate v3.0.0
 */
#define PRTE_PROPAGATE_BASE_VERSION_3_0_0       \
    /* propagate v3.0 is chained to MCA v2.0 */ \
    PRTE_MCA_BASE_VERSION_2_1_0("propagate", 3, 0, 0)

END_C_DECLS

#endif
