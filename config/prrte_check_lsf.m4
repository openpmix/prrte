dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2007-2016 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2015      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017      IBM Corporation.  All rights reserved.
dnl Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# check for lsf
# PRRTE_CHECK_LSF(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRRTE_CHECK_LSF],[
    AS_IF([test -z "$prrte_check_lsf_happy"],[
       AC_ARG_WITH([lsf],
               [AC_HELP_STRING([--with-lsf(=DIR)],
                       [Build LSF support])])
       PRRTE_CHECK_WITHDIR([lsf], [$with_lsf], [include/lsf/lsbatch.h])
       AC_ARG_WITH([lsf-libdir],
               [AC_HELP_STRING([--with-lsf-libdir=DIR],
                       [Search for LSF libraries in DIR])])
       PRRTE_CHECK_WITHDIR([lsf-libdir], [$with_lsf_libdir], [libbat.*])

       AS_IF([test "$with_lsf" != "no"],[
          # Defaults
          prrte_check_lsf_dir_msg="compiler default"
          prrte_check_lsf_libdir_msg="linker default"

          # Save directory names if supplied
          AS_IF([test ! -z "$with_lsf" && test "$with_lsf" != "yes"],
                    [prrte_check_lsf_dir="$with_lsf"
                     prrte_check_lsf_dir_msg="$prrte_check_lsf_dir (from --with-lsf)"])
          AS_IF([test ! -z "$with_lsf_libdir" && test "$with_lsf_libdir" != "yes"],
                    [prrte_check_lsf_libdir="$with_lsf_libdir"
                     prrte_check_lsf_libdir_msg="$prrte_check_lsf_libdir (from --with-lsf-libdir)"])

          # If no directories were specified, look for LSF_LIBDIR,
          # LSF_INCLUDEDIR, and/or LSF_ENVDIR.
          AS_IF([test -z "$prrte_check_lsf_dir" && test -z "$prrte_check_lsf_libdir"],
                    [AS_IF([test ! -z "$LSF_ENVDIR" && test -z "$LSF_LIBDIR" && test -f "$LSF_ENVDIR/lsf.conf"],
                           [LSF_LIBDIR=`egrep ^LSF_LIBDIR= $LSF_ENVDIR/lsf.conf | cut -d= -f2-`])
                     AS_IF([test ! -z "$LSF_ENVDIR" && test -z "$LSF_INCLUDEDIR" && test -f "$LSF_ENVDIR/lsf.conf"],
                           [LSF_INCLUDEDIR=`egrep ^LSF_INCLUDEDIR= $LSF_ENVDIR/lsf.conf | cut -d= -f2-`])
                     AS_IF([test ! -z "$LSF_LIBDIR"],
                           [prrte_check_lsf_libdir=$LSF_LIBDIR
                            prrte_check_lsf_libdir_msg="$LSF_LIBDIR (from \$LSF_LIBDIR)"])
                     AS_IF([test ! -z "$LSF_INCLUDEDIR"],
                           [prrte_check_lsf_dir=`dirname $LSF_INCLUDEDIR`
                            prrte_check_lsf_dir_msg="$prrte_check_lsf_dir (from \$LSF_INCLUDEDIR)"])])

          AS_IF([test "$with_lsf" = "no"],
                    [prrte_check_lsf_happy="no"],
                    [prrte_check_lsf_happy="yes"])

          prrte_check_lsf_$1_save_CPPFLAGS="$CPPFLAGS"
          prrte_check_lsf_$1_save_LDFLAGS="$LDFLAGS"
          prrte_check_lsf_$1_save_LIBS="$LIBS"

          # liblsf requires yp_all, yp_get_default_domain, and ypprot_err
          # on Linux, Solaris, NEC, and Sony NEWSs these are found in libnsl
          # on AIX it should be in libbsd
          # on HP-UX it should be in libBSD
          # on IRIX < 6 it should be in libsun (IRIX 6 and later it is in libc)
          PRRTE_SEARCH_LIBS_COMPONENT([yp_all_nsl], [yp_all], [nsl bsd BSD sun],
                         [yp_all_nsl_happy="yes"],
                         [yp_all_nsl_happy="no"])

          AS_IF([test "$yp_all_nsl_happy" = "no"],
                    [prrte_check_lsf_happy="no"],
                    [prrte_check_lsf_happy="yes"])

          # liblsb requires liblsf - using ls_info as a test for liblsf presence
          PRRTE_CHECK_PACKAGE([ls_info_lsf],
                     [lsf/lsf.h],
                     [lsf],
                     [ls_info],
                     [$yp_all_nsl_LIBS],
                     [$prrte_check_lsf_dir],
                     [$prrte_check_lsf_libdir],
                     [ls_info_lsf_happy="yes"],
                     [ls_info_lsf_happy="no"])

          AS_IF([test "$ls_info_lsf_happy" = "no"],
                    [prrte_check_lsf_happy="no"],
                    [prrte_check_lsf_happy="yes"])

          # test function of liblsb LSF package
          AS_IF([test "$prrte_check_lsf_happy" = "yes"],
                    [AC_MSG_CHECKING([for LSF dir])
                     AC_MSG_RESULT([$prrte_check_lsf_dir_msg])
                     AC_MSG_CHECKING([for LSF library dir])
                     AC_MSG_RESULT([$prrte_check_lsf_libdir_msg])
                     AC_MSG_CHECKING([for liblsf function])
                     AC_MSG_RESULT([$ls_info_lsf_happy])
                     AC_MSG_CHECKING([for liblsf yp requirements])
                     AC_MSG_RESULT([$yp_all_nsl_happy])
                     PRRTE_CHECK_PACKAGE([prrte_check_lsf],
                        [lsf/lsbatch.h],
                        [bat],
                        [lsb_launch],
                        [$ls_info_lsf_LIBS $yp_all_nsl_LIBS],
                        [$prrte_check_lsf_dir],
                        [$prrte_check_lsf_libdir],
                        [prrte_check_lsf_happy="yes"],
                        [prrte_check_lsf_happy="no"])])

          CPPFLAGS="$prrte_check_lsf_$1_save_CPPFLAGS"
          LDFLAGS="$prrte_check_lsf_$1_save_LDFLAGS"
          LIBS="$prrte_check_lsf_$1_save_LIBS"

       ],[prrte_check_lsf_happy=no])

       PRRTE_SUMMARY_ADD([[Resource Managers]],[[LSF]],[$1],[$prrte_check_lsf_happy])
    ])

    AS_IF([test "$prrte_check_lsf_happy" = "yes"],
          [$1_LIBS="[$]$1_LIBS $prrte_check_lsf_LIBS"
           $1_LDFLAGS="[$]$1_LDFLAGS $prrte_check_lsf_LDFLAGS"
           $1_CPPFLAGS="[$]$1_CPPFLAGS $prrte_check_lsf_CPPFLAGS"
           # add the LSF libraries to static builds as they are required
           $1_WRAPPER_EXTRA_LDFLAGS=[$]$1_LDFLAGS
           $1_WRAPPER_EXTRA_LIBS=[$]$1_LIBS
           $2],
          [AS_IF([test ! -z "$with_lsf" && test "$with_lsf" != "no"],
                 [AC_MSG_WARN([LSF support requested (via --with-lsf) but not found.])
                  AC_MSG_ERROR([Aborting.])])
           $3])
])
