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
# Copyright (c) 2009-2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRRTE_CHECK_MOAB(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRRTE_CHECK_MOAB],[
    if test -z "$prrte_check_moab_happy" ; then
        PRRTE_VAR_SCOPE_PUSH([prrte_check_moab_$1_save_CPPFLAGS prrte_check_moab_$1_save_LDFLAGS prrte_check_moab_$1_save_LIBS])

        AC_ARG_WITH([moab],
                    [AC_HELP_STRING([--with-moab],
                                    [Build MOAB scheduler component (default: yes)])])
        PRRTE_CHECK_WITHDIR([moab], [$with_moab], [mapi.h])
        AC_ARG_WITH([moab-libdir],
                    [AC_HELP_STRING([--with-moab-libdir=DIR],
                    [Search for Moab libraries in DIR])])
        PRRTE_CHECK_WITHDIR([moab-libdir], [$with_moab_libdir], [libmoab.*])

        prrte_check_moab_happy="yes"
        AS_IF([test "$with_moab" = "no"],
              [prrte_check_moab_happy=no])


        AS_IF([test $prrte_check_moab_happy = yes],
              [AC_MSG_CHECKING([looking for moab in])
               AS_IF([test "$with_moab" != "yes"],
                     [prrte_moab_dir=$with_moab
                      AC_MSG_RESULT([($prrte_moab_dir)])],
                     [AC_MSG_RESULT([(default search paths)])])
               AS_IF([test ! -z "$with_moab_libdir" && \
                      test "$with_moab_libdir" != "yes"],
                           [prrte_moab_libdir=$with_moab_libdir])
              ])

        prrte_check_moab_$1_save_CPPFLAGS=$CPPFLAGS
        prrte_check_moab_$1_save_LDFLAGS=$LDFLAGS
        prrte_check_moab_$1_save_LIBS=$LIBS

        AS_IF([test $prrte_check_moab_happy = yes],
              [PRRTE_CHECK_PACKAGE([prrte_check_moab],
                                  [mapi.h],
                                  [cmoab],
                                  [MCCJobGetRemainingTime],
                                  [],
                                  [$prrte_moab_dir],
                                  [$prrte_moab_libdir],
                                  [],
                                  [prrte_check_moab_happy=no])])

        CPPFLAGS=$prrte_check_moab_$1_save_CPPFLAGS
        LDFLAGS=$prrte_check_moab_$1_save_LDFLAGS
        LIBS=$prrte_check_moab_$1_save_LIBS

        PRRTE_SUMMARY_ADD([[Resource Managers]],[[Moab]],[$1],[$prrte_check_moab_happy])
        PRRTE_VAR_SCOPE_POP
    fi

    if test $prrte_check_moab_happy = yes ; then
        $1_CPPFLAGS="[$]$1_CPPFLAGS $prrte_check_moab_CPPFLAGS"
        $1_LIBS="[$]$1_LIBS $prrte_check_moab_LIBS"
        $1_LDFLAGS="[$]$1_LDFLAGS $prrte_check_moab_LDFLAGS"

        AC_SUBST($1_CPPFLAGS)
        AC_SUBST($1_LDFLAGS)
        AC_SUBST($1_LIBS)
    fi

    AS_IF([test "$prrte_check_moab_happy" = "yes"],
          [$2],
          [$3])
])
