/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      NVIDIA Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The Open RTE Error and Recovery Manager (ErrMgr)
 *
 * This framework is the logically central clearing house for process/daemon
 * state updates. In particular when a process fails and another process detects
 * it, then that information is reported through this framework. This framework
 * then (depending on the active component) decides how to handle the failure.
 *
 * For example, if a process fails this may activate an automatic recovery
 * of the process from a previous checkpoint, or initial state. Conversely,
 * the active component could decide not to continue the job, and request that
 * it be terminated. The error and recovery policy is determined by individual
 * components within this framework.
 *
 */

#ifndef PRRTE_MCA_ERRMGR_H
#define PRRTE_MCA_ERRMGR_H

/*
 * includes
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/class/prrte_object.h"
#include "src/class/prrte_pointer_array.h"
#include "src/util/output.h"
#include "src/util/error.h"

#include "src/runtime/prrte_globals.h"
#include "src/mca/plm/plm_types.h"

BEGIN_C_DECLS

/*
 * Macro definitions
 */
/*
 * Thess macros and associated error name array are used to output intelligible error
 * messages.
 */

#define PRRTE_ERROR_NAME(n)  prrte_strerror(n)

/*
 * Framework Interfaces
 */
/**
 * Module initialization function.
 *
 * @retval PRRTE_SUCCESS The operation completed successfully
 * @retval PRRTE_ERROR   An unspecifed error occurred
 */
typedef int (*prrte_errmgr_base_module_init_fn_t)(void);

/**
 * Module finalization function.
 *
 * @retval PRRTE_SUCCESS The operation completed successfully
 * @retval PRRTE_ERROR   An unspecifed error occurred
 */
typedef int (*prrte_errmgr_base_module_finalize_fn_t)(void);

/**
 * This is not part of any module so it can be used at any time!
 */
typedef void (*prrte_errmgr_base_module_log_fn_t)(int error_code, char *filename, int line);

/**
 * Alert - self aborting
 * This function is called when a process is aborting due to some internal error.
 * It will finalize the process
 * itself, and then exit - it takes no other actions. The intent here is to provide
 * a last-ditch exit procedure that attempts to clean up a little.
 */
typedef void (*prrte_errmgr_base_module_abort_fn_t)(int error_code, char *fmt, ...)
__prrte_attribute_format_funcptr__(__printf__, 2, 3);

/**
 * Alert - abort peers
 *  This function is called when a process wants to abort one or more peer processes.
 *  For example, MPI_Abort(comm) will use this function to terminate peers in the
 *  communicator group before aborting itself.
 */
typedef int (*prrte_errmgr_base_module_abort_peers_fn_t)(prrte_process_name_t *procs,
                                                        prrte_std_cntr_t num_procs,
                                                        int error_code);

/*
 * Module Structure
 */
struct prrte_errmgr_base_module_2_3_0_t {
    /** Initialization Function */
    prrte_errmgr_base_module_init_fn_t                       init;
    /** Finalization Function */
    prrte_errmgr_base_module_finalize_fn_t                   finalize;

    prrte_errmgr_base_module_log_fn_t                        logfn;
    prrte_errmgr_base_module_abort_fn_t                      abort;
    prrte_errmgr_base_module_abort_peers_fn_t                abort_peers;
};
typedef struct prrte_errmgr_base_module_2_3_0_t prrte_errmgr_base_module_2_3_0_t;
typedef prrte_errmgr_base_module_2_3_0_t prrte_errmgr_base_module_t;
PRRTE_EXPORT extern prrte_errmgr_base_module_t prrte_errmgr;

/*
 * ErrMgr Component
 */
struct prrte_errmgr_base_component_3_0_0_t {
    /** MCA base component */
    prrte_mca_base_component_t base_version;
    /** MCA base data */
    prrte_mca_base_component_data_t base_data;

    /** Verbosity Level */
    int verbose;
    /** Output Handle for prrte_output */
    int output_handle;
    /** Default Priority */
    int priority;
};
typedef struct prrte_errmgr_base_component_3_0_0_t prrte_errmgr_base_component_3_0_0_t;
typedef prrte_errmgr_base_component_3_0_0_t prrte_errmgr_base_component_t;

/*
 * Macro for use in components that are of type errmgr
 */
#define PRRTE_ERRMGR_BASE_VERSION_3_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("errmgr", 3, 0, 0)

END_C_DECLS

#endif
