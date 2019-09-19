/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/****    PRRTE STATE MACHINE    ****/

/* States are treated as events so that the event
 * library can sequence them. Each state consists
 * of an event, a job or process state, a pointer
 * to the respective object, and a callback function
 * to be executed for that state. Events can be defined
 * at different priorities - e.g., SYS priority for
 * events associated with launching jobs, and ERR priority
 * for events associated with abnormal termination of
 * a process.
 *
 * The state machine consists of a list of state objects,
 * each defining a state-cbfunc pair. At startup, a default
 * list is created by the base functions which is then
 * potentially customized by selected components within
 * the various PRRTE frameworks. For example, a PLM component
 * may need to insert states in the launch procedure, or may
 * want to redirect a particular state callback to a custom
 * function.
 *
 * For convenience, an ANY state can be defined along with a generic
 * callback function, with the corresponding state object
 * placed at the end of the state machine. Setting the
 * machine to a state that has not been explicitly defined
 * will cause this default action to be executed. Thus, you
 * don't have to explicitly define a state-cbfunc pair
 * for every job or process state.
 */

#ifndef _PRRTE_STATE_H_
#define _PRRTE_STATE_H_

#include "prrte_config.h"

#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/plm/plm_types.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/state/state_types.h"

BEGIN_C_DECLS

/*
 * MCA Framework - put here to access the prrte_output channel
 * in the macros
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_state_base_framework;

/* For ease in debugging the state machine, it is STRONGLY recommended
 * that the functions be accessed using the following macros
 */
#define PRRTE_FORCED_TERMINATE(x)                                                    \
    do {                                                                            \
        if (!prrte_abnormal_term_ordered) {                                          \
            prrte_output(0, "FORCE TERMINATE ORDERED AT %s:%d - error %s(%d)",       \
                        __FILE__, __LINE__, PRRTE_ERROR_NAME((x)), (x));             \
        } \
    } while(0);

#define PRRTE_ACTIVATE_JOB_STATE(j, s)                                       \
    do {                                                                    \
        prrte_job_t *shadow=(j);                                             \
        prrte_output_verbose(1, prrte_state_base_framework.framework_output,  \
                            "%s ACTIVATE JOB %s STATE %s AT %s:%d",         \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),             \
                            (NULL == shadow) ? "NULL" :                     \
                            PRRTE_JOBID_PRINT(shadow->jobid),                \
                            prrte_job_state_to_str((s)),                     \
                            __FILE__, __LINE__);                            \
        prrte_state.activate_job_state(shadow, (s));                         \
    } while(0);

#define PRRTE_ACTIVATE_PROC_STATE(p, s)                                      \
    do {                                                                    \
        prrte_process_name_t *shadow=(p);                                    \
        prrte_output_verbose(1, prrte_state_base_framework.framework_output,  \
                            "%s ACTIVATE PROC %s STATE %s AT %s:%d",        \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),             \
                            (NULL == shadow) ? "NULL" :                     \
                            PRRTE_NAME_PRINT(shadow),                        \
                            prrte_proc_state_to_str((s)),                    \
                            __FILE__, __LINE__);                            \
        prrte_state.activate_proc_state(shadow, (s));                        \
    } while(0);

/**
 * Module initialization function.
 *
 * @retval PRRTE_SUCCESS The operation completed successfully
 * @retval PRRTE_ERROR   An unspecifed error occurred
 */
typedef int (*prrte_state_base_module_init_fn_t)(void);

/**
 * Module finalization function.
 *
 * @retval PRRTE_SUCCESS The operation completed successfully
 * @retval PRRTE_ERROR   An unspecifed error occurred
 */
typedef int (*prrte_state_base_module_finalize_fn_t)(void);

/****    JOB STATE APIs    ****/
/* Job states are accessed via prrte_job_t objects as they are only
 * used in PRRTE tools and not application processes. APIs are provided
 * for assembling and editing the state machine, as well as activating
 * a specific job state
 *
 * Note the inherent assumption in this design that any customization
 * of the state machine will at least start with the base states - i.e.,
 * that one would start with the default machine and edit it to add,
 * remove, or modify callbacks as required. Alternatively, one could
 * just clear the list entirely and assemble a fully custom state
 * machine - both models are supported.
 */

/* Activate a state in the job state machine.
 *
 * Creates and activates an event with the callback corresponding to the
 * specified job state. If the specified state is not found:
 *
 * 1. if a state machine entry for PRRTE_JOB_STATE_ERROR was given, and
 *    the state is an error state (i.e., PRRTE_JOB_STATE_ERROR <= state),
 *    then the callback for the ERROR state will be used
 *
 * 2. if a state machine entry for PRRTE_JOB_STATE_ANY was given, and
 *    the state is not an error state (i.e., state < PRRTE_JOB_STATE_ERROR),
 *    then the callback for the ANY state will be used
 *
 * 3. if neither of the above is true, then the call will be ignored.
 */
typedef void (*prrte_state_base_module_activate_job_state_fn_t)(prrte_job_t *jdata,
                                                                prrte_job_state_t state);

