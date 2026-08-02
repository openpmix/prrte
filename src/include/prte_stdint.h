/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * This file includes the C99 stdint.h file if available, and otherwise
 * defines fixed-width types according to the SIZEOF information
 * gathered by configure.
 */

#ifndef PRTE_STDINT_H
#define PRTE_STDINT_H 1

#include "prte_config.h"

/*
 * Include what we can and define what is missing.
 */
#include <limits.h>
#include <stdint.h>

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

/* There was a 128-bit block here, selecting prte_int128_t/prte_uint128_t
 * from HAVE_INT128_T or HAVE___INT128 and setting HAVE_PRTE_INT128_T.  It
 * was inherited from OPAL, where the atomics needed a 128-bit type.  Neither
 * of those two configure symbols has ever been probed by PRRTE's configure,
 * so the types were never declared and HAVE_PRTE_INT128_T was always 0 --
 * and nothing in the tree names any of the three.  If PRRTE ever needs one,
 * probe for it in configure.ac at the same time. */

/* Pointers */

#if SIZEOF_VOID_P == SIZEOF_INT

#    ifndef HAVE_INTPTR_T
typedef signed int intptr_t;
#    endif

#    ifndef HAVE_UINTPTR_T
typedef unsigned int uintptr_t;
#    endif

#elif SIZEOF_VOID_P == SIZEOF_LONG

#    ifndef HAVE_INTPTR_T
typedef signed long intptr_t;
#    endif

#    ifndef HAVE_UINTPTR_T
typedef unsigned long uintptr_t;
#    endif

#elif SIZEOF_VOID_P == SIZEOF_LONG_LONG

#    ifndef HAVE_INTPTR_T
typedef signed long long intptr_t;
#    endif
#    ifndef HAVE_UINTPTR_T
typedef unsigned long long uintptr_t;
#    endif

#else

#    error Failed to define pointer-sized integer types

#endif

/* inttypes.h printf specifiers */
#include <inttypes.h>

/* PRRTE requires a C11 compiler (config/prte_setup_cc.m4 aborts otherwise),
 * so "z" is available and is the only spelling that is correct by
 * definition rather than by a size coincidence.
 *
 * This used to be a ladder guarded on ACCEPT_C99 -- a symbol PRRTE's
 * configure has never defined -- so the "zu" arm was unreachable and every
 * build fell through to comparing SIZEOF_SIZE_T against SIZEOF_LONG and
 * SIZEOF_LONG_LONG.  That happens to land on a working answer for the usual
 * data models and silently lands on "u" for any where it does not. */
#ifndef PRIsize_t
#    define PRIsize_t "zu"
#endif

#endif /* PRTE_STDINT_H */
