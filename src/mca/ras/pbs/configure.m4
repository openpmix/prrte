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
# Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
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
AC_DEFUN([MCA_prte_ras_pbs_CONFIG],[
    AC_CONFIG_FILES([src/mca/ras/pbs/Makefile])

    PRTE_CHECK_PBS([ras_pbs], [ras_pbs_good=1], [ras_pbs_good=0])

    AS_IF([test "$ras_pbs_good" = "1" || test "$prte_testbuild_launchers" = "1"],
          [$1],
          [$2])

    # there are no libraries to add to the Makefile as we don't
    # link against any PBS libs
])dnl
