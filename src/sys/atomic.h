/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2011      Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
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
 * Atomic operations.
 *
 * This API is patterned after the FreeBSD kernel atomic interface
 * (which is influenced by Intel's ia64 architecture).  The
 * FreeBSD interface is documented at
 *
 * http://www.freebsd.org/cgi/man.cgi?query=atomic&sektion=9
 *
 * Only the necessary subset of functions are implemented here.
 *
 * The following #defines will be true / false based on
 * assembly support:
 *
 *  - \c PRTE_HAVE_ATOMIC_MEM_BARRIER atomic memory barriers
 *  - \c PRTE_HAVE_ATOMIC_SPINLOCKS atomic spinlocks
 *  - \c PRTE_HAVE_ATOMIC_MATH_32 if 32 bit add/sub/compare-exchange can be done "atomicly"
 *  - \c PRTE_HAVE_ATOMIC_MATH_64 if 64 bit add/sub/compare-exchange can be done "atomicly"
 *
 * Note that for the Atomic math, atomic add/sub may be implemented as
 * C code using prte_atomic_compare_exchange.  The appearance of atomic
 * operation will be upheld in these cases.
 */

#ifndef PRTE_SYS_ATOMIC_H
#define PRTE_SYS_ATOMIC_H 1

#include "prte_config.h"

#include <stdbool.h>

#include "src/sys/architecture.h"
#include "prte_stdatomic.h"

#if PRTE_ASSEMBLY_BUILTIN == PRTE_BUILTIN_C11

#    include "atomic_stdc.h"

#else /* !PRTE_C_HAVE__ATOMIC */

/* do some quick #define cleanup in cases where we are doing
   testing... */
#    ifdef PRTE_DISABLE_INLINE_ASM
#        undef PRTE_C_GCC_INLINE_ASSEMBLY
#        define PRTE_C_GCC_INLINE_ASSEMBLY 0
#    endif

/* define PRTE_{GCC,DEC,XLC}_INLINE_ASSEMBLY based on the
   PRTE_C_{GCC,DEC,XLC}_INLINE_ASSEMBLY defines and whether we
   are in C or C++ */
#    if defined(c_plusplus) || defined(__cplusplus)
/* We no longer support inline assembly for C++ as PRTE is a C-only interface */
#        define PRTE_GCC_INLINE_ASSEMBLY 0
#    else
#        define PRTE_GCC_INLINE_ASSEMBLY PRTE_C_GCC_INLINE_ASSEMBLY
#    endif

BEGIN_C_DECLS
/**********************************************************************
 *
 * Data structures for atomic ops
 *
 *********************************************************************/
/**
 * Volatile lock object (with optional padding).
 *
 * \note The internals of the lock are included here, but should be
 * considered private.  The implementation currently in use may choose
 * to use an int or unsigned char as the lock value - the user is not
 * informed either way.
 */
struct prte_atomic_lock_t {
    union {
        prte_atomic_int32_t lock;          /**< The lock address (an integer) */
        volatile unsigned char sparc_lock; /**< The lock address on sparc */
        char padding[sizeof(int)];         /**< Array for optional padding */
    } u;
};
typedef struct prte_atomic_lock_t prte_atomic_lock_t;

/**********************************************************************
 *
 * Set or unset these macros in the architecture-specific atomic.h
 * files if we need to specify them as inline or non-inline
 *
 *********************************************************************/
#    if !PRTE_GCC_INLINE_ASSEMBLY
#        define PRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER         0
#        define PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32 0
#        define PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64 0
#        define PRTE_HAVE_INLINE_ATOMIC_ADD_32              0
#        define PRTE_HAVE_INLINE_ATOMIC_AND_32              0
#        define PRTE_HAVE_INLINE_ATOMIC_OR_32               0
#        define PRTE_HAVE_INLINE_ATOMIC_XOR_32              0
#        define PRTE_HAVE_INLINE_ATOMIC_SUB_32              0
#        define PRTE_HAVE_INLINE_ATOMIC_ADD_64              0
#        define PRTE_HAVE_INLINE_ATOMIC_AND_64              0
#        define PRTE_HAVE_INLINE_ATOMIC_OR_64               0
#        define PRTE_HAVE_INLINE_ATOMIC_XOR_64              0
#        define PRTE_HAVE_INLINE_ATOMIC_SUB_64              0
#        define PRTE_HAVE_INLINE_ATOMIC_SWAP_32             0
#        define PRTE_HAVE_INLINE_ATOMIC_SWAP_64             0
#    else
#        define PRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER         1
#        define PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32 1
#        define PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64 1
#        define PRTE_HAVE_INLINE_ATOMIC_ADD_32              1
#        define PRTE_HAVE_INLINE_ATOMIC_AND_32              1
#        define PRTE_HAVE_INLINE_ATOMIC_OR_32               1
#        define PRTE_HAVE_INLINE_ATOMIC_XOR_32              1
#        define PRTE_HAVE_INLINE_ATOMIC_SUB_32              1
#        define PRTE_HAVE_INLINE_ATOMIC_ADD_64              1
#        define PRTE_HAVE_INLINE_ATOMIC_AND_64              1
#        define PRTE_HAVE_INLINE_ATOMIC_OR_64               1
#        define PRTE_HAVE_INLINE_ATOMIC_XOR_64              1
#        define PRTE_HAVE_INLINE_ATOMIC_SUB_64              1
#        define PRTE_HAVE_INLINE_ATOMIC_SWAP_32             1
#        define PRTE_HAVE_INLINE_ATOMIC_SWAP_64             1
#    endif

/**
 * Enumeration of lock states
 */
enum { PRTE_ATOMIC_LOCK_UNLOCKED = 0, PRTE_ATOMIC_LOCK_LOCKED = 1 };

#    define PRTE_ATOMIC_LOCK_INIT                     \
        {                                             \
            .u = {.lock = PRTE_ATOMIC_LOCK_UNLOCKED } \
        }

/**********************************************************************
 *
 * Load the appropriate architecture files and set some reasonable
 * default values for our support
 *
 *********************************************************************/
#    if defined(DOXYGEN)
/* don't include system-level gorp when generating doxygen files */
#    elif PRTE_ASSEMBLY_BUILTIN == PRTE_BUILTIN_GCC
#        include "src/sys/gcc_builtin/atomic.h"
#    elif PRTE_ASSEMBLY_ARCH == PRTE_X86_64
#        include "src/sys/x86_64/atomic.h"
#    elif PRTE_ASSEMBLY_ARCH == PRTE_ARM
#        include "src/sys/arm/atomic.h"
#    elif PRTE_ASSEMBLY_ARCH == PRTE_ARM64
#        include "src/sys/arm64/atomic.h"
#    elif PRTE_ASSEMBLY_ARCH == PRTE_IA32
#        include "src/sys/ia32/atomic.h"
#    elif PRTE_ASSEMBLY_ARCH == PRTE_POWERPC32
#        include "src/sys/powerpc/atomic.h"
#    elif PRTE_ASSEMBLY_ARCH == PRTE_POWERPC64
#        include "src/sys/powerpc/atomic.h"
#    endif

#    ifndef DOXYGEN
/* compare and set operations can't really be emulated from software,
   so if these defines aren't already set, they should be set to 0
   now */
#        ifndef PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32
#            define PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 0
#        endif
#        ifndef PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64
#            define PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64 0
#        endif
#        ifndef PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128
#            define PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128 0
#        endif
#        ifndef PRTE_HAVE_ATOMIC_LLSC_32
#            define PRTE_HAVE_ATOMIC_LLSC_32 0
#        endif
#        ifndef PRTE_HAVE_ATOMIC_LLSC_64
#            define PRTE_HAVE_ATOMIC_LLSC_64 0
#        endif
#    endif /* DOXYGEN */

/**********************************************************************
 *
 * Memory Barriers - defined here if running doxygen or have barriers
 *                   but can't inline
 *
 *********************************************************************/
#    if !defined(PRTE_HAVE_ATOMIC_MEM_BARRIER) && !defined(DOXYGEN)
/* no way to emulate in C code */
#        define PRTE_HAVE_ATOMIC_MEM_BARRIER 0
#    endif

#    if defined(DOXYGEN) || PRTE_HAVE_ATOMIC_MEM_BARRIER
/**
 * Memory barrier
 *
 * Will use system-specific features to instruct the processor and
 * memory controller that all writes and reads that have been posted
 * before the call to \c prte_atomic_mb() must appear to have
 * completed before the next read or write.
 *
 * \note This can have some expensive side effects, including flushing
 * the pipeline, preventing the cpu from reordering instructions, and
 * generally grinding the memory controller's performance.  Use only
 * if you need *both* read and write barriers.
 */

#        if PRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER
static inline
#        endif
    void
    prte_atomic_mb(void);

/**
 * Read memory barrier
 *
 * Use system-specific features to instruct the processor and memory
 * conrtoller that all reads that have been posted before the call to
 * \c prte_atomic_rmb() must appear to have been completed before the
 * next read.  Nothing is said about the ordering of writes when using
 * \c prte_atomic_rmb().
 */

#        if PRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER
static inline
#        endif
    void
    prte_atomic_rmb(void);

/**
 * Write memory barrier.
 *
 * Use system-specific features to instruct the processor and memory
 * conrtoller that all writes that have been posted before the call to
 * \c prte_atomic_wmb() must appear to have been completed before the
 * next write.  Nothing is said about the ordering of reads when using
 * \c prte_atomic_wmb().
 */

#        if PRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER
static inline
#        endif
    void
    prte_atomic_wmb(void);

#    endif /* defined(DOXYGEN) || PRTE_HAVE_ATOMIC_MEM_BARRIER */

/**********************************************************************
 *
 * Atomic spinlocks - always inlined, if have atomic compare-and-swap
 *
 *********************************************************************/

#    if !defined(PRTE_HAVE_ATOMIC_SPINLOCKS) && !defined(DOXYGEN)
/* 0 is more like "pending" - we'll fix up at the end after all
   the static inline functions are declared */
#        define PRTE_HAVE_ATOMIC_SPINLOCKS 0
#    endif

#    if defined(DOXYGEN) || PRTE_HAVE_ATOMIC_SPINLOCKS \
        || (PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64)

/**
 * Initialize a lock to value
 *
 * @param lock         Address of the lock
 * @param value        Initial value to set lock to
 */
#        if PRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#        endif
    void
    prte_atomic_lock_init(prte_atomic_lock_t *lock, int32_t value);

/**
 * Try to acquire a lock.
 *
 * @param lock          Address of the lock.
 * @return              0 if the lock was acquired, 1 otherwise.
 */
#        if PRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#        endif
    int
    prte_atomic_trylock(prte_atomic_lock_t *lock);

/**
 * Acquire a lock by spinning.
 *
 * @param lock          Address of the lock.
 */
#        if PRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#        endif
    void
    prte_atomic_lock(prte_atomic_lock_t *lock);

/**
 * Release a lock.
 *
 * @param lock          Address of the lock.
 */
#        if PRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#        endif
    void
    prte_atomic_unlock(prte_atomic_lock_t *lock);

#        if PRTE_HAVE_ATOMIC_SPINLOCKS == 0
#            undef PRTE_HAVE_ATOMIC_SPINLOCKS
#            define PRTE_HAVE_ATOMIC_SPINLOCKS \
                (PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64)
#            define PRTE_NEED_INLINE_ATOMIC_SPINLOCKS 1
#        endif

#    endif /* PRTE_HAVE_ATOMIC_SPINLOCKS */

/**********************************************************************
 *
 * Atomic math operations
 *
 *********************************************************************/
#    if !defined(PRTE_HAVE_ATOMIC_CMPSET_32) && !defined(DOXYGEN)
#        define PRTE_HAVE_ATOMIC_CMPSET_32 0
#    endif
#    if defined(DOXYGEN) || PRTE_HAVE_ATOMIC_CMPSET_32

#        if PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32
static inline
#        endif
    bool
    prte_atomic_compare_exchange_strong_32(prte_atomic_int32_t *addr, int32_t *oldval,
                                           int32_t newval);

#        if PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32
static inline
#        endif
    bool
    prte_atomic_compare_exchange_strong_acq_32(prte_atomic_int32_t *addr, int32_t *oldval,
                                               int32_t newval);

#        if PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32
static inline
#        endif
    bool
    prte_atomic_compare_exchange_strong_rel_32(prte_atomic_int32_t *addr, int32_t *oldval,
                                               int32_t newval);
#    endif

#    if !defined(PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64) && !defined(DOXYGEN)
#        define PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64 0
#    endif
#    if defined(DOXYGEN) || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64

#        if PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64
static inline
#        endif
    bool
    prte_atomic_compare_exchange_strong_64(prte_atomic_int64_t *addr, int64_t *oldval,
                                           int64_t newval);

#        if PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64
static inline
#        endif
    bool
    prte_atomic_compare_exchange_strong_acq_64(prte_atomic_int64_t *addr, int64_t *oldval,
                                               int64_t newval);

#        if PRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64
static inline
#        endif
    bool
    prte_atomic_compare_exchange_strong_rel_64(prte_atomic_int64_t *addr, int64_t *oldval,
                                               int64_t newval);

#    endif

#    if !defined(PRTE_HAVE_ATOMIC_MATH_32) && !defined(DOXYGEN)
/* define to 0 for these tests.  WIll fix up later. */
#        define PRTE_HAVE_ATOMIC_MATH_32 0
#    endif

#    if defined(DOXYGEN) || PRTE_HAVE_ATOMIC_MATH_32 || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32

static inline int32_t prte_atomic_add_fetch_32(prte_atomic_int32_t *addr, int delta);
static inline int32_t prte_atomic_fetch_add_32(prte_atomic_int32_t *addr, int delta);
static inline int32_t prte_atomic_and_fetch_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_fetch_and_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_or_fetch_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_fetch_or_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_xor_fetch_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_fetch_xor_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_sub_fetch_32(prte_atomic_int32_t *addr, int delta);
static inline int32_t prte_atomic_fetch_sub_32(prte_atomic_int32_t *addr, int delta);
static inline int32_t prte_atomic_min_fetch_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_fetch_min_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_max_fetch_32(prte_atomic_int32_t *addr, int32_t value);
static inline int32_t prte_atomic_fetch_max_32(prte_atomic_int32_t *addr, int32_t value);

#    endif /* PRTE_HAVE_ATOMIC_MATH_32 */

#    if !PRTE_HAVE_ATOMIC_MATH_32
/* fix up the value of prte_have_atomic_math_32 to allow for C versions */
#        undef PRTE_HAVE_ATOMIC_MATH_32
#        define PRTE_HAVE_ATOMIC_MATH_32 PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32
#    endif

#    ifndef PRTE_HAVE_ATOMIC_MATH_64
/* define to 0 for these tests.  WIll fix up later. */
#        define PRTE_HAVE_ATOMIC_MATH_64 0
#    endif

#    if defined(DOXYGEN) || PRTE_HAVE_ATOMIC_MATH_64 || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64

static inline int64_t prte_atomic_add_fetch_64(prte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prte_atomic_fetch_add_64(prte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prte_atomic_and_fetch_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_fetch_and_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_or_fetch_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_fetch_or_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_fetch_xor_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_sub_fetch_64(prte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prte_atomic_fetch_sub_64(prte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prte_atomic_min_fetch_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_fetch_min_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_max_fetch_64(prte_atomic_int64_t *addr, int64_t value);
static inline int64_t prte_atomic_fetch_max_64(prte_atomic_int64_t *addr, int64_t value);

#    endif /* PRTE_HAVE_ATOMIC_MATH_64 */

#    if !PRTE_HAVE_ATOMIC_MATH_64
/* fix up the value of prte_have_atomic_math_64 to allow for C versions */
#        undef PRTE_HAVE_ATOMIC_MATH_64
#        define PRTE_HAVE_ATOMIC_MATH_64 PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64
#    endif

/* provide a size_t add/subtract.  When in debug mode, make it an
 * inline function so that we don't have any casts in the
 *  interface and can catch type errors.  When not in debug mode,
 * just make it a macro, so that there's no performance penalty
 */
#    if defined(DOXYGEN) || PRTE_ENABLE_DEBUG
static inline size_t prte_atomic_add_fetch_size_t(prte_atomic_size_t *addr, size_t delta)
{
#        if SIZEOF_SIZE_T == 4
    return (size_t) prte_atomic_add_fetch_32((int32_t *) addr, delta);
#        elif SIZEOF_SIZE_T == 8
    return (size_t) prte_atomic_add_fetch_64((int64_t *) addr, delta);
#        else
#            error "Unknown size_t size"
#        endif
}

static inline size_t prte_atomic_fetch_add_size_t(prte_atomic_size_t *addr, size_t delta)
{
#        if SIZEOF_SIZE_T == 4
    return (size_t) prte_atomic_fetch_add_32((int32_t *) addr, delta);
#        elif SIZEOF_SIZE_T == 8
    return (size_t) prte_atomic_fetch_add_64((int64_t *) addr, delta);
#        else
#            error "Unknown size_t size"
#        endif
}

static inline size_t prte_atomic_sub_fetch_size_t(prte_atomic_size_t *addr, size_t delta)
{
#        if SIZEOF_SIZE_T == 4
    return (size_t) prte_atomic_sub_fetch_32((int32_t *) addr, delta);
#        elif SIZEOF_SIZE_T == 8
    return (size_t) prte_atomic_sub_fetch_64((int64_t *) addr, delta);
#        else
#            error "Unknown size_t size"
#        endif
}

static inline size_t prte_atomic_fetch_sub_size_t(prte_atomic_size_t *addr, size_t delta)
{
#        if SIZEOF_SIZE_T == 4
    return (size_t) prte_atomic_fetch_sub_32((int32_t *) addr, delta);
#        elif SIZEOF_SIZE_T == 8
    return (size_t) prte_atomic_fetch_sub_64((int64_t *) addr, delta);
#        else
#            error "Unknown size_t size"
#        endif
}

#    else
#        if SIZEOF_SIZE_T == 4
#            define prte_atomic_add_fetch_size_t(addr, delta) \
                ((size_t) prte_atomic_add_fetch_32((prte_atomic_int32_t *) addr, delta))
#            define prte_atomic_fetch_add_size_t(addr, delta) \
                ((size_t) prte_atomic_fetch_add_32((prte_atomic_int32_t *) addr, delta))
#            define prte_atomic_sub_fetch_size_t(addr, delta) \
                ((size_t) prte_atomic_sub_fetch_32((prte_atomic_int32_t *) addr, delta))
#            define prte_atomic_fetch_sub_size_t(addr, delta) \
                ((size_t) prte_atomic_fetch_sub_32((prte_atomic_int32_t *) addr, delta))
#        elif SIZEOF_SIZE_T == 8
#            define prte_atomic_add_fetch_size_t(addr, delta) \
                ((size_t) prte_atomic_add_fetch_64((prte_atomic_int64_t *) addr, delta))
#            define prte_atomic_fetch_add_size_t(addr, delta) \
                ((size_t) prte_atomic_fetch_add_64((prte_atomic_int64_t *) addr, delta))
#            define prte_atomic_sub_fetch_size_t(addr, delta) \
                ((size_t) prte_atomic_sub_fetch_64((prte_atomic_int64_t *) addr, delta))
#            define prte_atomic_fetch_sub_size_t(addr, delta) \
                ((size_t) prte_atomic_fetch_sub_64((prte_atomic_int64_t *) addr, delta))
#        else
#            error "Unknown size_t size"
#        endif
#    endif

#    if defined(DOXYGEN) \
        || (PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64)
/* these are always done with inline functions, so always mark as
   static inline */

static inline bool prte_atomic_compare_exchange_strong_xx(prte_atomic_intptr_t *addr,
                                                          intptr_t *oldval, int64_t newval,
                                                          size_t length);
static inline bool prte_atomic_compare_exchange_strong_acq_xx(prte_atomic_intptr_t *addr,
                                                              intptr_t *oldval, int64_t newval,
                                                              size_t length);
static inline bool prte_atomic_compare_exchange_strong_rel_xx(prte_atomic_intptr_t *addr,
                                                              intptr_t *oldval, int64_t newval,
                                                              size_t length);

static inline bool prte_atomic_compare_exchange_strong_ptr(prte_atomic_intptr_t *addr,
                                                           intptr_t *oldval, intptr_t newval);
static inline bool prte_atomic_compare_exchange_strong_acq_ptr(prte_atomic_intptr_t *addr,
                                                               intptr_t *oldval, intptr_t newval);
static inline bool prte_atomic_compare_exchange_strong_rel_ptr(prte_atomic_intptr_t *addr,
                                                               intptr_t *oldval, intptr_t newval);

/**
 * Atomic compare and set of generic type with relaxed semantics. This
 * macro detect at compile time the type of the first argument and
 * choose the correct function to be called.
 *
 * \note This macro should only be used for integer types.
 *
 * @param addr          Address of <TYPE>.
 * @param oldval        Comparison value address of <TYPE>.
 * @param newval        New value to set if comparision is true <TYPE>.
 *
 * See prte_atomic_compare_exchange_* for pseudo-code.
 */
#        define prte_atomic_compare_exchange_strong(ADDR, OLDVAL, NEWVAL)                     \
            prte_atomic_compare_exchange_strong_xx((prte_atomic_intptr_t *) (ADDR),           \
                                                   (intptr_t *) (OLDVAL), (intptr_t)(NEWVAL), \
                                                   sizeof(*(ADDR)))

/**
 * Atomic compare and set of generic type with acquire semantics. This
 * macro detect at compile time the type of the first argument and
 * choose the correct function to be called.
 *
 * \note This macro should only be used for integer types.
 *
 * @param addr          Address of <TYPE>.
 * @param oldval        Comparison value address of <TYPE>.
 * @param newval        New value to set if comparision is true <TYPE>.
 *
 * See prte_atomic_compare_exchange_acq_* for pseudo-code.
 */
#        define prte_atomic_compare_exchange_strong_acq(ADDR, OLDVAL, NEWVAL)                     \
            prte_atomic_compare_exchange_strong_acq_xx((prte_atomic_intptr_t *) (ADDR),           \
                                                       (intptr_t *) (OLDVAL), (intptr_t)(NEWVAL), \
                                                       sizeof(*(ADDR)))

/**
 * Atomic compare and set of generic type with release semantics. This
 * macro detect at compile time the type of the first argument and
 * choose the correct function to be called.
 *
 * \note This macro should only be used for integer types.
 *
 * @param addr          Address of <TYPE>.
 * @param oldval        Comparison value address of <TYPE>.
 * @param newval        New value to set if comparision is true <TYPE>.
 *
 * See prte_atomic_compare_exchange_rel_* for pseudo-code.
 */
#        define prte_atomic_compare_exchange_strong_rel(ADDR, OLDVAL, NEWVAL)                     \
            prte_atomic_compare_exchange_strong_rel_xx((prte_atomic_intptr_t *) (ADDR),           \
                                                       (intptr_t *) (OLDVAL), (intptr_t)(NEWVAL), \
                                                       sizeof(*(ADDR)))

#    endif /* (PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64) */

#    if defined(DOXYGEN) || (PRTE_HAVE_ATOMIC_MATH_32 || PRTE_HAVE_ATOMIC_MATH_64)

static inline void prte_atomic_add_xx(prte_atomic_intptr_t *addr, int32_t value, size_t length);
static inline void prte_atomic_sub_xx(prte_atomic_intptr_t *addr, int32_t value, size_t length);

static inline intptr_t prte_atomic_add_fetch_ptr(prte_atomic_intptr_t *addr, void *delta);
static inline intptr_t prte_atomic_fetch_add_ptr(prte_atomic_intptr_t *addr, void *delta);
static inline intptr_t prte_atomic_sub_fetch_ptr(prte_atomic_intptr_t *addr, void *delta);
static inline intptr_t prte_atomic_fetch_sub_ptr(prte_atomic_intptr_t *addr, void *delta);

/**
 * Atomically increment the content depending on the type. This
 * macro detect at compile time the type of the first argument
 * and choose the correct function to be called.
 *
 * \note This macro should only be used for integer types.
 *
 * @param addr          Address of <TYPE>
 * @param delta         Value to add (converted to <TYPE>).
 */
#        define prte_atomic_add(ADDR, VALUE) \
            prte_atomic_add_xx((prte_atomic_intptr_t *) (ADDR), (int32_t)(VALUE), sizeof(*(ADDR)))

/**
 * Atomically decrement the content depending on the type. This
 * macro detect at compile time the type of the first argument
 * and choose the correct function to be called.
 *
 * \note This macro should only be used for integer types.
 *
 * @param addr          Address of <TYPE>
 * @param delta         Value to substract (converted to <TYPE>).
 */
#        define prte_atomic_sub(ADDR, VALUE) \
            prte_atomic_sub_xx((prte_atomic_intptr_t *) (ADDR), (int32_t)(VALUE), sizeof(*(ADDR)))

#    endif /* PRTE_HAVE_ATOMIC_MATH_32 || PRTE_HAVE_ATOMIC_MATH_64 */

/*
 * Include inline implementations of everything not defined directly
 * in assembly
 */
#    include "src/sys/atomic_impl.h"

#endif /* !PRTE_C_HAVE__ATOMIC */

END_C_DECLS

#endif /* PRTE_SYS_ATOMIC_H */
