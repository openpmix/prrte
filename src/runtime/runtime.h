/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2008 Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Interface into the PRRTE Run Time Environment
 */
#ifndef PRRTE_RUNTIME_H
#define PRRTE_RUNTIME_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "src/util/proc_info.h"

BEGIN_C_DECLS

/** version string of ompi */
PRRTE_EXPORT extern const char prrte_version_string[];

/**
 * Whether PRRTE is initialized or we are in prrte_finalize
 */
PRRTE_EXPORT extern int prrte_initialized;
PRRTE_EXPORT extern bool prrte_finalizing;
PRRTE_EXPORT extern int prrte_debug_output;
PRRTE_EXPORT extern bool prrte_debug_flag;
PRRTE_EXPORT extern int prrte_cache_line_size;

    /**
     * Initialize the Open Run Time Environment
     *
     * Initlize the Open Run Time Environment, including process
     * control, malloc debugging and threads, and out of band messaging.
     * This function should be called exactly once.  This function should
     * be called by every application using the RTE interface, including
     * MPI applications and mpirun.
     *
     * @param pargc  Pointer to the number of arguments in the pargv array
     * @param pargv  The list of arguments.
     * @param flags  Whether we are PRRTE tool or not
     */
PRRTE_EXPORT    int prrte_init(int*pargc, char*** pargv, prrte_proc_type_t flags);
PRRTE_EXPORT int prrte_init_util(void);

    /**
     * Initialize parameters for PRRTE.
     *
     * @retval PRRTE_SUCCESS Upon success.
     * @retval PRRTE_ERROR Upon failure.
     */
PRRTE_EXPORT    int prrte_register_params(void);

    /**
     * Finalize the Open run time environment. Any function calling \code
     * prrte_init should call \code prrte_finalize.
     *
     */
PRRTE_EXPORT int prrte_finalize(void);

END_C_DECLS

#endif /* RUNTIME_H */
