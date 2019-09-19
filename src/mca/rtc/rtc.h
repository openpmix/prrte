/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The Open RTE Run-Time Control Framework (RTC)
 *
 */

#ifndef PRRTE_MCA_RTC_H
#define PRRTE_MCA_RTC_H

#include "prrte_config.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/class/prrte_list.h"

#include "src/runtime/prrte_globals.h"

BEGIN_C_DECLS

typedef struct {
    prrte_list_item_t super;
    char *component;
    char *category;
    prrte_value_t control;
} prrte_rtc_resource_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_rtc_resource_t);

/* Assign run-time controls for a given job. This provides each component with
 * an opportunity to insert attributes into the prrte_job_t and/or its
 * associated proc structures that will be passed to backend daemons for
 * controlling the job. For example, if the user specified a frequency
 * setting for the job, then the freq component will have an opportunity
 * to add an attribute to the job so the freq component on the remote daemons
 * can "catch" it and perform the desired action
 */
typedef void (*prrte_rtc_base_module_assign_fn_t)(prrte_job_t *jdata);

/* Set run-time controls for a given job and/or process. This can include
 * controls for power, binding, memory, and any other resource on the node.
 * Each active plugin will be given a chance to operate on the request, setting
 * whatever controls that lie within its purview.
 *
 * Each module is responsible for reporting errors via the state machine. Thus,
 * no error code is returned. However, warnings and error messages for the user
 * can be output via the provided error_fd */
typedef void (*prrte_rtc_base_module_set_fn_t)(prrte_job_t *jdata,
                                              prrte_proc_t *proc,
                                              char ***env,
                                              int error_fd);

/* Return a list of valid controls values for this component.
 * Each module is responsible for adding its control values
 * to a list of prrte_value_t objects.
 */
typedef void (*prrte_rtc_base_module_get_avail_vals_fn_t)(prrte_list_t *vals);

/* provide a way for the module to init during selection */
typedef int (*prrte_rtc_base_module_init_fn_t)(void);

/* provide a chance for the module to finalize */
typedef void (*prrte_rtc_base_module_fini_fn_t)(void);

/*
 * rtc module version 1.0.0
 */
typedef struct {
    prrte_rtc_base_module_init_fn_t            init;
    prrte_rtc_base_module_fini_fn_t            finalize;
    prrte_rtc_base_module_assign_fn_t          assign;
    prrte_rtc_base_module_set_fn_t             set;
    prrte_rtc_base_module_get_avail_vals_fn_t  get_available_values;
} prrte_rtc_base_module_t;


/* provide a public API version */
typedef struct {
    prrte_rtc_base_module_assign_fn_t          assign;
    prrte_rtc_base_module_set_fn_t             set;
    prrte_rtc_base_module_get_avail_vals_fn_t  get_available_values;
} prrte_rtc_API_module_t;


/**
 * rtc component version 1.0.0
 */
typedef struct prrte_rtc_base_component_1_0_0_t {
    /** Base MCA structure */
    prrte_mca_base_component_t base_version;
    /** Base MCA data */
    prrte_mca_base_component_data_t base_data;
} prrte_rtc_base_component_t;

/* declare the struct containing the public API */
PRRTE_EXPORT extern prrte_rtc_API_module_t prrte_rtc;

/*
 * Macro for use in components that are of type rtc
 */
#define PRRTE_RTC_BASE_VERSION_1_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("rtc", 1, 0, 0)


END_C_DECLS

#endif
