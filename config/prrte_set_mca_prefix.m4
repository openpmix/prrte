# -*- shell-script -*-
#
# Copyright (c) 2014      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRRTE_SET_MCA_PREFIX([mca_prefix]
#
# This macro sets a prefix for the MCA parameter system.  Specifically,
# OMPI_MCA_<foo> becomes <mca_prefix>_<foo>
#
# --------------------------------------------------------
AC_DEFUN([PRRTE_SET_MCA_PREFIX],[
    AS_IF([test "$prrte_mca_prefix_set" = "yes"],
          [AC_MSG_WARN([PRRTE mca prefix was already set!])
           AC_MSG_WARN([This is a configury programming error])
           AC_MSG_ERROR([Cannot continue])])

    MCA_PREFIX=$1
    prrte_mca_prefix_set=yes
    AC_DEFINE_UNQUOTED([PRRTE_MCA_PREFIX], ["$MCA_PREFIX"], [MCA prefix string for envars])
])dnl

#
# Set the MCA cmd line identifier - i.e., change "-mca" to "-<foo>"
#
AC_DEFUN([PRRTE_SET_MCA_CMD_LINE_ID],[
    AS_IF([test "$prrte_mca_cmd_id_set" = "yes"],
          [AC_MSG_WARN([PRRTE mca cmd line id was already set!])
           AC_MSG_WARN([This is a configury programming error])
           AC_MSG_ERROR([Cannot continue])])

    MCA_CMD_LINE_ID=$1
    prrte_mca_cmd_id_set=yes
    AC_DEFINE_UNQUOTED([PRRTE_MCA_CMD_LINE_ID], ["$MCA_CMD_LINE_ID"], [MCA cmd line identifier])
])dnl
