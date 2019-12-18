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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
 *  - \c PRRTE_HAVE_ATOMIC_MEM_BARRIER atomic memory barriers
 *  - \c PRRTE_HAVE_ATOMIC_SPINLOCKS atomic spinlocks
 *  - \c PRRTE_HAVE_ATOMIC_MATH_32 if 32 bit add/sub/compare-exchange can be done "atomicly"
 *  - \c PRRTE_HAVE_ATOMIC_MATH_64 if 64 bit add/sub/compare-exchange can be done "atomicly"
 *
 * Note that for the Atomic math, atomic add/sub may be implemented as
 * C code using prrte_atomic_compare_exchange.  The appearance of atomic
 * operation will be upheld in these cases.
 */

#ifndef PRRTE_SYS_ATOMIC_H
#define PRRTE_SYS_ATOMIC_H 1

#include "prrte_config.h"

#include <stdbool.h>

#include "src/sys/architecture.h"
#include "prrte_stdatomic.h"

#if PRRTE_ASSEMBLY_BUILTIN == PRRTE_BUILTIN_C11

#include "atomic_stdc.h"

#else /* !PRRTE_C_HAVE__ATOMIC */

/* do some quick #define cleanup in cases where we are doing
   testing... */
#ifdef PRRTE_DISABLE_INLINE_ASM
#undef PRRTE_C_GCC_INLINE_ASSEMBLY
#define PRRTE_C_GCC_INLINE_ASSEMBLY 0
#endif

/* define PRRTE_{GCC,DEC,XLC}_INLINE_ASSEMBLY based on the
   PRRTE_C_{GCC,DEC,XLC}_INLINE_ASSEMBLY defines and whether we
   are in C or C++ */
#if defined(c_plusplus) || defined(__cplusplus)
/* We no longer support inline assembly for C++ as PRRTE is a C-only interface */
#define PRRTE_GCC_INLINE_ASSEMBLY 0
#else
#define PRRTE_GCC_INLINE_ASSEMBLY PRRTE_C_GCC_INLINE_ASSEMBLY
#endif


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
struct prrte_atomic_lock_t {
    union {
        prrte_atomic_int32_t lock;     /**< The lock address (an integer) */
        volatile unsigned char sparc_lock; /**< The lock address on sparc */
        char padding[sizeof(int)]; /**< Array for optional padding */
    } u;
};
typedef struct prrte_atomic_lock_t prrte_atomic_lock_t;

/**********************************************************************
 *
 * Set or unset these macros in the architecture-specific atomic.h
 * files if we need to specify them as inline or non-inline
 *
 *********************************************************************/
#if !PRRTE_GCC_INLINE_ASSEMBLY
#define PRRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER 0
#define PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64 0
#define PRRTE_HAVE_INLINE_ATOMIC_ADD_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_AND_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_OR_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_XOR_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_SUB_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_ADD_64 0
#define PRRTE_HAVE_INLINE_ATOMIC_AND_64 0
#define PRRTE_HAVE_INLINE_ATOMIC_OR_64 0
#define PRRTE_HAVE_INLINE_ATOMIC_XOR_64 0
#define PRRTE_HAVE_INLINE_ATOMIC_SUB_64 0
#define PRRTE_HAVE_INLINE_ATOMIC_SWAP_32 0
#define PRRTE_HAVE_INLINE_ATOMIC_SWAP_64 0
#else
#define PRRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER 1
#define PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64 1
#define PRRTE_HAVE_INLINE_ATOMIC_ADD_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_AND_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_OR_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_XOR_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_SUB_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_ADD_64 1
#define PRRTE_HAVE_INLINE_ATOMIC_AND_64 1
#define PRRTE_HAVE_INLINE_ATOMIC_OR_64 1
#define PRRTE_HAVE_INLINE_ATOMIC_XOR_64 1
#define PRRTE_HAVE_INLINE_ATOMIC_SUB_64 1
#define PRRTE_HAVE_INLINE_ATOMIC_SWAP_32 1
#define PRRTE_HAVE_INLINE_ATOMIC_SWAP_64 1
#endif

/**
 * Enumeration of lock states
 */
enum {
    PRRTE_ATOMIC_LOCK_UNLOCKED = 0,
    PRRTE_ATOMIC_LOCK_LOCKED = 1
};

#define PRRTE_ATOMIC_LOCK_INIT {.u = {.lock = PRRTE_ATOMIC_LOCK_UNLOCKED}}

/**********************************************************************
 *
 * Load the appropriate architecture files and set some reasonable
 * default values for our support
 *
 *********************************************************************/
