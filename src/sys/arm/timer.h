/*
 * Copyright (c) 2008      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
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

#include <sys/times.h>

typedef uint64_t prrte_timer_t;

static inline prrte_timer_t
prrte_sys_timer_get_cycles(void)
{
    prrte_timer_t ret;
    struct tms accurate_clock;

    times(&accurate_clock);
    ret = accurate_clock.tms_utime + accurate_clock.tms_stime;

    return ret;
}

#define PRRTE_HAVE_SYS_TIMER_GET_CYCLES 1

#endif /* ! PRRTE_SYS_ARCH_TIMER_H */
