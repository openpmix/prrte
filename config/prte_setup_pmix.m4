# -*- autoconf ; indent-tabs-mode:nil -*-
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
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

#
# We have three modes for building PMIx.
#
# First is an embedded PMIx, where PRTE is being built into
# another library and assumes that PMIx is available, that there
# is a single header (pointed to by --with-pmix-header) which
# includes all the PMIx bits, and that the right PMIx
# configuration is used.  This mode is used when --enable-embeded-mode
# is specified to configure.
#
# Second is as a co-built hwloc.  In this case, PRTE's CPPFLAGS
# will be set before configure to include the right -Is to pick up
# PMIx headers and LIBS will point to where the .la file for
# PMIx will exist.  When co-building, PMIX's configure will be
# run already, but the library will not yet be built.  It is ok to run
# any compile-time (not link-time) tests in this mode.  This mode is
# used when the --with-pmix=cobuild option is specified.
#
# Third is an external package.  In this case, all compile and link
# time tests can be run.  This macro must do any CPPFLAGS/LDFLAGS/LIBS
# modifications it desires in order to compile and link against
# PMIx.  This mode is used whenever the other modes are not used.
#
# PRTE_CHECK_PMIX[action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_CHECK_PMIX],[
    PRTE_VAR_SCOPE_PUSH([prte_pmix_support prte_pmix_source prte_pmix_support_will_build prte_pmix_header_given])

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

    prte_pmix_support=0
    prte_pmix_source=""
    prte_pmix_support_will_build=no
    prte_pmix_header_given=0
    PRTE_PMIX_HEADER="<pmix.h>"

    # figure out our mode...
    AS_IF([test "$prte_mode" = "embedded"],
          [_PRTE_PMIX_EMBEDDED_MODE(embedded)],
          [test "$with_pmix" = "cobuild"],
          [_PRTE_PMIX_EMBEDDED_MODE(cobuild)],
	  [test "$with_pmix" = "no"],
          [AC_MSG_WARN([PRTE requires a PMIx 4.0 or newer library to build.])
           AC_MSG_ERROR([Cannot continue])],
          [_PRTE_PMIX_EXTERNAL])

    AS_IF([test $prte_pmix_support -eq 1],
          [AC_MSG_CHECKING([pmix header])
           AC_MSG_RESULT([$PRTE_PMIX_HEADER])])

    AC_DEFINE_UNQUOTED([PRTE_PMIX_HEADER], [$PRTE_PMIX_HEADER],
                   [Location of pmix.h])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_HEADER_GIVEN], [$prte_pmix_header_given],
                       [Whether or not the pmix header was given to us])

    PRTE_SUMMARY_ADD([[Required Packages]],[[PMIx]], [pmix], [$prte_pmix_support_will_build ($prte_pmix_source)])

    PRTE_VAR_SCOPE_POP
])

AC_DEFUN([_PRTE_PMIX_EMBEDDED_MODE], [
    AC_MSG_CHECKING([for PMIx])
    AC_MSG_RESULT([$1])

    AS_IF([test "$1" == "embedded"], [
        AS_IF([test -n "$with_pmix_header" && test "$with_pmix_header" != "yes"],
              [prte_pmix_header_given=1
               PRTE_PMIX_HEADER="$with_pmix_header"])])

    AS_IF([test "$1" == "cobuild"],
           [AC_MSG_CHECKING([if cobuild PMIx version is 4.1 or greater])
            AC_COMPILE_IFELSE(
              [AC_LANG_PROGRAM([[#include <pmix.h>]],
              [[
    #if (PMIX_VERSION_MAJOR == 4L && PMIX_VERSION_MINOR < 1L)
    #  error "PMIx version not version 4.1 or above"
    #endif
              ]])],
              [AC_MSG_RESULT([yes])],
              [AC_MSG_RESULT([no])
               AC_MSG_ERROR([Cannot continue])])])

    prte_pmix_support=1
    prte_pmix_source=$1
    prte_pmix_support_will_build=yes
])

AC_DEFUN([_PRTE_PMIX_EXTERNAL], [
    PRTE_VAR_SCOPE_PUSH([prte_external_pmix_save_CPPFLAGS prte_external_pmix_save_LDFLAGS prte_external_pmix_save_LIBS prte_external_pmix_version_found prte_external_pmix_version pmix_ext_install_dir])

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
           [AC_MSG_CHECKING([version 4.1 or greater])
            AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                                #include <pmix_version.h>
                                                #if (PMIX_VERSION_MAJOR == 4L && PMIX_VERSION_MINOR < 1L)
                                                #error "not version 4.1 or above"
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
           AC_MSG_WARN([less than v4.1 as only PMIx-based tools can])
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

    prte_pmix_support_will_build=yes
    prte_pmix_source=$pmix_ext_install_dir

    PRTE_VAR_SCOPE_POP
])
