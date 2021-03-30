/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2014 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016      Broadcom Limited. All rights reserved.
 * Copyright (c) 2016-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file
 *
 * Cycle counter reading instructions.  Do not use directly - see the
 * timer interface instead
 */

#ifndef PRTE_SYS_TIMER_H
#define PRTE_SYS_TIMER_H 1

#include "prte_config.h"

#include "src/sys/architecture.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

/* do some quick #define cleanup in cases where we are doing
   testing... */
#ifdef PRTE_DISABLE_INLINE_ASM
#    undef PRTE_C_GCC_INLINE_ASSEMBLY
#    define PRTE_C_GCC_INLINE_ASSEMBLY 0
#endif

/* define PRTE_{GCC,DEC,XLC}_INLINE_ASSEMBLY based on the
   PRTE_{C,CXX}_{GCC,DEC,XLC}_INLINE_ASSEMBLY defines and whether we
   are in C or C++ */
#if defined(c_plusplus) || defined(__cplusplus)
#    define PRTE_GCC_INLINE_ASSEMBLY PRTE_CXX_GCC_INLINE_ASSEMBLY
#else
#    define PRTE_GCC_INLINE_ASSEMBLY PRTE_C_GCC_INLINE_ASSEMBLY
#endif

/**********************************************************************
 *
 * Load the appropriate architecture files and set some reasonable
 * default values for our support
 *
 *********************************************************************/

/* By default we suppose all timers are monotonic per node. */
#define PRTE_TIMER_MONOTONIC 1

BEGIN_C_DECLS

/* If you update this list, you probably also want to update
   src/mca/timer/linux/configure.m4.  Or not. */

#if defined(DOXYGEN)
/* don't include system-level gorp when generating doxygen files */
#elif PRTE_ASSEMBLY_ARCH == PRTE_X86_64
#    include "src/sys/x86_64/timer.h"
#elif PRTE_ASSEMBLY_ARCH == PRTE_ARM
#    include "src/sys/arm/timer.h"
#elif PRTE_ASSEMBLY_ARCH == PRTE_ARM64
#    include "src/sys/arm64/timer.h"
#elif PRTE_ASSEMBLY_ARCH == PRTE_IA32
#    include "src/sys/ia32/timer.h"
#elif PRTE_ASSEMBLY_ARCH == PRTE_POWERPC32
#    include "src/sys/powerpc/timer.h"
#elif PRTE_ASSEMBLY_ARCH == PRTE_POWERPC64
#    include "src/sys/powerpc/timer.h"
#endif

#ifndef DOXYGEN
#    ifndef PRTE_HAVE_SYS_TIMER_GET_CYCLES
#        define PRTE_HAVE_SYS_TIMER_GET_CYCLES 0

typedef long prte_timer_t;
#    endif

#    ifndef PRTE_HAVE_SYS_TIMER_GET_FREQ
#        define PRTE_HAVE_SYS_TIMER_GET_FREQ 0
#    endif
#endif

#ifndef PRTE_HAVE_SYS_TIMER_IS_MONOTONIC

#    define PRTE_HAVE_SYS_TIMER_IS_MONOTONIC 1

static inline bool prte_sys_timer_is_monotonic(void)
{
    return PRTE_TIMER_MONOTONIC;
}

#endif

END_C_DECLS

#endif /* PRTE_SYS_TIMER_H */
