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
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRTE_CHECK_PBS(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_PBS],[
    AC_ARG_WITH([pbs],
           [AS_HELP_STRING([--with-pbs],
                           [Build PBS scheduler support (default: no). Launching daemons as PBS tasks also needs the TM library: see --with-tm])])

    if test -z "$with_pbs" || test "$with_pbs" = "no" ; then
        prte_check_pbs_happy="no"
    else
        prte_check_pbs_happy="yes"
    fi

    AS_IF([test "$prte_check_pbs_happy" = "yes"],
          [ # PBS supports only linux
            case $host in
            *-linux*)
                    prte_check_pbs_happy="yes"
                    ;;
            *)
                    AC_MSG_WARN([PBS support was requested, but we are not on])
                    AC_MSG_WARN([a supported environment (linux). PBS support])
                    AC_MSG_WARN([therefore cannot be built. Please correct and])
                    AC_MSG_WARN([try again.])
                    AC_MSG_ERROR([Cannot continue])
                    ;;
            esac
        ])

    PRTE_SUMMARY_ADD([Resource Managers], [PBS], [], [$prte_check_pbs_happy])

    AS_IF([test "$prte_check_pbs_happy" = "yes"],
          [$2],
          [$3])
])