/* Add a state to the job state machine.
 *
 */
typedef int (*prrte_state_base_module_add_job_state_fn_t)(prrte_job_state_t state,
                                                          prrte_state_cbfunc_t cbfunc,
                                                          int priority);

/* Set the callback function for a state in the job state machine.
 *
 */
typedef int (*prrte_state_base_module_set_job_state_callback_fn_t)(prrte_job_state_t state,
                                                                   prrte_state_cbfunc_t cbfunc);

/* Set the event priority for a state in the job state machine.
 *
 */
typedef int (*prrte_state_base_module_set_job_state_priority_fn_t)(prrte_job_state_t state,
                                                                   int priority);

/* Remove a state from the job state machine.
 *
 */
typedef int (*prrte_state_base_module_remove_job_state_fn_t)(prrte_job_state_t state);


/****    Proc STATE APIs  ****/
/* Proc states are accessed via prrte_process_name_t as the state machine
 * must be available to both application processes and PRRTE tools. APIs are
 * providedfor assembling and editing the state machine, as well as activating
 * a specific proc state
 *
 * Note the inherent assumption in this design that any customization
 * of the state machine will at least start with the base states - i.e.,
 * that one would start with the default machine and edit it to add,
 * remove, or modify callbacks as required. Alternatively, one could
 * just clear the list entirely and assemble a fully custom state
 * machine - both models are supported.
 */

/* Activate a proc state.
 *
 * Creates and activates an event with the callback corresponding to the
 * specified proc state. If the specified state is not found:
 *
 * 1. if a state machine entry for PRRTE_PROC_STATE_ERROR was given, and
 *    the state is an error state (i.e., PRRTE_PROC_STATE_ERROR <= state),
 *    then the callback for the ERROR state will be used
 *
 * 2. if a state machine entry for PRRTE_PROC_STATE_ANY was given, and
 *    the state is not an error state (i.e., state < PRRTE_PROC_STATE_ERROR),
 *    then the callback for the ANY state will be used
 *
 * 3. if neither of the above is true, then the call will be ignored.
 */
typedef void (*prrte_state_base_module_activate_proc_state_fn_t)(prrte_process_name_t *proc,
                                                                 prrte_proc_state_t state);

/* Add a state to the proc state machine.
 *
 */
typedef int (*prrte_state_base_module_add_proc_state_fn_t)(prrte_proc_state_t state,
                                                           prrte_state_cbfunc_t cbfunc,
                                                           int priority);

/* Set the callback function for a state in the proc state machine.
 *
 */
typedef int (*prrte_state_base_module_set_proc_state_callback_fn_t)(prrte_proc_state_t state,
                                                                    prrte_state_cbfunc_t cbfunc);

/* Set the event priority for a state in the proc state machine.
 *
 */
typedef int (*prrte_state_base_module_set_proc_state_priority_fn_t)(prrte_proc_state_t state,
                                                                    int priority);

/* Remove a state from the proc state machine.
 *
 */
typedef int (*prrte_state_base_module_remove_proc_state_fn_t)(prrte_proc_state_t state);


/*
 * Module Structure
 */
struct prrte_state_base_module_1_0_0_t {
    /** Initialization Function */
    prrte_state_base_module_init_fn_t                      init;
    /** Finalization Function */
    prrte_state_base_module_finalize_fn_t                  finalize;
    /* Job state APIs */
    prrte_state_base_module_activate_job_state_fn_t        activate_job_state;
    prrte_state_base_module_add_job_state_fn_t             add_job_state;
    prrte_state_base_module_set_job_state_callback_fn_t    set_job_state_callback;
    prrte_state_base_module_set_job_state_priority_fn_t    set_job_state_priority;
    prrte_state_base_module_remove_job_state_fn_t          remove_job_state;
    /* Proc state APIs */
    prrte_state_base_module_activate_proc_state_fn_t       activate_proc_state;
    prrte_state_base_module_add_proc_state_fn_t            add_proc_state;
    prrte_state_base_module_set_proc_state_callback_fn_t   set_proc_state_callback;
    prrte_state_base_module_set_proc_state_priority_fn_t   set_proc_state_priority;
    prrte_state_base_module_remove_proc_state_fn_t         remove_proc_state;
};
typedef struct prrte_state_base_module_1_0_0_t prrte_state_base_module_1_0_0_t;
typedef prrte_state_base_module_1_0_0_t prrte_state_base_module_t;
PRRTE_EXPORT extern prrte_state_base_module_t prrte_state;

/*
 * State Component
 */
struct prrte_state_base_component_1_0_0_t {
    /** MCA base component */
    prrte_mca_base_component_t base_version;
    /** MCA base data */
    prrte_mca_base_component_data_t base_data;
};
typedef struct prrte_state_base_component_1_0_0_t prrte_state_base_component_1_0_0_t;
typedef prrte_state_base_component_1_0_0_t prrte_state_base_component_t;

/*
 * Macro for use in components that are of type state
 */
#define PRRTE_STATE_BASE_VERSION_1_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("state", 1, 0, 0)

END_C_DECLS
#endif
