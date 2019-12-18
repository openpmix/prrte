# -*- shell-script -*-
#
# Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2019      Sylabs, Inc. All rights reserved.
# Copyright (c) 2019      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_schizo_singularity_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prrte_schizo_singularity_CONFIG],[
    AC_CONFIG_FILES([src/mca/schizo/singularity/Makefile])

    PRRTE_CHECK_SINGULARITY
    AC_CHECK_FUNC([fork], [schizo_singularity_happy="yes"], [schizo_singularity_happy="no"])

    AS_IF([test "$schizo_singularity_happy" = "yes"], [$1], [$2])
])dnl