#if defined(DOXYGEN)
/* don't include system-level gorp when generating doxygen files */
#elif PRRTE_ASSEMBLY_BUILTIN == PRRTE_BUILTIN_SYNC
#include "src/sys/sync_builtin/atomic.h"
#elif PRRTE_ASSEMBLY_BUILTIN == PRRTE_BUILTIN_GCC
#include "src/sys/gcc_builtin/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_X86_64
#include "src/sys/x86_64/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_ARM
#include "src/sys/arm/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_ARM64
#include "src/sys/arm64/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_IA32
#include "src/sys/ia32/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_IA64
#include "src/sys/ia64/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_MIPS
#include "src/sys/mips/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_POWERPC32
#include "src/sys/powerpc/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_POWERPC64
#include "src/sys/powerpc/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_SPARC
#include "src/sys/sparc/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_SPARCV9_32
#include "src/sys/sparcv9/atomic.h"
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_SPARCV9_64
#include "src/sys/sparcv9/atomic.h"
#endif

#ifndef DOXYGEN
/* compare and set operations can't really be emulated from software,
   so if these defines aren't already set, they should be set to 0
   now */
#ifndef PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32
#define PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 0
#endif
#ifndef PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64
#define PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64 0
#endif
#ifndef PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128
#define PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128 0
#endif
#ifndef PRRTE_HAVE_ATOMIC_LLSC_32
#define PRRTE_HAVE_ATOMIC_LLSC_32 0
#endif
#ifndef PRRTE_HAVE_ATOMIC_LLSC_64
#define PRRTE_HAVE_ATOMIC_LLSC_64 0
#endif
#endif /* DOXYGEN */

/**********************************************************************
 *
 * Memory Barriers - defined here if running doxygen or have barriers
 *                   but can't inline
 *
 *********************************************************************/
#if !defined(PRRTE_HAVE_ATOMIC_MEM_BARRIER) && !defined(DOXYGEN)
/* no way to emulate in C code */
#define PRRTE_HAVE_ATOMIC_MEM_BARRIER 0
#endif

#if defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_MEM_BARRIER
/**
 * Memory barrier
 *
 * Will use system-specific features to instruct the processor and
 * memory controller that all writes and reads that have been posted
 * before the call to \c prrte_atomic_mb() must appear to have
 * completed before the next read or write.
 *
 * \note This can have some expensive side effects, including flushing
 * the pipeline, preventing the cpu from reordering instructions, and
 * generally grinding the memory controller's performance.  Use only
 * if you need *both* read and write barriers.
 */

#if PRRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER
static inline
#endif
void prrte_atomic_mb(void);

/**
 * Read memory barrier
 *
 * Use system-specific features to instruct the processor and memory
 * conrtoller that all reads that have been posted before the call to
 * \c prrte_atomic_rmb() must appear to have been completed before the
 * next read.  Nothing is said about the ordering of writes when using
 * \c prrte_atomic_rmb().
 */

#if PRRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER
static inline
#endif
void prrte_atomic_rmb(void);

/**
 * Write memory barrier.
 *
 * Use system-specific features to instruct the processor and memory
 * conrtoller that all writes that have been posted before the call to
 * \c prrte_atomic_wmb() must appear to have been completed before the
 * next write.  Nothing is said about the ordering of reads when using
 * \c prrte_atomic_wmb().
 */

#if PRRTE_HAVE_INLINE_ATOMIC_MEM_BARRIER
static inline
#endif
void prrte_atomic_wmb(void);

#endif /* defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_MEM_BARRIER */


/**********************************************************************
 *
 * Atomic spinlocks - always inlined, if have atomic compare-and-swap
 *
 *********************************************************************/

#if !defined(PRRTE_HAVE_ATOMIC_SPINLOCKS) && !defined(DOXYGEN)
/* 0 is more like "pending" - we'll fix up at the end after all
   the static inline functions are declared */
#define PRRTE_HAVE_ATOMIC_SPINLOCKS 0
#endif

#if defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_SPINLOCKS || (PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64)

/**
 * Initialize a lock to value
 *
 * @param lock         Address of the lock
 * @param value        Initial value to set lock to
 */
#if PRRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#endif
void prrte_atomic_lock_init(prrte_atomic_lock_t* lock, int32_t value);


/**
 * Try to acquire a lock.
 *
 * @param lock          Address of the lock.
 * @return              0 if the lock was acquired, 1 otherwise.
 */
#if PRRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#endif
int prrte_atomic_trylock(prrte_atomic_lock_t *lock);


