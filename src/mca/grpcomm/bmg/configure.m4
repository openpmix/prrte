# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_grpcomm_bmg_CONFIG([action-if-can-compile],
#                        [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_grpcomm_bmg_CONFIG],[
    AC_CONFIG_FILES([src/mca/grpcomm/bmg/Makefile])

    AS_IF([test "$prte_enable_ft" = "1"],
          [$1],
          [$2])

])dnl
