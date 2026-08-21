dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2026      Nanook Consulting  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# See if we can issue set_mempolicy(2) directly.  glibc ships no wrapper
# for it, so the only ways to reach it are libnuma (which allocates) or
# the bare syscall - and the bare syscall is what hwloc itself does
# underneath hwloc_set_membind().  When it is available, the odls child
# can apply the job's memory-binding policy just before execve() without
# calling into hwloc, which allocates and therefore cannot be used in the
# async-signal-safe window after fork().  On platforms without it the
# child falls back to hwloc.

AC_DEFUN([PRTE_CHECK_SET_MEMPOLICY],[

    PRTE_VAR_SCOPE_PUSH([prte_have_set_mempolicy])

    prte_have_set_mempolicy=0

    AC_MSG_CHECKING([for a usable set_mempolicy(2) syscall])
    AC_LINK_IFELSE(
        [AC_LANG_PROGRAM(
            [[#include <sys/syscall.h>
              #include <unistd.h>]],
            [[unsigned long mask = 1;
              (void) syscall(__NR_set_mempolicy, 0, (void *) 0, 0UL);
              (void) syscall(__NR_set_mempolicy, 2, &mask, 1UL);]])
        ],
        [AC_MSG_RESULT([yes])
         prte_have_set_mempolicy=1],
        [AC_MSG_RESULT([no])
         prte_have_set_mempolicy=0])

    AC_DEFINE_UNQUOTED([PRTE_HAVE_SET_MEMPOLICY], [$prte_have_set_mempolicy],
        [Whether we can issue set_mempolicy(2) directly for async-signal-safe memory binding])

    PRTE_VAR_SCOPE_POP
])
