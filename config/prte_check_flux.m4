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
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2025-2026 Triad National Security, LLC. All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRTE_CHECK_FLUX(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_FLUX],[
    PRTE_VAR_SCOPE_PUSH(prte_check_flux_jansson_happy)
    if test -z "$prte_check_flux_happy" ; then
	AC_ARG_WITH([flux],
           [AS_HELP_STRING([--with-flux(=DIR)],
                           [Build flux scheduler component (default: no)])],
            [], [with_flux=no])

	if test "$with_flux" = "no" ; then
            prte_check_flux_happy="no"
	else
            prte_check_flux_happy="yes"
        fi

dnl
dnl     first check if jansson available as this is needed by flux RAS
dnl
dnl
        AS_IF([test "$prte_check_flux_happy" = "yes"],
              [AS_IF([test -z "$with_jansson"], [with_jansson="yes"],[])
               PRTE_CHECK_JANSSON([prrte_ras_flux_jansson], [prte_check_flux_jansson_happy="yes"], [prte_check_flux_jansson_happy="no"])])
        AS_IF([test "$prte_check_flux_jansson_happy" != "yes"],
              [prte_check_flux_happy="no"])

        AS_IF([test "$prte_check_flux_happy" = "yes"],
              [OAC_CHECK_PACKAGE([flux],
                                 [$1],
                                 [flux/core.h flux/idset.h],
                                 [flux-core flux-idset flux-hostlist],
                                 [flux_open],
                                 [prte_check_flux_happy="yes"],
                                 [prte_check_flux_happy="no"])
              ])

dnl
dnl        placeholder in case we need to add some kind of flux PLM
dnl
dnl        AS_IF([test "$prte_check_flux_happy" = "yes"],
dnl              [AC_CHECK_FUNC([execve],
dnl                             [prte_check_flux_happy="yes"],
dnl                             [prte_check_flux_happy="no"])])


        PRTE_SUMMARY_ADD([Resource Managers], [flux], [], [$prte_check_flux_happy])
    fi

    AS_IF([test "$prte_check_flux_happy" = "yes"],
         [$2],
         [AS_IF([test ! -z "$with_flux" && test "$with_flux" != "no"],
               [AC_MSG_ERROR([flux support requested but not found.  Aborting])])
         $3])
    PRTE_VAR_SCOPE_POP
])

