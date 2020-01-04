/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * The PRRTE Daemon's Local Launch Subsystem
 *
 */

#ifndef PRRTE_MCA_ODLS_H
#define PRRTE_MCA_ODLS_H

#include "prrte_config.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/class/prrte_pointer_array.h"

#include "src/dss/dss_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/odls/odls_types.h"

BEGIN_C_DECLS

/*
 * odls module functions
 */

/*
 * Construct a buffer for use in adding local processes
 * In order to reuse daemons, we need a way for the HNP to construct a buffer that
 * contains the data needed by the active ODLS component to launch a local process. Since the
 * only one that knows what a particular ODLS component needs is that component, we require an
 * entry point that the HNP can call to get the required buffer. This is constructed
 * for *all* nodes - the individual orteds then parse that data to find the specific launch info
 * for procs on their node
 */
typedef int (*prrte_odls_base_module_get_add_procs_data_fn_t)(prrte_buffer_t *data,
                                                             prrte_jobid_t job);

/**
 * Locally launch the provided processes
 */
typedef int (*prrte_odls_base_module_launch_local_processes_fn_t)(prrte_buffer_t *data);

/**
 * Kill the local processes on this node
 */
typedef int (*prrte_odls_base_module_kill_local_processes_fn_t)(prrte_pointer_array_t *procs);

/**
 * Signal local processes
 */
typedef int (*prrte_odls_base_module_signal_local_process_fn_t)(const prrte_process_name_t *proc,
                                                              int32_t signal);

/**
 * Restart a local process
 */
typedef int (*prrte_odls_base_module_restart_proc_fn_t)(prrte_proc_t *child);

/**
 * pls module version
 */
struct prrte_odls_base_module_1_3_0_t {
    prrte_odls_base_module_get_add_procs_data_fn_t           get_add_procs_data;
    prrte_odls_base_module_launch_local_processes_fn_t       launch_local_procs;
    prrte_odls_base_module_kill_local_processes_fn_t         kill_local_procs;
    prrte_odls_base_module_signal_local_process_fn_t         signal_local_procs;
    prrte_odls_base_module_restart_proc_fn_t                 restart_proc;
};

/** shprrten prrte_odls_base_module_1_3_0_t declaration */
typedef struct prrte_odls_base_module_1_3_0_t prrte_odls_base_module_1_3_0_t;
/** shprrten prrte_odls_base_module_t declaration */
typedef struct prrte_odls_base_module_1_3_0_t prrte_odls_base_module_t;

/**
 * odls component
 */
struct prrte_odls_base_component_2_0_0_t {
    /** component version */
    prrte_mca_base_component_t version;
    /** component data */
    prrte_mca_base_component_data_t base_data;
};
/** Convenience typedef */
typedef struct prrte_odls_base_component_2_0_0_t prrte_odls_base_component_2_0_0_t;
/** Convenience typedef */
typedef prrte_odls_base_component_2_0_0_t prrte_odls_base_component_t;


/**
 * Macro for use in modules that are of type odls
 */
#define PRRTE_ODLS_BASE_VERSION_2_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("odls", 2, 0, 0)

/* Global structure for accessing ODLS functions
*/
PRRTE_EXPORT extern prrte_odls_base_module_t prrte_odls;  /* holds selected module's function pointers */

END_C_DECLS

#endif /* MCA_ODLS_H */
