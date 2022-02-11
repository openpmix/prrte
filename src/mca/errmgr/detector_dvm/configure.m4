# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2022      Nanook Consulting  All rights reserved.
# Copyright (c) 2022      The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_errmgr_detector_dvm_CONFIG([action-if-can-compile],
#                           [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_errmgr_detector_dvm_CONFIG],[
    AC_CONFIG_FILES([src/mca/errmgr/detector_dvm/Makefile])

    AS_IF([test "$prte_build_ft_method_detector" = "yes"],
          [$1],
          [$2])

])dnl
