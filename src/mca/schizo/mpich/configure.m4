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
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_schizo_mpich_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prte_schizo_mpich_CONFIG],[
    AC_CONFIG_FILES([src/mca/schizo/mpich/Makefile])

    AC_ARG_ENABLE([mpich-support],
                  [AS_HELP_STRING([--disable-mpich-support],
                                  [Disable support for MPICH (default: no)])],
                  [],
                  [enable_mpich_support=yes])

    AS_IF([test "$enable_mpich_support" = "yes"],
          [$1], [$2])

    PRTE_SUMMARY_ADD([Personalities], [MPICH], [], [$enable_mpich_support])

])dnl
