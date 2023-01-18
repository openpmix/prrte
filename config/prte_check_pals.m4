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
dnl Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2015      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2019      Intel, Inc.  All rights reserved.
dnl Copyright (c) 2020-2023 Triad National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PRTE_CHECK_PALS(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_PALS],[

    AC_ARG_WITH([pals],
                [AS_HELP_STRING([--with-pals(=DIR|yes|no)],
                [Build with PALS (HPE/Cray) application launcher (default: auto)])],[],with_pals=auto)

    AC_MSG_CHECKING([for HPE PALS support])

dnl  We do not currently use libpals.so but this is a good sentinel for the pals runtime being installed 
dnl  so we check this way for presence of HPE/Cray PALS system
    AS_IF([test "$with_pals" != "no"],
          [OAC_CHECK_PACKAGE([libpals],
                             [$1],
                             [pals.h],
                             [pals],
                             [pals_get_apid],
                             [prte_check_pals_happy="yes"],
                             [prte_check_pals_happy="no"])
              ])

   AC_MSG_RESULT([prte_check_pals_happy = $prte_check_pals_happy $with_pals])

   AS_IF([test "$prte_check_pals_happy" = "yes"],
         [$2],
         [AS_IF([test ! -z "$with_pals" && test "$with_pals" != "no" && test "$with_pals" != "auto"],
               [AC_MSG_ERROR([PALS support requested but not found.  Aborting])])
         $3])
])
