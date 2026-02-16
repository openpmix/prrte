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
# Copyright (c) 2004-2006 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006      QLogic Corp. All rights reserved.
# Copyright (c) 2009-2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRTE_CHECK_JANSSON
# --------------------------------------------------------
# check if JANSSON support can be found.  sets jansson_{CPPFLAGS,
# LDFLAGS, LIBS}
AC_DEFUN([PRTE_CHECK_JANSSON],[

    PRTE_VAR_SCOPE_PUSH(prte_check_jansson_happy prte_check_jansson_found)
	AC_ARG_WITH([jansson],
		    [AS_HELP_STRING([--with-jansson(=DIR)],
				    [Build jansson support (default=no), optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])],
            [], [with_jansson=no])

    AC_ARG_WITH([jansson-libdir],
            [AS_HELP_STRING([--with-jansson-libdir=DIR],
                    [Search for Jansson libraries in DIR])])

    prte_jansson_source=unknown
    prte_check_jansson_found=0
    prte_check_jansson_happy="yes"

    if test "$with_jansson" != "no"; then
        AS_IF([test "$with_jansson" = "yes" || test -z "$with_jansson"],
              [prte_jansson_source=standard],
              [prte_jansson_source=$with_jansson])

        OAC_CHECK_PACKAGE([jansson],
                          [$1],
		                  [jansson.h],
	 	                  [jansson],
		                  [json_loads],
		                  [prte_check_jansson_found=1])

        if test ${prte_check_jansson_found} -eq 1; then
            AC_MSG_CHECKING([if libjansson version is 2.11 or greater])
            AS_IF([test "$prte_jansson_source" != "standard"],
                  [PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_jansson_CPPFLAGS)])
            AC_COMPILE_IFELSE(
                  [AC_LANG_PROGRAM([[#include <jansson.h>]],
                  [[
        #if JANSSON_VERSION_HEX < 0x00020b00
        #error "jansson API version is less than 2.11"
        #endif
                  ]])],
                  [AC_MSG_RESULT([yes])],
                  [AC_MSG_RESULT([no])
                   prte_check_jansson_found=0])
        fi
    fi
    AS_IF([test ${prte_check_jansson_found} -eq 0], [prte_check_jansson_happy="no"])

    # Did we find the right stuff?
    AS_IF([test "${prte_check_jansson_happy}" = "yes"],
          [$2],
          [AS_IF([test ! -z "${with_jansson}" && test "${with_jansson}" != "no"],
                 [AC_MSG_ERROR([Jansson support requested but not found.  Aborting])])
           $3])

    AC_MSG_CHECKING([Jansson support available])
    AC_MSG_RESULT([$prte_check_jansson_happy])

    PRTE_SUMMARY_ADD([[External Packages]],[[Jansson]], [prte_jansson], [$prte_check_jansson_happy ($prte_jansson_source)])

    PRTE_VAR_SCOPE_POP
])
