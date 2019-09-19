/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PRRTE_THREAD_USAGE_H)
#define PRRTE_THREAD_USAGE_H

#include "src/include/prrte_config.h"

#include "src/sys/atomic.h"
#include "src/include/prefetch.h"


/**
 * Use an atomic operation for increment/decrement
 */

#define PRRTE_THREAD_DEFINE_ATOMIC_OP(type, name, operator, suffix)      \
__prrte_attribute_always_inline__ static inline type prrte_thread_ ## name ## _fetch_ ## suffix (prrte_atomic_ ## type *addr, type delta) \
{                                                                       \
    return prrte_atomic_ ## name ## _fetch_ ## suffix (addr, delta);     \
}                                                                       \
                                                                        \
__prrte_attribute_always_inline__ static inline type prrte_thread_fetch_ ## name ## _ ## suffix (prrte_atomic_ ## type *addr, type delta) \
{                                                                       \
    return prrte_atomic_fetch_ ## name ## _ ## suffix (addr, delta);     \
}

#define PRRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(type, addr_type, suffix)       \
__prrte_attribute_always_inline__ static inline bool prrte_thread_compare_exchange_strong_ ## suffix (prrte_atomic_ ## addr_type *addr, type *compare, type value) \
{                                                                       \
    return prrte_atomic_compare_exchange_strong_ ## suffix (addr, (addr_type *) compare, (addr_type) value); \
}

#define PRRTE_THREAD_DEFINE_ATOMIC_SWAP(type, addr_type, suffix)         \
__prrte_attribute_always_inline__ static inline type prrte_thread_swap_ ## suffix (prrte_atomic_ ## addr_type *ptr, type newvalue) \
{                                                                       \
    return (type) prrte_atomic_swap_ ## suffix (ptr, (addr_type) newvalue); \
}

PRRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, add, +, 32)
PRRTE_THREAD_DEFINE_ATOMIC_OP(size_t, add, +, size_t)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, and, &, 32)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, or, |, 32)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, xor, ^, 32)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, sub, -, 32)
PRRTE_THREAD_DEFINE_ATOMIC_OP(size_t, sub, -, size_t)

PRRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(int32_t, int32_t, 32)
PRRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(void *, intptr_t, ptr)
PRRTE_THREAD_DEFINE_ATOMIC_SWAP(int32_t, int32_t, 32)
PRRTE_THREAD_DEFINE_ATOMIC_SWAP(void *, intptr_t, ptr)

#define PRRTE_THREAD_ADD_FETCH32 prrte_thread_add_fetch_32
#define PRRTE_ATOMIC_ADD_FETCH32 prrte_thread_add_fetch_32

#define PRRTE_THREAD_AND_FETCH32 prrte_thread_and_fetch_32
#define PRRTE_ATOMIC_AND_FETCH32 prrte_thread_and_fetch_32

#define PRRTE_THREAD_OR_FETCH32 prrte_thread_or_fetch_32
#define PRRTE_ATOMIC_OR_FETCH32 prrte_thread_or_fetch_32

#define PRRTE_THREAD_XOR_FETCH32 prrte_thread_xor_fetch_32
#define PRRTE_ATOMIC_XOR_FETCH32 prrte_thread_xor_fetch_32

#define PRRTE_THREAD_ADD_FETCH_SIZE_T prrte_thread_add_fetch_size_t
#define PRRTE_ATOMIC_ADD_FETCH_SIZE_T prrte_thread_add_fetch_size_t

#define PRRTE_THREAD_SUB_FETCH_SIZE_T prrte_thread_sub_fetch_size_t
#define PRRTE_ATOMIC_SUB_FETCH_SIZE_T prrte_thread_sub_fetch_size_t

#define PRRTE_THREAD_FETCH_ADD32 prrte_thread_fetch_add_32
#define PRRTE_ATOMIC_FETCH_ADD32 prrte_thread_fetch_add_32

#define PRRTE_THREAD_FETCH_AND32 prrte_thread_fetch_and_32
#define PRRTE_ATOMIC_FETCH_AND32 prrte_thread_fetch_and_32

#define PRRTE_THREAD_FETCH_OR32 prrte_thread_fetch_or_32
#define PRRTE_ATOMIC_FETCH_OR32 prrte_thread_fetch_or_32

#define PRRTE_THREAD_FETCH_XOR32 prrte_thread_fetch_xor_32
#define PRRTE_ATOMIC_FETCH_XOR32 prrte_thread_fetch_xor_32

