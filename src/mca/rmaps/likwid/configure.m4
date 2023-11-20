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
# Copyright (c) 2011-2013 Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
# Copyright (c) 2022      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_ras_pbs_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prte_rmaps_likwid_CONFIG],[
    AC_CONFIG_FILES([src/mca/rmaps/likwid/Makefile])

# this is where you would want to provide the path to
# any 3rd-party libs you might need. Look at the src/mca/plm/tm
# plugin for an example - uses the config/prte_check_tm.m4
# For now, I've just left this so the plugin won't build unless
# requested

	AC_ARG_WITH([likwid],
           [AS_HELP_STRING([--with-likwid],
                           [Build LIKWID rmaps component (default: no)])])

	if test "$with_likwid" != "yes" ; then
            prte_check_likwid_happy="no"
    else
        prte_check_likwid_happy="yes"
    fi

    AS_IF([test "$prte_check_likwid_happy" = "yes"],
          [$1],
          [$2])

])dnl
