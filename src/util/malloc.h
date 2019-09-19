/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file */

#ifndef PRRTE_MALLOC_H
#define PRRTE_MALLOC_H

#include <stdio.h>
#include <stdlib.h>

/*
 * THIS FILE CANNOT INCLUDE ANY OTHER PRRTE HEADER FILES!!!
 *
 * It is included via <prrte_config_bottom.h>.  Hence, it should not
 * include ANY other files, nor should it include "prrte_config.h".
 *
 */

/*
 * Set PRRTE_MALLOC_DEBUG_LEVEL to
 * 0 for no checking
 * 1 for basic error checking
 * 2 for more error checking
 */

#ifndef PRRTE_MALLOC_DEBUG_LEVEL
#define PRRTE_MALLOC_DEBUG_LEVEL 2
#endif

BEGIN_C_DECLS
  /**
   * Initialize malloc debug output.
   *
   * This function is invoked to setup a dedicated output stream for
   * malloc debug functions.  It does \em not (currently) do anything
   * other than that (i.e., no internal accounting for tracking
   * malloc/free statements, etc.).
   *
   * It is invoked as part of prrte_init().  Although this function is
   * not \em necessary for PRRTE_MALLOC() and PRRTE_FREE(), it is strong
   * recommended because no output messages -- regardless of the
   * malloc debug level set by prrte_malloc_debug() -- will be displayed
   * unless this function is invoked first.
   */
PRRTE_EXPORT void prrte_malloc_init(void);
PRRTE_EXPORT void prrte_malloc_finalize(void);

  /**
   * \internal
   *
   * Back-end error-checking malloc function for PRRTE (you should use
   * the normal malloc() instead of this function).
   *
   * @param size The number of bytes to allocate
   * @param file Typically the __FILE__ macro
   * @param line Typically the __LINE__ macro
   *
   * This function is only used when --enable-mem-debug was specified to
   * configure (or by default if you're building in a SVN checkout).
   */
PRRTE_EXPORT void *prrte_malloc(size_t size, const char *file, int line) __prrte_attribute_malloc__ __prrte_attribute_warn_unused_result__;

  /**
   * \internal
   *
   * Back-end error-checking calloc function for PRRTE (you should use
   * the normal calloc() instead of this function).
   *
   * @param nmembers Number of elements to malloc
   * @param size Size of each elements
   * @param file Typically the __FILE__ macro
   * @param line Typically the __LINE__ macro
   *
   * This function is only used when --enable-mem-debug was specified to
   * configure (or by default if you're building in a SVN checkout).
   */
PRRTE_EXPORT void *prrte_calloc(size_t nmembers, size_t size, const char *file, int line) __prrte_attribute_malloc__ __prrte_attribute_warn_unused_result__;

  /**
   * \internal
   *
   * Back-end error-checking realloc function for PRRTE (you should use
   * the normal realloc() instead of this function).
   *
   * @param ptr Pointer to reallocate
   * @param size The number of bytes to allocate
   * @param file Typically the __FILE__ macro
   * @param line Typically the __LINE__ macro
   *
   * This function is only used when --enable-mem-debug was specified to
   * configure (or by default if you're building in a SVN checkout).
   */
PRRTE_EXPORT void *prrte_realloc(void *ptr, size_t size, const char *file, int line) __prrte_attribute_malloc__ __prrte_attribute_warn_unused_result__;

  /**
   * \internal
   *
   * Back-end error-checking free function for PRRTE (you should use
   * free() instead of this function).
   *
   * @param addr Address on the heap to free()
   * @param file Typically the __FILE__ macro
   * @param line Typically the __LINE__ macro
   *
   * This function is only used when --enable-mem-debug was specified
   * to configure (or by default if you're building in a SVN
   * checkout).
   */
PRRTE_EXPORT void prrte_free(void *addr, const char *file, int line) __prrte_attribute_nonnull__(1);

/**
 * Used to set the debug level for malloc debug.
 *
 * @param level The level of debugging (0 = none, 1 = some, 2 = more)
 *
 * This value defaults to the PRRTE_MALLOC_DEBUG_LEVEL.
 */
PRRTE_EXPORT void prrte_malloc_debug(int level);

END_C_DECLS

#endif /* PRRTE_MALLOC_H */
