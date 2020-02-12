dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2020      Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# See if there is support for ptrace options required for
# "stop-on-exec" behavior.

AC_DEFUN([PRRTE_CHECK_PTRACE],[

    PRRTE_VAR_SCOPE_PUSH(prrte_have_ptrace_traceme prrte_have_ptrace_detach prrte_have_ptrace_header prrte_have_ptrace prrte_want_stop_on_exec prrte_traceme_cmd prrte_detach_cmd prrte_ptrace_linux_sig)

    prrte_have_ptrace_traceme=no
    prrte_have_ptrace_detach=no
    prrte_traceme_cmd=
    prrte_detach_cmd=

    AC_CHECK_HEADER([sys/ptrace.h],
                    [prrte_have_ptrace_header=1],
                    [prrte_have_ptrace_header=0])
    # must manually define the header protection since check_header doesn't know it
    AC_DEFINE_UNQUOTED([HAVE_SYS_PTRACE_H], [$prrte_have_ptrace_header], [Whether or not we have the ptrace header])

    AC_CHECK_FUNC([ptrace],
                  [prrte_have_ptrace=yes],
                  [prrte_have_ptrace=no])

    if test "$prrte_have_ptrace_header" == "1" && test "$prrte_have_ptrace" == "yes"; then
        AC_MSG_CHECKING([PTRACE_TRACEME])
        AC_EGREP_CPP([yes],
                     [#include <sys/ptrace.h>
                      #ifdef PTRACE_TRACEME
                        yes
                      #endif
                     ],
                     [AC_MSG_RESULT(yes)
                      prrte_have_ptrace_traceme=yes
                      prrte_traceme_cmd=PTRACE_TRACEME],
                     [AC_MSG_RESULT(no)
                      AC_MSG_CHECKING([PT_TRACE_ME])
                      AC_EGREP_CPP([yes],
                                   [#include <sys/ptrace.h>
                                    #ifdef PT_TRACE_ME
                                      yes
                                    #endif
                                   ],
                                   [AC_MSG_RESULT(yes)
                                    prrte_have_ptrace_traceme=yes
                                    prrte_traceme_cmd=PT_TRACE_ME],
                                   [AC_MSG_RESULT(no)
                                    prrte_have_ptrace_traceme=no])
                     ])

        AC_MSG_CHECKING([PTRACE_DETACH])
        AC_EGREP_CPP([yes],
                     [#include <sys/ptrace.h>
                      #ifdef PTRACE_DETACH
                        yes
                      #endif
                     ],
                     [AC_MSG_RESULT(yes)
                      prrte_have_ptrace_detach=yes
                      prrte_detach_cmd=PTRACE_DETACH],
                     [AC_MSG_RESULT(no)
                      AC_MSG_CHECKING(PT_DETACH)
                      AC_EGREP_CPP([yes],
                                   [#include <sys/ptrace.h>
                                    #ifdef PT_DETACH
                                      yes
                                    #endif
                                   ],
                                   [AC_MSG_RESULT(yes)
                                    prrte_have_ptrace_detach=yes
                                    prrte_detach_cmd=PT_DETACH],
                                   [AC_MSG_RESULT(no)
                                    prrte_have_ptrace_detach=no])
                     ])

        AC_MSG_CHECKING([Linux ptrace function signature])
        AC_LANG_PUSH(C)
        AC_COMPILE_IFELSE(
            [AC_LANG_PROGRAM(
                [[#include "sys/ptrace.h"]],
                [[long (*ptr)(enum __ptrace_request request, pid_t pid, void *addr, void *data);]
                 [ptr = ptrace;]])
            ],[
                AC_MSG_RESULT([yes])
                prrte_ptrace_linux_sig=1
            ],[
                AC_MSG_RESULT([no])
                prrte_ptrace_linux_sig=0
            ])
        AC_LANG_POP(C)

    fi

    AC_MSG_CHECKING(ptrace stop-on-exec will be supported)
    AS_IF([test "$prrte_have_ptrace_traceme" == "yes" && test "$prrte_have_ptrace_detach" == "yes"],
          [AC_MSG_RESULT(yes)
           prrte_want_stop_on_exec=1],
          [AC_MSG_RESULT(no)
           prrte_want_stop_on_exec=0])

    AC_DEFINE_UNQUOTED([PRRTE_HAVE_LINUX_PTRACE], [$prrte_ptrace_linux_sig], [Does ptrace have the Linux signature])
    AC_DEFINE_UNQUOTED([PRRTE_HAVE_STOP_ON_EXEC], [$prrte_want_stop_on_exec], [Whether or not we have stop-on-exec support])
    AC_DEFINE_UNQUOTED([PRRTE_TRACEME], [$prrte_traceme_cmd], [Command for declaring that process expects to be traced by parent])
    AC_DEFINE_UNQUOTED([PRRTE_DETACH], [$prrte_detach_cmd], [Command to detach from process being traced])

    PRRTE_VAR_SCOPE_POP
])
