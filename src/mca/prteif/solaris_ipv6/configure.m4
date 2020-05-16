# -*- shell-script -*-
#
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([MCA_prte_prteif_solaris_ipv6_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $1:$2 compile mode])
    $3="static"
    AC_MSG_RESULT([$$3])
])

# MCA_prteif_config_CONFIG(action-if-can-compile,
#                        [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_prteif_solaris_ipv6_CONFIG], [
    AC_CONFIG_FILES([src/mca/prteif/solaris_ipv6/Makefile])

    AC_REQUIRE([PRTE_CHECK_OS_FLAVORS])

    # check to see if we are on a solaris machine
    AS_IF([test "$prte_found_sun" = "yes"], [$1], [$2])
])dnl

#
# ifdef __sun__
#

