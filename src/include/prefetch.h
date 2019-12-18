/*
 * Copyright (c) 2004-2006 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file
 *
 * Compiler-specific prefetch functions
 *
 * A small set of prefetch / prediction interfaces for using compiler
 * directives to improve memory prefetching and branch prediction
 */

#ifndef PRRTE_PREFETCH_H
#define PRRTE_PREFETCH_H

#include "prrte_config.h"

/* C code */

#if PRRTE_C_HAVE_BUILTIN_EXPECT
#define PRRTE_LIKELY(expression) __builtin_expect(!!(expression), 1)
#define PRRTE_UNLIKELY(expression) __builtin_expect(!!(expression), 0)
#else
#define PRRTE_LIKELY(expression) (expression)
#define PRRTE_UNLIKELY(expression) (expression)
#endif

#if PRRTE_C_HAVE_BUILTIN_PREFETCH
#define PRRTE_PREFETCH(address,rw,locality) __builtin_prefetch(address,rw,locality)
#else
#define PRRTE_PREFETCH(address,rw,locality)
#endif

#endif
