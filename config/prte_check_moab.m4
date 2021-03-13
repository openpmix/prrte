# -*- shell-script -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRTE_CHECK_MOAB(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_MOAB],[
    if test -z "$prte_check_moab_happy" ; then
        PRTE_VAR_SCOPE_PUSH([prte_check_moab_$1_save_CPPFLAGS prte_check_moab_$1_save_LDFLAGS prte_check_moab_$1_save_LIBS])

        AC_ARG_WITH([moab],
                    [AS_HELP_STRING([--with-moab],
                                    [Build MOAB scheduler component (default: yes)])])
        PRTE_CHECK_WITHDIR([moab], [$with_moab], [mapi.h])
        AC_ARG_WITH([moab-libdir],
                    [AS_HELP_STRING([--with-moab-libdir=DIR],
                    [Search for Moab libraries in DIR])])
        PRTE_CHECK_WITHDIR([moab-libdir], [$with_moab_libdir], [libmoab.*])

        prte_check_moab_happy="yes"
        AS_IF([test "$with_moab" = "no"],
              [prte_check_moab_happy=no])


        AS_IF([test $prte_check_moab_happy = yes],
              [AC_MSG_CHECKING([looking for moab in])
               AS_IF([test "$with_moab" != "yes"],
                     [prte_moab_dir=$with_moab
                      AC_MSG_RESULT([($prte_moab_dir)])],
                     [AC_MSG_RESULT([(default search paths)])])
               AS_IF([test ! -z "$with_moab_libdir" && \
                      test "$with_moab_libdir" != "yes"],
                           [prte_moab_libdir=$with_moab_libdir])
              ])

        prte_check_moab_$1_save_CPPFLAGS=$CPPFLAGS
        prte_check_moab_$1_save_LDFLAGS=$LDFLAGS
        prte_check_moab_$1_save_LIBS=$LIBS

        AS_IF([test $prte_check_moab_happy = yes],
              [PRTE_CHECK_PACKAGE([prte_check_moab],
                                  [mapi.h],
                                  [cmoab],
                                  [MCCJobGetRemainingTime],
                                  [],
                                  [$prte_moab_dir],
                                  [$prte_moab_libdir],
                                  [],
                                  [prte_check_moab_happy=no])])

        CPPFLAGS=$prte_check_moab_$1_save_CPPFLAGS
        LDFLAGS=$prte_check_moab_$1_save_LDFLAGS
        LIBS=$prte_check_moab_$1_save_LIBS

        PRTE_SUMMARY_ADD([[Resource Managers]],[[Moab]],[$1],[$prte_check_moab_happy])
        PRTE_VAR_SCOPE_POP
    fi

    if test $prte_check_moab_happy = yes ; then
        $1_CPPFLAGS="[$]$1_CPPFLAGS $prte_check_moab_CPPFLAGS"
        $1_LIBS="[$]$1_LIBS $prte_check_moab_LIBS"
        $1_LDFLAGS="[$]$1_LDFLAGS $prte_check_moab_LDFLAGS"

        AC_SUBST($1_CPPFLAGS)
        AC_SUBST($1_LDFLAGS)
        AC_SUBST($1_LIBS)
    fi

    AS_IF([test "$prte_check_moab_happy" = "yes"],
          [$2],
          [$3])
])
