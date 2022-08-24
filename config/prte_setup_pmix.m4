# -*- shell-script ; indent-tabs-mode:nil -*-
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
# Copyright (c) 2011-2014 Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2014-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2016      IBM Corporation.  All rights reserved.
# Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
# Copyright (c) 2021-2022 Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([PRTE_CHECK_PMIX],[

    PRTE_VAR_SCOPE_PUSH([prte_external_pmix_save_CPPFLAGS prte_pmix_support found_pmixcc])

    AC_ARG_WITH([pmix],
                [AS_HELP_STRING([--with-pmix(=DIR)],
                                [Where to find PMIx support, optionally adding DIR to the search path])])
    AC_ARG_WITH([pmix-libdir],
                [AS_HELP_STRING([--with-pmix-libdir=DIR],
                                [Look for libpmix in the given directory DIR, DIR/lib or DIR/lib64])])
    AC_ARG_WITH([pmix-extra-libs],
                [AS_HELP_STRING([--with-pmix-extra-libs=LIBS],
                                [Add LIBS as dependencies of pmix])])
    AC_ARG_ENABLE([pmix-lib-checks],
                  [AS_HELP_STRING([--disable-pmix-lib-checks],
                                  [If --disable-pmix-lib-checks is specified, configure will assume that -lpmix is available])])

    prte_pmix_support=1

    if test "$with_pmix" = "no"; then
        AC_MSG_WARN([PRTE requires PMIx support using])
        AC_MSG_WARN([an external copy that you supply.])
        AC_MSG_ERROR([Cannot continue])
    fi

    AS_IF([test "$with_pmix_extra_libs" = "yes" -o "$with_pmix_extra_libs" = "no"],
	  [AC_MSG_ERROR([--with-pmix-extra-libs requires an argument other than yes or no])])

    AS_IF([test "$enable_pmix_lib_checks" != "no"],
          [dnl Need to explicitly enable wrapper compiler to get the dependent libraries
           dnl when pkg-config is not available.
           pmix_USE_WRAPPER_COMPILER=1
           OAC_CHECK_PACKAGE([pmix],
                             [prte_pmix],
                             [pmix.h],
                             [pmix $with_pmix_extra_libs],
                             [PMIx_Init],
                             [],
                             [prte_pmix_support=0])],
          [PRTE_FLAGS_APPEND_UNIQ([PRTE_FINAL_LIBS], [$with_pmix_extra_libs])])

    AS_IF([test $prte_pmix_support -eq 0],
          [AC_MSG_WARN([PRRTE requires PMIx support using an external copy that you supply.])
           AC_MSG_ERROR([Cannot continue.])])

    prte_external_pmix_save_CPPFLAGS=$CPPFLAGS
    PRTE_FLAGS_PREPEND_UNIQ(CPPFLAGS, $prte_pmix_CPPFLAGS)

    # if the version file exists, then we need to parse it to find
    # the actual release series
    AC_MSG_CHECKING([version at or above v4.2.1])
    AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                        #include <pmix_version.h>
                                        #if (PMIX_NUMERIC_VERSION < 0x00040201)
                                        #error "not version 4.2.1 or above"
                                        #endif
                                       ], [])],
                      [AC_MSG_RESULT([yes])],
                      [AC_MSG_RESULT(no)
                       AC_MSG_WARN([PRRTE requires PMIx v4.2.1 or above.])
                       AC_MSG_ERROR([Please select a supported version and configure again])])

    AC_CHECK_HEADER([src/util/pmix_argv.h], [],
                    [AC_MSG_ERROR([Could not find PMIx devel headers.  Can not continue.])])

    # restore the global flags
    CPPFLAGS=$prte_external_pmix_save_CPPFLAGS

    PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_pmix_CPPFLAGS)
    PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_pmix_LDFLAGS)
    PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_pmix_LIBS)

    found_pmixcc=0
    PMIXCC_PATH="pmixcc"
    AS_IF([test -n "${with_pmix}"],
          [PMIXCC_PATH="${with_pmix}/bin/$PMIXCC_PATH"])
    PRTE_LOG_COMMAND([pmixcc_showme_results=`$PMIXCC_PATH --showme:version 2>&1`], [found_pmixcc=1])
    PRTE_LOG_MSG([pmixcc version: $pmixcc_showme_results])
    AS_IF([test $found_pmixcc -eq 0],
          [AC_MSG_WARN([Could not find $PMIXCC_PATH])
           PMIXCC_PATH=])
    AM_CONDITIONAL([PRTE_HAVE_PMIXCC], [test $found_pmixcc -eq 1])
    AC_SUBST([PMIXCC_PATH])

    PRTE_SUMMARY_ADD([Required Packages], [PMIx], [], [$prte_pmix_SUMMARY])

    PRTE_VAR_SCOPE_POP
])
