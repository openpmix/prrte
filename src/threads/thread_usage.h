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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PRTE_THREAD_USAGE_H)
#    define PRTE_THREAD_USAGE_H

#    include "src/include/prte_config.h"

#    include "src/include/prefetch.h"
#    include "src/sys/atomic.h"

/**
 * Use an atomic operation for increment/decrement
 */

#    define PRTE_THREAD_DEFINE_ATOMIC_OP(type, name, operator, suffix)                \
        __prte_attribute_always_inline__ static inline type                           \
            prte_thread_##name##_fetch_##suffix(prte_atomic_##type *addr, type delta) \
        {                                                                             \
            return prte_atomic_##name##_fetch_##suffix(addr, delta);                  \
        }                                                                             \
                                                                                      \
        __prte_attribute_always_inline__ static inline type                           \
            prte_thread_fetch_##name##_##suffix(prte_atomic_##type *addr, type delta) \
        {                                                                             \
            return prte_atomic_fetch_##name##_##suffix(addr, delta);                  \
        }

#    define PRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(type, addr_type, suffix)              \
        __prte_attribute_always_inline__ static inline bool                                  \
            prte_thread_compare_exchange_strong_##suffix(prte_atomic_##addr_type *addr,      \
                                                         type *compare, type value)          \
        {                                                                                    \
            return prte_atomic_compare_exchange_strong_##suffix(addr, (addr_type *) compare, \
                                                                (addr_type) value);          \
        }

#    define PRTE_THREAD_DEFINE_ATOMIC_SWAP(type, addr_type, suffix)                \
        __prte_attribute_always_inline__ static inline type                        \
            prte_thread_swap_##suffix(prte_atomic_##addr_type *ptr, type newvalue) \
        {                                                                          \
            return (type) prte_atomic_swap_##suffix(ptr, (addr_type) newvalue);    \
        }

PRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, add, +, 32)
PRTE_THREAD_DEFINE_ATOMIC_OP(size_t, add, +, size_t)
PRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, and, &, 32)
PRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, or, |, 32)
PRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, xor, ^, 32)
PRTE_THREAD_DEFINE_ATOMIC_OP(int32_t, sub, -, 32)
PRTE_THREAD_DEFINE_ATOMIC_OP(size_t, sub, -, size_t)

PRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(int32_t, int32_t, 32)
PRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(void *, intptr_t, ptr)
PRTE_THREAD_DEFINE_ATOMIC_SWAP(int32_t, int32_t, 32)
PRTE_THREAD_DEFINE_ATOMIC_SWAP(void *, intptr_t, ptr)

#    define PRTE_THREAD_ADD_FETCH32 prte_thread_add_fetch_32
#    define PRTE_ATOMIC_ADD_FETCH32 prte_thread_add_fetch_32

#    define PRTE_THREAD_AND_FETCH32 prte_thread_and_fetch_32
#    define PRTE_ATOMIC_AND_FETCH32 prte_thread_and_fetch_32

#    define PRTE_THREAD_OR_FETCH32 prte_thread_or_fetch_32
#    define PRTE_ATOMIC_OR_FETCH32 prte_thread_or_fetch_32

#    define PRTE_THREAD_XOR_FETCH32 prte_thread_xor_fetch_32
#    define PRTE_ATOMIC_XOR_FETCH32 prte_thread_xor_fetch_32

#    define PRTE_THREAD_ADD_FETCH_SIZE_T prte_thread_add_fetch_size_t
#    define PRTE_ATOMIC_ADD_FETCH_SIZE_T prte_thread_add_fetch_size_t

#    define PRTE_THREAD_SUB_FETCH_SIZE_T prte_thread_sub_fetch_size_t
#    define PRTE_ATOMIC_SUB_FETCH_SIZE_T prte_thread_sub_fetch_size_t

#    define PRTE_THREAD_FETCH_ADD32 prte_thread_fetch_add_32
#    define PRTE_ATOMIC_FETCH_ADD32 prte_thread_fetch_add_32

#    define PRTE_THREAD_FETCH_AND32 prte_thread_fetch_and_32
#    define PRTE_ATOMIC_FETCH_AND32 prte_thread_fetch_and_32

#    define PRTE_THREAD_FETCH_OR32 prte_thread_fetch_or_32
#    define PRTE_ATOMIC_FETCH_OR32 prte_thread_fetch_or_32

#    define PRTE_THREAD_FETCH_XOR32 prte_thread_fetch_xor_32
#    define PRTE_ATOMIC_FETCH_XOR32 prte_thread_fetch_xor_32

