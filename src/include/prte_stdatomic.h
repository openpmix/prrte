/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PRTE_STDATOMIC_H)
#define PRTE_STDATOMIC_H

#include "prte_stdint.h"

#if PRTE_ASSEMBLY_BUILTIN != PRTE_BUILTIN_C11

typedef volatile int prte_atomic_int_t;
typedef volatile long prte_atomic_long_t;

typedef volatile int32_t prte_atomic_int32_t;
typedef volatile uint32_t prte_atomic_uint32_t;
typedef volatile int64_t prte_atomic_int64_t;
typedef volatile uint64_t prte_atomic_uint64_t;

typedef volatile size_t prte_atomic_size_t;
typedef volatile ssize_t prte_atomic_ssize_t;
typedef volatile intptr_t prte_atomic_intptr_t;
typedef volatile uintptr_t prte_atomic_uintptr_t;

#else /* PRTE_HAVE_C__ATOMIC */

#include <stdatomic.h>

typedef atomic_int prte_atomic_int_t;
typedef atomic_long prte_atomic_long_t;

typedef _Atomic int32_t prte_atomic_int32_t;
typedef _Atomic uint32_t prte_atomic_uint32_t;
typedef _Atomic int64_t prte_atomic_int64_t;
typedef _Atomic uint64_t prte_atomic_uint64_t;

typedef _Atomic size_t prte_atomic_size_t;
typedef _Atomic ssize_t prte_atomic_ssize_t;
typedef _Atomic intptr_t prte_atomic_intptr_t;
typedef _Atomic uintptr_t prte_atomic_uintptr_t;

#endif /* PRTE_HAVE_C__ATOMIC */

#if HAVE_PRTE_INT128_T

/* do not use C11 atomics for __int128 if they are not lock free */
#if PRTE_HAVE_C11_CSWAP_INT128

typedef _Atomic prte_int128_t prte_atomic_int128_t;

#else

typedef volatile prte_int128_t prte_atomic_int128_t;

#endif

#endif

#endif /* !defined(PRTE_STDATOMIC_H) */
