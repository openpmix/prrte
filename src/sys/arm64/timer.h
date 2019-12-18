/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2008      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2016      Broadcom Limited. All rights reserved.
 * Copyright (c) 2016      Los Alamos National Security, LLC. All rights
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

static inline prrte_timer_t
prrte_sys_timer_get_cycles(void)
{
    prrte_timer_t ret;

    __asm__ __volatile__ ("isb" ::: "memory");
    __asm__ __volatile__ ("mrs %0,  CNTVCT_EL0" : "=r" (ret));

    return ret;
}


static inline prrte_timer_t
prrte_sys_timer_get_freq(void)
{
    prrte_timer_t freq;
    __asm__ __volatile__ ("mrs %0,  CNTFRQ_EL0" : "=r" (freq));
    return (prrte_timer_t)(freq);
}

#define PRRTE_HAVE_SYS_TIMER_GET_CYCLES 1
#define PRRTE_HAVE_SYS_TIMER_GET_FREQ 1

#endif /* ! PRRTE_SYS_ARCH_TIMER_H */
