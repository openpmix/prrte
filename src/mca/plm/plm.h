/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The Process Lifecycle Management (PLM) subsystem serves as the central
 * switchyard for all process management activities, including
 * resource allocation, process mapping, process launch, and process
 * monitoring.
 */

#ifndef PRRTE_PLM_H
#define PRRTE_PLM_H

/*
 * includes
 */

#include "prrte_config.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/dss/dss_types.h"
#include "src/class/prrte_pointer_array.h"

#include "src/runtime/prrte_globals.h"

#include "plm_types.h"

BEGIN_C_DECLS

/*
 * Component functions - all MUST be provided
 */

/*
 * allow the selected module to initialize
 */
typedef int (*prrte_plm_base_module_init_fn_t)(void);

/*
 * Spawn a job - this is a non-blocking function!
 */
typedef int (*prrte_plm_base_module_spawn_fn_t)(prrte_job_t *jdata);

/*
 * Remote spawn - spawn called by a daemon to launch a process on its own
 */
typedef int (*prrte_plm_base_module_remote_spawn_fn_t)(void);

/*
 * Entry point to set the HNP name
 */
typedef int (*prrte_plm_base_module_set_hnp_name_fn_t)(void);

/**
    * Cleanup resources held by module.
 */

typedef int (*prrte_plm_base_module_finalize_fn_t)(void);

/**
 * Terminate any processes launched for the respective jobid by
 * this component.
 */
typedef int (*prrte_plm_base_module_terminate_job_fn_t)(prrte_jobid_t);

/**
 * Terminate the daemons
 */
typedef int (*prrte_plm_base_module_terminate_orteds_fn_t)(void);

/**
 * Terminate an array of specific procs
 */
typedef int (*prrte_plm_base_module_terminate_procs_fn_t)(prrte_pointer_array_t *procs);

    /**
 * Signal any processes launched for the respective jobid by
 * this component.
 */
typedef int (*prrte_plm_base_module_signal_job_fn_t)(prrte_jobid_t, int32_t);

/**
 * plm module version 1.0.0
 */
struct prrte_plm_base_module_1_0_0_t {
    prrte_plm_base_module_init_fn_t               init;
    prrte_plm_base_module_set_hnp_name_fn_t       set_hnp_name;
    prrte_plm_base_module_spawn_fn_t              spawn;
    prrte_plm_base_module_remote_spawn_fn_t       remote_spawn;
    prrte_plm_base_module_terminate_job_fn_t      terminate_job;
    prrte_plm_base_module_terminate_orteds_fn_t   terminate_orteds;
    prrte_plm_base_module_terminate_procs_fn_t    terminate_procs;
    prrte_plm_base_module_signal_job_fn_t         signal_job;
    prrte_plm_base_module_finalize_fn_t           finalize;
};

/** shprrten prrte_plm_base_module_1_0_0_t declaration */
typedef struct prrte_plm_base_module_1_0_0_t prrte_plm_base_module_1_0_0_t;
/** shprrten prrte_plm_base_module_t declaration */
typedef struct prrte_plm_base_module_1_0_0_t prrte_plm_base_module_t;


/**
 * plm component
 */
struct prrte_plm_base_component_2_0_0_t {
    /** component version */
    prrte_mca_base_component_t base_version;
    /** component data */
    prrte_mca_base_component_data_t base_data;
};
/** Convenience typedef */
typedef struct prrte_plm_base_component_2_0_0_t prrte_plm_base_component_2_0_0_t;
/** Convenience typedef */
typedef prrte_plm_base_component_2_0_0_t prrte_plm_base_component_t;


/**
 * Macro for use in modules that are of type plm
 */
#define PRRTE_PLM_BASE_VERSION_2_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("plm", 2, 0, 0)

/* Global structure for accessing PLM functions */
PRRTE_EXPORT extern prrte_plm_base_module_t prrte_plm;  /* holds selected module's function pointers */

END_C_DECLS

#endif
