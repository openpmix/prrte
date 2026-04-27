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
    PRTE_VAR_SCOPE_PUSH([prte_check_pbs_save_PATH])

	AC_ARG_WITH([pbs],
           [AS_HELP_STRING([--with-pbs],
                           [Build PBS scheduler and launch components (default: no)])])

    prte_check_pbs_save_PATH="$PATH"
    PRTE_PBSTRMSH_PATH=
	if test -z "$with_pbs" || test "$with_pbs" = "no" ; then
            prte_check_pbs_happy="no"
	elif test "$with_pbs" = ""  || test "$with_pbs" = "yes"; then
        prte_check_pbs_happy="yes"
    else
        prte_check_pbs_happy="yes"
        PATH="$with_pbs/bin:$PATH"
    fi

    AS_IF([test "$prte_check_pbs_happy" = "yes"],
          [ # unless user asked, only build pbs component on linux
            # as that is the platforms that PBS supports
            case $host in
            *-linux*)
                    prte_check_pbs_happy="yes"
                    ;;
            *)
                    AC_MSG_WARN([PBS support was requested, but we are not on])
                    AC_MSG_WARN([a supported environment (linux). PBS support])
                    AC_MSG_WARN([threfore cannot be built. Please correct and])
                    AC_MSG_WARN([try again.])
                    AC_MSG_ERROR([Cannot continue])
                    ;;
            esac
        ])

    AS_IF([test "$prte_check_pbs_happy" = "yes"],
          [AC_MSG_CHECKING([for pbs_tmrsh in PATH])
           PRTE_WHICH([pbs_tmrsh], [PRTE_PBSTRMSH_PATH])
           if test "$PRTE_PBSTRMSH_PATH" = ""; then
             if test "$with_pbs" != "" && test "$with_pbs" != "yes" ; then
                AC_MSG_RESULT([no])
                AC_MSG_WARN([PBS path was specified, but pbs_tmrsh was not found])
                AC_MSG_WARN([in $with_pbs/bin. Please correct the path and try again])
                AC_MSG_ERROR([Cannot continue])
             else
                AC_MSG_RESULT([no])
             fi
           else
             AC_MSG_RESULT([yes: $PRTE_PBSTRMSH_PATH])
           fi
          ])

    PATH="$prte_check_pbs_save_PATH"
    PRTE_SUMMARY_ADD([Resource Managers], [PBS], [], [$prte_check_pbs_happy])

    AC_SUBST(PRTE_PBSTRMSH_PATH)
    AS_IF([test "$prte_check_pbs_happy" = "yes"],
          [$2],
          [$3])

    PRTE_VAR_SCOPE_POP
])