#define PRRTE_THREAD_FETCH_ADD_SIZE_T prrte_thread_fetch_add_size_t
#define PRRTE_ATOMIC_FETCH_ADD_SIZE_T prrte_thread_fetch_add_size_t

#define PRRTE_THREAD_FETCH_SUB_SIZE_T prrte_thread_fetch_sub_size_t
#define PRRTE_ATOMIC_FETCH_SUB_SIZE_T prrte_thread_fetch_sub_size_t

#define PRRTE_THREAD_COMPARE_EXCHANGE_STRONG_32 prrte_thread_compare_exchange_strong_32
#define PRRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_32 prrte_thread_compare_exchange_strong_32

#define PRRTE_THREAD_COMPARE_EXCHANGE_STRONG_PTR(x, y, z) prrte_thread_compare_exchange_strong_ptr ((prrte_atomic_intptr_t *) x, (intptr_t *) y, (intptr_t) z)
#define PRRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_PTR PRRTE_THREAD_COMPARE_EXCHANGE_STRONG_PTR

#define PRRTE_THREAD_SWAP_32 prrte_thread_swap_32
#define PRRTE_ATOMIC_SWAP_32 prrte_thread_swap_32

#define PRRTE_THREAD_SWAP_PTR(x, y) prrte_thread_swap_ptr ((prrte_atomic_intptr_t *) x, (intptr_t) y)
#define PRRTE_ATOMIC_SWAP_PTR PRRTE_THREAD_SWAP_PTR

/* define 64-bit macros is 64-bit atomic math is available */
#if PRRTE_HAVE_ATOMIC_MATH_64

PRRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, add, +, 64)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, and, &, 64)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, or, |, 64)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, xor, ^, 64)
PRRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, sub, -, 64)
PRRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(int64_t, int64_t, 64)
PRRTE_THREAD_DEFINE_ATOMIC_SWAP(int64_t, int64_t, 64)

#define PRRTE_THREAD_ADD_FETCH64 prrte_thread_add_fetch_64
#define PRRTE_ATOMIC_ADD_FETCH64 prrte_thread_add_fetch_64

#define PRRTE_THREAD_AND_FETCH64 prrte_thread_and_fetch_64
#define PRRTE_ATOMIC_AND_FETCH64 prrte_thread_and_fetch_64

#define PRRTE_THREAD_OR_FETCH64 prrte_thread_or_fetch_64
#define PRRTE_ATOMIC_OR_FETCH64 prrte_thread_or_fetch_64

#define PRRTE_THREAD_XOR_FETCH64 prrte_thread_xor_fetch_64
#define PRRTE_ATOMIC_XOR_FETCH64 prrte_thread_xor_fetch_64

#define PRRTE_THREAD_FETCH_ADD64 prrte_thread_fetch_add_64
#define PRRTE_ATOMIC_FETCH_ADD64 prrte_thread_fetch_add_64

#define PRRTE_THREAD_FETCH_AND64 prrte_thread_fetch_and_64
#define PRRTE_ATOMIC_FETCH_AND64 prrte_thread_fetch_and_64

#define PRRTE_THREAD_FETCH_OR64 prrte_thread_fetch_or_64
#define PRRTE_ATOMIC_FETCH_OR64 prrte_thread_fetch_or_64

#define PRRTE_THREAD_FETCH_XOR64 prrte_thread_fetch_xor_64
#define PRRTE_ATOMIC_FETCH_XOR64 prrte_thread_fetch_xor_64

#define PRRTE_THREAD_COMPARE_EXCHANGE_STRONG_64 prrte_thread_compare_exchange_strong_64
#define PRRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_64 prrte_thread_compare_exchange_strong_64

#define PRRTE_THREAD_SWAP_64 prrte_thread_swap_64
#define PRRTE_ATOMIC_SWAP_64 prrte_thread_swap_64

#endif

/* thread local storage */
#if PRRTE_C_HAVE__THREAD_LOCAL
#define prrte_thread_local _Thread_local
#define PRRTE_HAVE_THREAD_LOCAL 1

#elif PRRTE_C_HAVE___THREAD /* PRRTE_C_HAVE__THREAD_LOCAL */
#define prrte_thread_local __thread
#define PRRTE_HAVE_THREAD_LOCAL 1
#endif /* PRRTE_C_HAVE___THREAD */

#if !defined(PRRTE_HAVE_THREAD_LOCAL)
#define PRRTE_HAVE_THREAD_LOCAL 0
#endif /* !defined(PRRTE_HAVE_THREAD_LOCAL) */

#endif /* !defined(PRRTE_THREAD_USAGE_H) */
