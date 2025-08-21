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
# Copyright (c) 2026      Triad National Security, LLC. All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_ras_flux_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prte_ras_flux_CONFIG],[
    AC_CONFIG_FILES([src/mca/ras/flux/Makefile])

    PRTE_CHECK_FLUX([ras_flux], [ras_flux_good=1], [ras_flux_good=0])

    # The Flux ras uses jansson so check for that
    PRTE_CHECK_JANSSON([ras_flux_jansson], [ras_jansson_good=1], [ras_jansson_good=0])

    # if check worked, set wrapper flags if so.
    # Evaluate succeed / fail
    AS_IF([test "$ras_flux_good" = "1" -a "$ras_jansson_good" = "1"],
          [$1],
          [$2])
 
    # set build flags to use in makefile
    AC_SUBST([ras_flux_CPPFLAGS])
    AC_SUBST([ras_flux_LDFLAGS])
    AC_SUBST([ras_flux_LIBS])

    AC_SUBST([ras_flux_jansson_CPPFLAGS])
    AC_SUBST([ras_flux_jansson_LDFLAGS])
    AC_SUBST([ras_flux_jansson_LIBS])
])dnl
