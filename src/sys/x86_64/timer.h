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
 * Copyright (c) 2016      Los Alamos National Security, LLC. ALl rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_SYS_ARCH_TIMER_H
#define PRRTE_SYS_ARCH_TIMER_H 1


typedef uint64_t prrte_timer_t;

/* Using RDTSC(P) results in non-monotonic timers across cores */
#undef PRRTE_TIMER_MONOTONIC
#define PRRTE_TIMER_MONOTONIC 0

#if PRRTE_GCC_INLINE_ASSEMBLY

/* TODO: add AMD mfence version and dispatch at init */
static inline prrte_timer_t
prrte_sys_timer_get_cycles(void)
{
     uint32_t l, h;
     __asm__ __volatile__ ("lfence\n\t"
                           "rdtsc\n\t"
                           : "=a" (l), "=d" (h));
     return ((prrte_timer_t)l) | (((prrte_timer_t)h) << 32);
}

static inline bool prrte_sys_timer_is_monotonic (void)
{
    int64_t tmp;
    int32_t cpuid1, cpuid2;
    const int32_t level = 0x80000007;

    /* cpuid clobbers ebx but it must be restored for -fPIC so save
     * then restore ebx */
    __asm__ volatile ("xchg %%rbx, %2\n"
                      "cpuid\n"
                      "xchg %%rbx, %2\n":
                      "=a" (cpuid1), "=d" (cpuid2), "=r" (tmp) :
                      "a" (level) :
                      "ecx", "ebx");
    /* bit 8 of edx contains the invariant tsc flag */
    return !!(cpuid2 & (1 << 8));
}

#define PRRTE_HAVE_SYS_TIMER_GET_CYCLES 1
#define PRRTE_HAVE_SYS_TIMER_IS_MONOTONIC 1

#else

#define PRRTE_HAVE_SYS_TIMER_GET_CYCLES 0

#endif /* PRRTE_GCC_INLINE_ASSEMBLY */

#endif /* ! PRRTE_SYS_ARCH_TIMER_H */