#    define PRTE_THREAD_FETCH_ADD_SIZE_T prte_thread_fetch_add_size_t
#    define PRTE_ATOMIC_FETCH_ADD_SIZE_T prte_thread_fetch_add_size_t

#    define PRTE_THREAD_FETCH_SUB_SIZE_T prte_thread_fetch_sub_size_t
#    define PRTE_ATOMIC_FETCH_SUB_SIZE_T prte_thread_fetch_sub_size_t

#    define PRTE_THREAD_COMPARE_EXCHANGE_STRONG_32 prte_thread_compare_exchange_strong_32
#    define PRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_32 prte_thread_compare_exchange_strong_32

#    define PRTE_THREAD_COMPARE_EXCHANGE_STRONG_PTR(x, y, z)                                \
        prte_thread_compare_exchange_strong_ptr((prte_atomic_intptr_t *) x, (intptr_t *) y, \
                                                (intptr_t) z)
#    define PRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_PTR PRTE_THREAD_COMPARE_EXCHANGE_STRONG_PTR

#    define PRTE_THREAD_SWAP_32 prte_thread_swap_32
#    define PRTE_ATOMIC_SWAP_32 prte_thread_swap_32

#    define PRTE_THREAD_SWAP_PTR(x, y) \
        prte_thread_swap_ptr((prte_atomic_intptr_t *) x, (intptr_t) y)
#    define PRTE_ATOMIC_SWAP_PTR PRTE_THREAD_SWAP_PTR

/* define 64-bit macros is 64-bit atomic math is available */
#    if PRTE_HAVE_ATOMIC_MATH_64

PRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, add, +, 64)
PRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, and, &, 64)
PRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, or, |, 64)
PRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, xor, ^, 64)
PRTE_THREAD_DEFINE_ATOMIC_OP(int64_t, sub, -, 64)
PRTE_THREAD_DEFINE_ATOMIC_COMPARE_EXCHANGE(int64_t, int64_t, 64)
PRTE_THREAD_DEFINE_ATOMIC_SWAP(int64_t, int64_t, 64)

#        define PRTE_THREAD_ADD_FETCH64 prte_thread_add_fetch_64
#        define PRTE_ATOMIC_ADD_FETCH64 prte_thread_add_fetch_64

#        define PRTE_THREAD_AND_FETCH64 prte_thread_and_fetch_64
#        define PRTE_ATOMIC_AND_FETCH64 prte_thread_and_fetch_64

#        define PRTE_THREAD_OR_FETCH64 prte_thread_or_fetch_64
#        define PRTE_ATOMIC_OR_FETCH64 prte_thread_or_fetch_64

#        define PRTE_THREAD_XOR_FETCH64 prte_thread_xor_fetch_64
#        define PRTE_ATOMIC_XOR_FETCH64 prte_thread_xor_fetch_64

#        define PRTE_THREAD_FETCH_ADD64 prte_thread_fetch_add_64
#        define PRTE_ATOMIC_FETCH_ADD64 prte_thread_fetch_add_64

#        define PRTE_THREAD_FETCH_AND64 prte_thread_fetch_and_64
#        define PRTE_ATOMIC_FETCH_AND64 prte_thread_fetch_and_64

#        define PRTE_THREAD_FETCH_OR64 prte_thread_fetch_or_64
#        define PRTE_ATOMIC_FETCH_OR64 prte_thread_fetch_or_64

#        define PRTE_THREAD_FETCH_XOR64 prte_thread_fetch_xor_64
#        define PRTE_ATOMIC_FETCH_XOR64 prte_thread_fetch_xor_64

#        define PRTE_THREAD_COMPARE_EXCHANGE_STRONG_64 prte_thread_compare_exchange_strong_64
#        define PRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_64 prte_thread_compare_exchange_strong_64

#        define PRTE_THREAD_SWAP_64 prte_thread_swap_64
#        define PRTE_ATOMIC_SWAP_64 prte_thread_swap_64

#    endif

/* thread local storage */
#    if PRTE_C_HAVE__THREAD_LOCAL
#        define prte_thread_local      _Thread_local
#        define PRTE_HAVE_THREAD_LOCAL 1

#    elif PRTE_C_HAVE___THREAD /* PRTE_C_HAVE__THREAD_LOCAL */
#        define prte_thread_local      __thread
#        define PRTE_HAVE_THREAD_LOCAL 1
#    endif /* PRTE_C_HAVE___THREAD */

#    if !defined(PRTE_HAVE_THREAD_LOCAL)
#        define PRTE_HAVE_THREAD_LOCAL 0
#    endif /* !defined(PRTE_HAVE_THREAD_LOCAL) */

#endif /* !defined(PRTE_THREAD_USAGE_H) */
