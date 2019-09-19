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

static inline prrte_timer_t
prrte_sys_timer_get_cycles(void)
{
    prrte_timer_t ret;
    int tmp;

    __asm__ __volatile__(
                         "xchgl %%ebx, %1\n"
                         "cpuid\n"
                         "xchgl %%ebx, %1\n"
                         "rdtsc\n"
                         : "=A"(ret), "=r"(tmp)
                         :: "ecx");

    return ret;
}

#define PRRTE_HAVE_SYS_TIMER_GET_CYCLES 1

#else

#define PRRTE_HAVE_SYS_TIMER_GET_CYCLES 0

#endif /* PRRTE_GCC_INLINE_ASSEMBLY */

#endif /* ! PRRTE_SYS_ARCH_TIMER_H */