/**
 * Acquire a lock by spinning.
 *
 * @param lock          Address of the lock.
 */
#if PRRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#endif
void prrte_atomic_lock(prrte_atomic_lock_t *lock);


/**
 * Release a lock.
 *
 * @param lock          Address of the lock.
 */
#if PRRTE_HAVE_ATOMIC_SPINLOCKS == 0
static inline
#endif
void prrte_atomic_unlock(prrte_atomic_lock_t *lock);


#if PRRTE_HAVE_ATOMIC_SPINLOCKS == 0
#undef PRRTE_HAVE_ATOMIC_SPINLOCKS
#define PRRTE_HAVE_ATOMIC_SPINLOCKS (PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64)
#define PRRTE_NEED_INLINE_ATOMIC_SPINLOCKS 1
#endif

#endif /* PRRTE_HAVE_ATOMIC_SPINLOCKS */


/**********************************************************************
 *
 * Atomic math operations
 *
 *********************************************************************/
#if !defined(PRRTE_HAVE_ATOMIC_CMPSET_32) && !defined(DOXYGEN)
#define PRRTE_HAVE_ATOMIC_CMPSET_32 0
#endif
#if defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_CMPSET_32

#if PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32
static inline
#endif
bool prrte_atomic_compare_exchange_strong_32 (prrte_atomic_int32_t *addr, int32_t *oldval,
                                             int32_t newval);

#if PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32
static inline
#endif
bool prrte_atomic_compare_exchange_strong_acq_32 (prrte_atomic_int32_t *addr, int32_t *oldval,
                                                 int32_t newval);

#if PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_32
static inline
#endif
bool prrte_atomic_compare_exchange_strong_rel_32 (prrte_atomic_int32_t *addr, int32_t *oldval,
                                                 int32_t newval);
#endif


#if !defined(PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64) && !defined(DOXYGEN)
#define PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64 0
#endif
#if defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64

#if PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64
static inline
#endif
bool prrte_atomic_compare_exchange_strong_64 (prrte_atomic_int64_t *addr, int64_t *oldval,
                                             int64_t newval);

#if PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64
static inline
#endif
bool prrte_atomic_compare_exchange_strong_acq_64 (prrte_atomic_int64_t *addr, int64_t *oldval,
                                                 int64_t newval);

#if PRRTE_HAVE_INLINE_ATOMIC_COMPARE_EXCHANGE_64
static inline
#endif
bool prrte_atomic_compare_exchange_strong_rel_64 (prrte_atomic_int64_t *addr, int64_t *oldval,
                                                 int64_t newval);

#endif

#if !defined(PRRTE_HAVE_ATOMIC_MATH_32) && !defined(DOXYGEN)
  /* define to 0 for these tests.  WIll fix up later. */
  #define PRRTE_HAVE_ATOMIC_MATH_32 0
#endif

#if defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_MATH_32 || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32

