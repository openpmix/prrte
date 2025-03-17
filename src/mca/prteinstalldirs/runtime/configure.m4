# -*- shell-script -*-
#
# Copyright (c) 2025      NVIDIA Corporation.  All rights reserved.
# Copyright (c) 2025      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
AC_DEFUN([MCA_prte_prteinstalldirs_runtime_PRIORITY], [5])

AC_DEFUN([MCA_prte_prteinstalldirs_runtime_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $2:$3 compile mode])
    $4="static"
    AC_MSG_RESULT([$$4])
])

# MCA_prteinstalldirs_config_CONFIG(action-if-can-compile,
#                        [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_prteinstalldirs_runtime_CONFIG], [
    # Check if we are building a shared library or not. Disable if static
    AC_MSG_CHECKING([if shared libraries are enabled])
    AS_IF([test "$enable_shared" != "yes"],
          [prteinstalldirs_runtime_happy="no"],
          [prteinstalldirs_runtime_happy="yes"])
    AC_MSG_RESULT([$prteinstalldirs_runtime_happy])

    # Check if dladdr is available
    AS_IF([test "$prteinstalldirs_runtime_happy" = "yes"],
          [AC_CHECK_HEADERS([dlfcn.h],
                            [],
                            [prteinstalldirs_runtime_happy="no"])])
    AS_IF([test "$prteinstalldirs_runtime_happy" = "yes"],
          [AC_CHECK_LIB([dl], [dladdr],
                        [],
                        [prteinstalldirs_runtime_happy="no"])
          ])
    #
    AS_IF([test "$prteinstalldirs_runtime_happy" = "yes"],
          [AC_CONFIG_FILES([src/mca/prteinstalldirs/runtime/Makefile])
           $1], [$2])
])

