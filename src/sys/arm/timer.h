/*
 * Copyright (c) 2008      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_SYS_ARCH_TIMER_H
#define PRTE_SYS_ARCH_TIMER_H 1

#include <sys/times.h>

typedef uint64_t prte_timer_t;

static inline prte_timer_t
prte_sys_timer_get_cycles(void)
{
    prte_timer_t ret;
    struct tms accurate_clock;

    times(&accurate_clock);
    ret = accurate_clock.tms_utime + accurate_clock.tms_stime;

    return ret;
}

#define PRTE_HAVE_SYS_TIMER_GET_CYCLES 1

#endif /* ! PRTE_SYS_ARCH_TIMER_H */