static inline int32_t prrte_atomic_add_fetch_32(prrte_atomic_int32_t *addr, int delta);
static inline int32_t prrte_atomic_fetch_add_32(prrte_atomic_int32_t *addr, int delta);
static inline int32_t prrte_atomic_and_fetch_32(prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_fetch_and_32(prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_or_fetch_32(prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_fetch_or_32(prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_xor_fetch_32(prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_fetch_xor_32(prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_sub_fetch_32(prrte_atomic_int32_t *addr, int delta);
static inline int32_t prrte_atomic_fetch_sub_32(prrte_atomic_int32_t *addr, int delta);
static inline int32_t prrte_atomic_min_fetch_32 (prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_fetch_min_32 (prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_max_fetch_32 (prrte_atomic_int32_t *addr, int32_t value);
static inline int32_t prrte_atomic_fetch_max_32 (prrte_atomic_int32_t *addr, int32_t value);

#endif /* PRRTE_HAVE_ATOMIC_MATH_32 */

#if ! PRRTE_HAVE_ATOMIC_MATH_32
/* fix up the value of prrte_have_atomic_math_32 to allow for C versions */
#undef PRRTE_HAVE_ATOMIC_MATH_32
#define PRRTE_HAVE_ATOMIC_MATH_32 PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32
#endif

#ifndef PRRTE_HAVE_ATOMIC_MATH_64
/* define to 0 for these tests.  WIll fix up later. */
#define PRRTE_HAVE_ATOMIC_MATH_64 0
#endif

#if defined(DOXYGEN) || PRRTE_HAVE_ATOMIC_MATH_64 || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64

static inline int64_t prrte_atomic_add_fetch_64(prrte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prrte_atomic_fetch_add_64(prrte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prrte_atomic_and_fetch_64(prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_fetch_and_64(prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_or_fetch_64(prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_fetch_or_64(prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_fetch_xor_64(prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_sub_fetch_64(prrte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prrte_atomic_fetch_sub_64(prrte_atomic_int64_t *addr, int64_t delta);
static inline int64_t prrte_atomic_min_fetch_64 (prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_fetch_min_64 (prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_max_fetch_64 (prrte_atomic_int64_t *addr, int64_t value);
static inline int64_t prrte_atomic_fetch_max_64 (prrte_atomic_int64_t *addr, int64_t value);

#endif /* PRRTE_HAVE_ATOMIC_MATH_64 */

#if ! PRRTE_HAVE_ATOMIC_MATH_64
/* fix up the value of prrte_have_atomic_math_64 to allow for C versions */
#undef PRRTE_HAVE_ATOMIC_MATH_64
#define PRRTE_HAVE_ATOMIC_MATH_64 PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64
#endif

/* provide a size_t add/subtract.  When in debug mode, make it an
 * inline function so that we don't have any casts in the
 *  interface and can catch type errors.  When not in debug mode,
 * just make it a macro, so that there's no performance penalty
 */
#if defined(DOXYGEN) || PRRTE_ENABLE_DEBUG
static inline size_t
prrte_atomic_add_fetch_size_t(prrte_atomic_size_t *addr, size_t delta)
{
#if SIZEOF_SIZE_T == 4
    return (size_t) prrte_atomic_add_fetch_32((int32_t*) addr, delta);
#elif SIZEOF_SIZE_T == 8
    return (size_t) prrte_atomic_add_fetch_64((int64_t*) addr, delta);
#else
#error "Unknown size_t size"
#endif
}

static inline size_t
prrte_atomic_fetch_add_size_t(prrte_atomic_size_t *addr, size_t delta)
{
#if SIZEOF_SIZE_T == 4
    return (size_t) prrte_atomic_fetch_add_32((int32_t*) addr, delta);
#elif SIZEOF_SIZE_T == 8
    return (size_t) prrte_atomic_fetch_add_64((int64_t*) addr, delta);
#else
#error "Unknown size_t size"
#endif
}

static inline size_t
prrte_atomic_sub_fetch_size_t(prrte_atomic_size_t *addr, size_t delta)
{
#if SIZEOF_SIZE_T == 4
    return (size_t) prrte_atomic_sub_fetch_32((int32_t*) addr, delta);
#elif SIZEOF_SIZE_T == 8
    return (size_t) prrte_atomic_sub_fetch_64((int64_t*) addr, delta);
#else
#error "Unknown size_t size"
#endif
}

static inline size_t
prrte_atomic_fetch_sub_size_t(prrte_atomic_size_t *addr, size_t delta)
{
#if SIZEOF_SIZE_T == 4
    return (size_t) prrte_atomic_fetch_sub_32((int32_t*) addr, delta);
#elif SIZEOF_SIZE_T == 8
    return (size_t) prrte_atomic_fetch_sub_64((int64_t*) addr, delta);
#else
#error "Unknown size_t size"
#endif
}

#else
#if SIZEOF_SIZE_T == 4
#define prrte_atomic_add_fetch_size_t(addr, delta) ((size_t) prrte_atomic_add_fetch_32((prrte_atomic_int32_t *) addr, delta))
#define prrte_atomic_fetch_add_size_t(addr, delta) ((size_t) prrte_atomic_fetch_add_32((prrte_atomic_int32_t *) addr, delta))
#define prrte_atomic_sub_fetch_size_t(addr, delta) ((size_t) prrte_atomic_sub_fetch_32((prrte_atomic_int32_t *) addr, delta))
#define prrte_atomic_fetch_sub_size_t(addr, delta) ((size_t) prrte_atomic_fetch_sub_32((prrte_atomic_int32_t *) addr, delta))
#elif SIZEOF_SIZE_T == 8
#define prrte_atomic_add_fetch_size_t(addr, delta) ((size_t) prrte_atomic_add_fetch_64((prrte_atomic_int64_t *) addr, delta))
#define prrte_atomic_fetch_add_size_t(addr, delta) ((size_t) prrte_atomic_fetch_add_64((prrte_atomic_int64_t *) addr, delta))
#define prrte_atomic_sub_fetch_size_t(addr, delta) ((size_t) prrte_atomic_sub_fetch_64((prrte_atomic_int64_t *) addr, delta))
#define prrte_atomic_fetch_sub_size_t(addr, delta) ((size_t) prrte_atomic_fetch_sub_64((prrte_atomic_int64_t *) addr, delta))
#else
#error "Unknown size_t size"
#endif
#endif

#if defined(DOXYGEN) || (PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64)
/* these are always done with inline functions, so always mark as
   static inline */

static inline bool prrte_atomic_compare_exchange_strong_xx (prrte_atomic_intptr_t *addr, intptr_t *oldval,
                                                           int64_t newval, size_t length);
static inline bool prrte_atomic_compare_exchange_strong_acq_xx (prrte_atomic_intptr_t *addr, intptr_t *oldval,
                                                               int64_t newval, size_t length);
static inline bool prrte_atomic_compare_exchange_strong_rel_xx (prrte_atomic_intptr_t *addr, intptr_t *oldval,
                                                               int64_t newval, size_t length);


static inline bool prrte_atomic_compare_exchange_strong_ptr (prrte_atomic_intptr_t* addr, intptr_t *oldval,
                                                            intptr_t newval);
static inline bool prrte_atomic_compare_exchange_strong_acq_ptr (prrte_atomic_intptr_t* addr, intptr_t *oldval,
                                                                intptr_t newval);
static inline bool prrte_atomic_compare_exchange_strong_rel_ptr (prrte_atomic_intptr_t* addr, intptr_t *oldval,
                                                                intptr_t newval);

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
 * See prrte_atomic_compare_exchange_* for pseudo-code.
 */
#define prrte_atomic_compare_exchange_strong( ADDR, OLDVAL, NEWVAL )                  \
    prrte_atomic_compare_exchange_strong_xx( (prrte_atomic_intptr_t*)(ADDR), (intptr_t *)(OLDVAL), \
                                            (intptr_t)(NEWVAL), sizeof(*(ADDR)) )

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
 * See prrte_atomic_compare_exchange_acq_* for pseudo-code.
 */
#define prrte_atomic_compare_exchange_strong_acq( ADDR, OLDVAL, NEWVAL )                  \
    prrte_atomic_compare_exchange_strong_acq_xx( (prrte_atomic_intptr_t*)(ADDR), (intptr_t *)(OLDVAL), \
                                                (intptr_t)(NEWVAL), sizeof(*(ADDR)) )

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
 * See prrte_atomic_compare_exchange_rel_* for pseudo-code.
 */
#define prrte_atomic_compare_exchange_strong_rel( ADDR, OLDVAL, NEWVAL ) \
    prrte_atomic_compare_exchange_strong_rel_xx( (prrte_atomic_intptr_t*)(ADDR), (intptr_t *)(OLDVAL), \
                                                (intptr_t)(NEWVAL), sizeof(*(ADDR)) )


#endif /* (PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_32 || PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_64) */

#if defined(DOXYGEN) || (PRRTE_HAVE_ATOMIC_MATH_32 || PRRTE_HAVE_ATOMIC_MATH_64)

static inline void prrte_atomic_add_xx(prrte_atomic_intptr_t* addr,
                                      int32_t value, size_t length);
static inline void prrte_atomic_sub_xx(prrte_atomic_intptr_t* addr,
                                      int32_t value, size_t length);

static inline intptr_t prrte_atomic_add_fetch_ptr( prrte_atomic_intptr_t* addr, void* delta );
static inline intptr_t prrte_atomic_fetch_add_ptr( prrte_atomic_intptr_t* addr, void* delta );
static inline intptr_t prrte_atomic_sub_fetch_ptr( prrte_atomic_intptr_t* addr, void* delta );
static inline intptr_t prrte_atomic_fetch_sub_ptr( prrte_atomic_intptr_t* addr, void* delta );

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
#define prrte_atomic_add( ADDR, VALUE )                                  \
   prrte_atomic_add_xx( (prrte_atomic_intptr_t*)(ADDR), (int32_t)(VALUE), \
                       sizeof(*(ADDR)) )

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
#define prrte_atomic_sub( ADDR, VALUE )                                  \
   prrte_atomic_sub_xx( (prrte_atomic_intptr_t*)(ADDR), (int32_t)(VALUE),        \
                      sizeof(*(ADDR)) )

#endif /* PRRTE_HAVE_ATOMIC_MATH_32 || PRRTE_HAVE_ATOMIC_MATH_64 */


/*
 * Include inline implementations of everything not defined directly
 * in assembly
 */
#include "src/sys/atomic_impl.h"

#endif /* !PRRTE_C_HAVE__ATOMIC */

END_C_DECLS

#endif /* PRRTE_SYS_ATOMIC_H */
