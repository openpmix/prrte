# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_hwloc_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_HWLOC_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([prte_hwloc_dir prte_hwloc_libdir prte_check_hwloc_save_CPPFLAGS prte_check_hwloc_save_LDFLAGS prte_check_hwloc_save_LIBS])

    AC_ARG_WITH([hwloc],
                [AS_HELP_STRING([--with-hwloc=DIR],
                                [Search for hwloc headers and libraries in DIR ])])

    AC_ARG_WITH([hwloc-libdir],
                [AS_HELP_STRING([--with-hwloc-libdir=DIR],
                                [Search for hwloc libraries in DIR ])])

    prte_hwloc_support=0
    prte_check_hwloc_save_CPPFLAGS="$CPPFLAGS"
    prte_check_hwloc_save_LDFLAGS="$LDFLAGS"
    prte_check_hwloc_save_LIBS="$LIBS"
    prte_have_topology_dup=0

    if test "$with_hwloc" == "no"; then
        AC_MSG_WARN([PRRTE requires HWLOC topology library support.])
        AC_MSG_WARN([Please reconfigure so we can find the library.])
        AC_MSG_ERROR([Cannot continue.])
    fi

    # get rid of any trailing slash(es)
    hwloc_prefix=$(echo $with_hwloc | sed -e 'sX/*$XXg')
    hwlocdir_prefix=$(echo $with_hwloc_libdir | sed -e 'sX/*$XXg')

    AS_IF([test ! -z "$hwloc_prefix" && test "$hwloc_prefix" != "yes"],
                 [prte_hwloc_dir="$hwloc_prefix"],
                 [prte_hwloc_dir=""])
    _PRTE_CHECK_PACKAGE_HEADER([prte_hwloc], [hwloc.h], [$prte_hwloc_dir],
                               [prte_hwloc_support=1],
                               [prte_hwloc_support=0])

    if test $prte_hwloc_support -eq 0 && test -z $prte_hwloc_dir; then
        # try default locations
        if test -d /usr/include; then
            prte_hwloc_dir=/usr
            _PRTE_CHECK_PACKAGE_HEADER([prte_hwloc], [hwloc.h], [$prte_hwloc_dir],
                                       [prte_hwloc_support=1],
                                       [prte_hwloc_support=0])
        fi
        if test $prte_hwloc_support -eq 0 && test -d /usr/local/include; then
            prte_hwloc_dir=/usr/local
            _PRTE_CHECK_PACKAGE_HEADER([prte_hwloc], [hwloc.h], [$prte_hwloc_dir],
                                       [prte_hwloc_support=1],
                                       [prte_hwloc_support=0])
        fi
    fi

    if test $prte_hwloc_support -eq 0; then
        AC_MSG_WARN([PRRTE requires HWLOC topology library support, but])
        AC_MSG_WARN([an adequate version of that library was not found.])
        AC_MSG_WARN([Please reconfigure and point to a location where])
        AC_MSG_WARN([the HWLOC library can be found.])
        AC_MSG_ERROR([Cannot continue.])
    fi

    AS_IF([test ! -z "$hwlocdir_prefix" && test "$hwlocdir_prefix" != "yes"],
                 [prte_hwloc_libdir="$hwlocdir_prefix"],
                 [AS_IF([test ! -z "$hwloc_prefix" && test "$hwloc_prefix" != "yes"],
                        [if test -d $hwloc_prefix/lib64; then
                            prte_hwloc_libdir=$hwloc_prefix/lib64
                         elif test -d $hwloc_prefix/lib; then
                            prte_hwloc_libdir=$hwloc_prefix/lib
                         else
                            AC_MSG_WARN([Could not find $hwloc_prefix/lib or $hwloc_prefix/lib64])
                            AC_MSG_ERROR([Can not continue])
                         fi
                        ],
                        [prte_hwloc_libdir=""])])

    _PRTE_CHECK_PACKAGE_LIB([prte_hwloc], [hwloc], [hwloc_topology_init],
                            [], [$prte_hwloc_dir],
                            [$prte_hwloc_libdir],
                            [],
                            [AC_MSG_WARN([PRTE requires HWLOC support using])
                             AC_MSG_WARN([an external copy that you supply.])
                             AC_MSG_WARN([The library was not found in $prte_hwloc_libdir.])
                             AC_MSG_ERROR([Cannot continue])])

    # update global flags to test for HWLOC version
    if test ! -z "$prte_hwloc_CPPFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_hwloc_CPPFLAGS)
    fi
    if test ! -z "$prte_hwloc_LDFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prte_hwloc_LDFLAGS)
    fi
    if test ! -z "$prte_hwloc_LIBS"; then
        PRTE_FLAGS_APPEND_UNIQ(LIBS, $prte_hwloc_LIBS)
    fi

    AC_MSG_CHECKING([if external hwloc version is 1.5 or greater])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([[#include <hwloc.h>]],
          [[
    #if HWLOC_API_VERSION < 0x00010500
    #error "hwloc API version is less than 0x00010500"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])],
          [AC_MSG_RESULT([no])
           AC_MSG_ERROR([Cannot continue])])

    AC_MSG_CHECKING([if external hwloc version is 1.8 or greater])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([[#include <hwloc.h>]],
          [[
    #if HWLOC_API_VERSION < 0x00010800
    #error "hwloc API version is less than 0x00010800"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])
           prte_have_topology_dup=1],
          [AC_MSG_RESULT([no])])

    # set the header
    PRTE_HWLOC_HEADER="<hwloc.h>"

    CPPFLAGS=$prte_check_hwloc_save_CPPFLAGS
    LDFLAGS=$prte_check_hwloc_save_LDFLAGS
    LIBS=$prte_check_hwloc_save_LIBS

    if test ! -z "$prte_hwloc_CPPFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_hwloc_CPPFLAGS)
        PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, prte_hwloc_CPPFLAGS)
    fi
    if test ! -z "$prte_hwloc_LDFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_hwloc_LDFLAGS)
        PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_hwloc_LDFLAGS)
    fi
    if test ! -z "$prte_hwloc_LIBS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_hwloc_LIBS)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_hwloc_LIBS)
    fi

    AC_MSG_CHECKING([location of hwloc header])
    AC_DEFINE_UNQUOTED([PRTE_HWLOC_HEADER], [$PRTE_HWLOC_HEADER],
                       [Location of hwloc.h])
    AC_MSG_RESULT([$PRTE_HWLOC_HEADER])

    AC_DEFINE_UNQUOTED([PRTE_HAVE_HWLOC_TOPOLOGY_DUP], [$prte_have_topology_dup],
                       [Whether or not hwloc_topology_dup is available])

    prte_hwloc_support_will_build=yes
    prte_hwloc_source=$prte_hwloc_dir

    PRTE_SUMMARY_ADD([[Required Packages]],[[HWLOC]], [prte_hwloc], [$prte_hwloc_support_will_build ($prte_hwloc_source)])

    PRTE_VAR_SCOPE_POP
])
