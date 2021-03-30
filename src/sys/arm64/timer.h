/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2008      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2016      Broadcom Limited. All rights reserved.
 * Copyright (c) 2016      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_SYS_ARCH_TIMER_H
#define PRTE_SYS_ARCH_TIMER_H 1

typedef uint64_t prte_timer_t;

static inline prte_timer_t prte_sys_timer_get_cycles(void)
{
    prte_timer_t ret;

    __asm__ __volatile__("isb" ::: "memory");
    __asm__ __volatile__("mrs %0,  CNTVCT_EL0" : "=r"(ret));

    return ret;
}

static inline prte_timer_t prte_sys_timer_get_freq(void)
{
    prte_timer_t freq;
    __asm__ __volatile__("mrs %0,  CNTFRQ_EL0" : "=r"(freq));
    return (prte_timer_t)(freq);
}

#define PRTE_HAVE_SYS_TIMER_GET_CYCLES 1
#define PRTE_HAVE_SYS_TIMER_GET_FREQ   1

#endif /* ! PRTE_SYS_ARCH_TIMER_H */
