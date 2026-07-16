dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2026      Nanook Consulting  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# See if we have sched_setaffinity(2) and the cpu_set_t API (a glibc/Linux
# bundle).  When available, the odls child can bind itself just before
# execve() by issuing a bare sched_setaffinity() syscall - which is
# async-signal-safe - instead of calling hwloc, which allocates.  On
# platforms without it (e.g., macOS) the child falls back to hwloc.

AC_DEFUN([PRTE_CHECK_SETAFFINITY],[

    PRTE_VAR_SCOPE_PUSH([prte_have_sched_setaffinity])

    prte_have_sched_setaffinity=0

    AC_CHECK_HEADERS([sched.h])

    AC_CHECK_FUNC([sched_setaffinity],
                  [AC_MSG_CHECKING([for usable cpu_set_t and CPU_ALLOC API])
                   AC_COMPILE_IFELSE(
                       [AC_LANG_PROGRAM(
                           [[#ifndef _GNU_SOURCE
                             #define _GNU_SOURCE
                             #endif
                             #include <sched.h>]],
                           [[cpu_set_t *set = CPU_ALLOC(128);
                             size_t setsize = CPU_ALLOC_SIZE(128);
                             CPU_ZERO_S(setsize, set);
                             CPU_SET_S(1, setsize, set);
                             (void) sched_setaffinity(0, setsize, set);
                             CPU_FREE(set);]])
                       ],
                       [AC_MSG_RESULT([yes])
                        prte_have_sched_setaffinity=1],
                       [AC_MSG_RESULT([no])
                        prte_have_sched_setaffinity=0])
                  ],
                  [prte_have_sched_setaffinity=0])

    AC_DEFINE_UNQUOTED([PRTE_HAVE_SCHED_SETAFFINITY], [$prte_have_sched_setaffinity],
        [Whether we have sched_setaffinity() and the cpu_set_t API for async-signal-safe CPU binding])

    PRTE_VAR_SCOPE_POP
])
