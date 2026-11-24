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

# PRTE_SETUP_PBS
# --------------------------------------------------------
# Decide, once, whether PBS support is being built: --with-pbs, or
# --with-tm, which asks for PBS daemon launch and so for PBS support as
# well. Sets prte_check_pbs_happy. Everything that needs the answer gets it
# through PRTE_CHECK_PBS, which requires this, so the components that ask
# - ras/pbs and plm/tm - may be configured in either order.
AC_DEFUN([PRTE_SETUP_PBS],[
    AC_ARG_WITH([pbs],
           [AS_HELP_STRING([--with-pbs],
                           [Build PBS support (default: no). Launching the daemons as PBS tasks also needs PBS's TM library, which is then required: see --with-tm and --without-tm])])

    prte_check_pbs_why=
    if test "$with_pbs" = "no" ; then
        prte_check_pbs_happy="no"
        AS_IF([test -n "$with_tm" && test "$with_tm" != "no"],
              [AC_MSG_WARN([--with-tm asks for daemon launch through PBS's TM])
               AC_MSG_WARN([library, which needs PBS support, but --without-pbs])
               AC_MSG_WARN([was also given. Drop one of the two.])
               AC_MSG_ERROR([Cannot continue])])
    elif test -n "$with_pbs" ; then
        prte_check_pbs_happy="yes"
    elif test -n "$with_tm" && test "$with_tm" != "no" ; then
        # a PBS launcher without the PBS allocator would take its nodes
        # from somewhere other than the job, and could start daemons on
        # none of them that are not in it
        prte_check_pbs_happy="yes"
        prte_check_pbs_why=" (implied by --with-tm)"
    else
        prte_check_pbs_happy="no"
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

    PRTE_SUMMARY_ADD([Resource Managers], [PBS], [], [${prte_check_pbs_happy}${prte_check_pbs_why}])
])

# PRTE_CHECK_PBS(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_PBS],[
    AC_REQUIRE([PRTE_SETUP_PBS])

    AS_IF([test "$prte_check_pbs_happy" = "yes"],
          [$2],
          [$3])
])
