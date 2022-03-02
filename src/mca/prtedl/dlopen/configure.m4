# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
#
# Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([MCA_prte_prtedl_dlopen_PRIORITY], [80])

#
# Force this component to compile in static-only mode
#
AC_DEFUN([MCA_prte_prtedl_dlopen_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $1:$2 compile mode])
    $3="static"
    AC_MSG_RESULT([$$3])
])

# MCA_prtedl_dlopen_CONFIG([action-if-can-compile],
#                      [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_prtedl_dlopen_CONFIG],[
    AC_CONFIG_FILES([src/mca/prtedl/dlopen/Makefile])

    dnl This is effectively a back-door for PRTE developers to
    dnl force the use of the libltprtedl prtedl component.
    AC_ARG_ENABLE([prtedl-dlopen],
        [AS_HELP_STRING([--disable-prtedl-dlopen],
            [Disable the "dlopen" PRTE DL component (and probably force the use of the "libltdl" DL component).  This option should really only be used by PRTE developers.  You are probably actually looking for the "--disable-prtedlopen" option, which disables all dlopen-like functionality from PRTE.])
        ])

    prte_prtedl_prtedlopen_happy=no
    AS_IF([test "$enable_prtedl_prtedlopen" != "no"],
          [OAC_CHECK_PACKAGE([dlopen],
              [prte_prtedl_dlopen],
              [dlfcn.h],
              [dl],
              [dlopen],
              [prte_prtedl_dlopen_happy=yes],
              [prte_prtedl_dlopen_happy=no])
          ])

    AS_IF([test "$prte_prtedl_dlopen_happy" = "yes"],
          [prtedl_dlopen_ADD_LIBS=$prte_prtedl_dlopen_LIBS
           prtedl_dlopen_WRAPPER_EXTRA_LIBS=$prte_prtedl_dlopen_LIBS
           $1],
          [$2])

    AC_SUBST(prte_prtedl_dlopen_LIBS)
])
