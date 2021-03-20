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
# Copyright (c) 2021      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([PRTE_CHECK_PMIX],[

    PRTE_VAR_SCOPE_PUSH([prte_external_pmix_save_CPPFLAGS prte_external_pmix_save_LDFLAGS prte_external_pmix_save_LIBS prte_external_pmix_version_found prte_external_pmix_version])

    AC_ARG_WITH([pmix],
                [AS_HELP_STRING([--with-pmix(=DIR)],
                                [Where to find PMIx support, optionally adding DIR to the search path])])

    AC_ARG_WITH([pmix-libdir],
                [AS_HELP_STRING([--with-pmix-libdir=DIR],
                                [Look for libpmix in the given directory DIR, DIR/lib or DIR/lib64])])

    AC_ARG_WITH([pmix-header],
                [AS_HELP_STRING([--with-pmix-header=HEADER],
                                [The value that should be included in C files to include pmix.h])])

    AC_ARG_ENABLE([pmix-devel-support],
                  [AS_HELP_STRING([--enable-pmix-devel-support],
                                  [Add necessary wrapper flags to enable access to PMIx devel headers])])

    if test "x$with_pmix_header" != "x"; then
        AS_IF([test "$with_pmix_header" = "yes"],
              [PRTE_PMIX_HEADER="<pmix.h>"
               prte_pmix_header_given=0],
              [PRTE_PMIX_HEADER="\"$with_pmix_header\""
               prte_pmix_header_given=1])
        pmix_ext_install_dir="external header"

    elif test "$with_pmix" = "no"; then
        AC_MSG_WARN([PRTE requires PMIx support using])
        AC_MSG_WARN([an external copy that you supply.])
        AC_MSG_ERROR([Cannot continue])

    else
        prte_pmix_header_given=0
        PRTE_PMIX_HEADER="<pmix.h>"
        # check for external pmix lib */
        AS_IF([test -z "$with_pmix"],
              [pmix_ext_install_dir=/usr],
              [pmix_ext_install_dir=$with_pmix])

        # Make sure we have the headers and libs in the correct location
        PRTE_CHECK_WITHDIR([pmix], [$pmix_ext_install_dir/include], [pmix.h])

        AS_IF([test -n "$with_pmix_libdir"],
              [AC_MSG_CHECKING([libpmix.* in $with_pmix_libdir])
               files=`ls $with_pmix_libdir/libpmix.* 2> /dev/null | wc -l`
               AS_IF([test "$files" -gt 0],
                     [AC_MSG_RESULT([found])
                      pmix_ext_install_libdir=$with_pmix_libdir],
                     [AC_MSG_RESULT([not found])
                      AC_MSG_CHECKING([libpmix.* in $with_pmix_libdir/lib64])
                      files=`ls $with_pmix_libdir/lib64/libpmix.* 2> /dev/null | wc -l`
                      AS_IF([test "$files" -gt 0],
                            [AC_MSG_RESULT([found])
                             pmix_ext_install_libdir=$with_pmix_libdir/lib64],
                            [AC_MSG_RESULT([not found])
                             AC_MSG_CHECKING([libpmix.* in $with_pmix_libdir/lib])
                             files=`ls $with_pmix_libdir/lib/libpmix.* 2> /dev/null | wc -l`
                             AS_IF([test "$files" -gt 0],
                                   [AC_MSG_RESULT([found])
                                    pmix_ext_install_libdir=$with_pmix_libdir/lib],
                                    [AC_MSG_RESULT([not found])
                                     AC_MSG_ERROR([Cannot continue])])])])],
              [# check for presence of lib64 directory - if found, see if the
               # desired library is present and matches our build requirements
               AC_MSG_CHECKING([libpmix.* in $pmix_ext_install_dir/lib64])
               files=`ls $pmix_ext_install_dir/lib64/libpmix.* 2> /dev/null | wc -l`
               AS_IF([test "$files" -gt 0],
               [AC_MSG_RESULT([found])
                pmix_ext_install_libdir=$pmix_ext_install_dir/lib64],
               [AC_MSG_RESULT([not found])
                AC_MSG_CHECKING([libpmix.* in $pmix_ext_install_dir/lib])
                files=`ls $pmix_ext_install_dir/lib/libpmix.* 2> /dev/null | wc -l`
                AS_IF([test "$files" -gt 0],
                      [AC_MSG_RESULT([found])
                       pmix_ext_install_libdir=$pmix_ext_install_dir/lib],
                      [AC_MSG_RESULT([not found])
                       AC_MSG_ERROR([Cannot continue])])])])

        # check the version
        prte_external_pmix_save_CPPFLAGS=$CPPFLAGS
        prte_external_pmix_save_LDFLAGS=$LDFLAGS
        prte_external_pmix_save_LIBS=$LIBS

        # if the pmix_version.h file does not exist, then
        # this must be from a pre-1.1.5 version
        AC_MSG_CHECKING([for PMIx version file])
        CPPFLAGS="-I$pmix_ext_install_dir/include $CPPFLAGS"
        AS_IF([test "x`ls $pmix_ext_install_dir/include/pmix_version.h 2> /dev/null`" = "x"],
               [AC_MSG_RESULT([not found - assuming pre-v2.0])
                AC_MSG_WARN([PRTE does not support PMIx versions])
                AC_MSG_WARN([less than v4.01 as only PMIx-based tools can])
                AC_MSG_WARN([can connect to the server.])
                AC_MSG_ERROR([Please select a newer version and configure again])],
               [AC_MSG_RESULT([found])
                prte_external_pmix_version_found=0])

        # if it does exist, then we need to parse it to find
        # the actual release series
        AC_MSG_CHECKING([version 4x])
        AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                            #include <pmix_version.h>
                                            #if (PMIX_VERSION_MAJOR < 4L)
                                            #error "not version 4 or above"
                                            #endif
                                           ], [])],
                          [AC_MSG_RESULT([found])
                           prte_external_pmix_version=4x
                           prte_external_pmix_version_found=4],
                          [AC_MSG_RESULT([not found])])

        AS_IF([test "$prte_external_pmix_version_found" = "4"],
              [AC_MSG_CHECKING([version 4.0.1 or greater])
                AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                                    #include <pmix_version.h>
                                                    #if (PMIX_VERSION_MAJOR == 4L && PMIX_VERSION_MINOR == 0L && PMIX_VERSION_RELEASE < 1L)
                                                    #error "not version 4.0.1 or above"
                                                    #endif
                                                   ], [])],
                                  [AC_MSG_RESULT([found])],
                                  [AC_MSG_RESULT([not found])
                                   prte_external_pmix_version_found=0])])

        # restore the global flags
        CPPFLAGS=$prte_external_pmix_save_CPPFLAGS
        LDFLAGS=$prte_external_pmix_save_LDFLAGS
        LIBS=$prte_external_pmix_save_LIBS

        AS_IF([test "$prte_external_pmix_version_found" = "0"],
              [AC_MSG_WARN([PRTE does not support PMIx versions])
               AC_MSG_WARN([less than v4.01 as only PMIx-based tools can])
               AC_MSG_WARN([can connect to the server.])
               AC_MSG_ERROR([Please select a newer version and configure again])])

        AS_IF([test "x$prte_external_pmix_version" = "x"],
              [AC_MSG_WARN([PMIx version information could not])
               AC_MSG_WARN([be detected])
               AC_MSG_ERROR([cannot continue])])

        AS_IF([test "$pmix_ext_install_dir" != "/usr"],
              [prte_pmix_CPPFLAGS="-I$pmix_ext_install_dir/include"
               prte_pmix_LDFLAGS="-L$pmix_ext_install_libdir"])

        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_pmix_CPPFLAGS)
        PRTE_WRAPPER_FLAGS_ADD([CPPFLAGS], [$prte_pmix_CPPFLAGS])

        AS_IF([test "$enable_pmix_devel_support" = "yes"],
              [PRTE_WRAPPER_FLAGS_ADD([CPPFLAGS], [-I$pmix_ext_install_dir/include/pmix -I$pmix_ext_install_dir/include/pmix/src -I$pmix_ext_install_dir/include/pmix/src/include])])

        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_pmix_LDFLAGS)
        PRTE_WRAPPER_FLAGS_ADD([LDFLAGS], [$prte_pmix_LDFLAGS])

        prte_pmix_LIBS=-lpmix
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_pmix_LIBS)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_pmix_LIBS)
    fi

    AC_DEFINE_UNQUOTED([PRTE_PMIX_HEADER], [$PRTE_PMIX_HEADER], [PMIx header to use])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_HEADER_GIVEN], [$prte_pmix_header_given], [Whether or not the PMIx header was explicitly passed])

    PRTE_SUMMARY_ADD([[Required Packages]],[[PMIx]],[pmix],[yes ($pmix_ext_install_dir)])

    PRTE_VAR_SCOPE_POP
])
