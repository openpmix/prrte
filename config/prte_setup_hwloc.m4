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
    PRTE_VAR_SCOPE_PUSH([prte_hwloc_dir prte_hwloc_libdir prte_hwloc_standard_lib_location prte_hwloc_standard_header_location prte_check_hwloc_save_CPPFLAGS prte_check_hwloc_save_LDFLAGS prte_check_hwloc_save_LIBS])

    AC_ARG_WITH([hwloc],
                [AS_HELP_STRING([--with-hwloc=DIR],
                                [Search for hwloc headers and libraries in DIR ])])

    AC_ARG_WITH([hwloc-libdir],
                [AS_HELP_STRING([--with-hwloc-libdir=DIR],
                                [Search for hwloc libraries in DIR ])])

    AC_ARG_WITH([hwloc-header],
                [AS_HELP_STRING([--with-hwloc-header=HEADER],
                                [The value that should be included in C files to include hwloc.h])])

    prte_hwloc_support=0
    prte_hwloc_header_given=0
    prte_check_hwloc_save_CPPFLAGS="$CPPFLAGS"
    prte_check_hwloc_save_LDFLAGS="$LDFLAGS"
    prte_check_hwloc_save_LIBS="$LIBS"
    prte_hwloc_standard_header_location=yes
    prte_hwloc_standard_lib_location=yes
    prte_have_topology_dup=0

    if test "x$with_hwloc_header" != "x"; then
        AS_IF([test "$with_hwloc_header" = "yes"],
              [PRTE_HWLOC_HEADER="<hwloc.h>"],
              [PRTE_HWLOC_HEADER="\"$with_hwloc_header\""
               prte_hwloc_header_given=1])
        prte_hwloc_support=1
        prte_hwloc_source="external header"
        prte_have_topology_dup=1

    elif test "$with_hwloc" == "no"; then
        AC_MSG_WARN([PRRTE requires HWLOC topology library support.])
        AC_MSG_WARN([Please reconfigure so we can find the library.])
        AC_MSG_ERROR([Cannot continue.])

    else
        AC_MSG_CHECKING([for hwloc in])
        if test ! -z "$with_hwloc" && test "$with_hwloc" != "yes"; then
            prte_hwloc_dir=$with_hwloc
            prte_hwloc_standard_header_location=no
            prte_hwloc_standard_lib_location=no
            AS_IF([test -z "$with_hwloc_libdir" || test "$with_hwloc_libdir" = "yes"],
                  [if test -d $with_hwloc/lib; then
                       prte_hwloc_libdir=$with_hwloc/lib
                   elif test -d $with_hwloc/lib64; then
                       prte_hwloc_libdir=$with_hwloc/lib64
                   else
                       AC_MSG_RESULT([$with_hwloc])
                       AC_MSG_WARN([Could not find $with_hwloc/lib or $with_hwloc/lib64])
                       AC_MSG_ERROR([Can not continue])
                   fi
                   AC_MSG_RESULT([$prte_hwloc_dir and $prte_hwloc_libdir])],
                  [AC_MSG_RESULT([$with_hwloc_libdir])])
        else
            AC_MSG_RESULT([(default search paths)])
            prte_hwloc_dir=/usr
            if test -d /usr/lib; then
                prte_hwloc_libdir=/usr/lib
            elif test -d /usr/lib64; then
                prte_hwloc_libdir=/usr/lib64
            else
                AC_MSG_WARN([Could not find /usr/lib or /usr/lib64 - you may])
                AC_MSG_WARN([need to specify --with-hwloc_libdir=<path>])
                AC_MSG_ERROR([Can not continue])
            fi
            prte_hwloc_standard_header_location=yes
            prte_hwloc_standard_lib_location=yes
        fi
        AS_IF([test ! -z "$with_hwloc_libdir" && test "$with_hwloc_libdir" != "yes"],
              [prte_hwloc_libdir="$with_hwloc_libdir"
               prte_hwloc_standard_lib_location=no])

        PRTE_CHECK_PACKAGE([prte_hwloc],
                           [hwloc.h],
                           [hwloc],
                           [hwloc_topology_init],
                           [-lhwloc],
                           [$prte_hwloc_dir],
                           [$prte_hwloc_libdir],
                           [prte_hwloc_support=1],
                           [prte_hwloc_support=0])

        if test "$prte_hwloc_support" != "1"; then
            AC_MSG_WARN([PRRTE requires HWLOC topology library support, but])
            AC_MSG_WARN([an adequate version of that library was not found.])
            AC_MSG_WARN([Please reconfigure and point to a location where])
            AC_MSG_WARN([the HWLOC library can be found.])
            AC_MSG_ERROR([Cannot continue.])
        fi

        # update global flags to test for HWLOC version
        AS_IF([test "$prte_hwloc_standard_header_location" != "yes"],
              [PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_hwloc_CPPFLAGS)])
        AS_IF([test "$prte_hwloc_standard_lib_location" != "yes"],
              [PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prte_hwloc_LDFLAGS)])
        PRTE_FLAGS_APPEND_UNIQ(LIBS, $prte_hwloc_LIBS)

        if test $prte_hwloc_support = "1"; then
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
        fi

    fi

    CPPFLAGS=$prte_check_hwloc_save_CPPFLAGS
    LDFLAGS=$prte_check_hwloc_save_LDFLAGS
    LIBS=$prte_check_hwloc_save_LIBS

<<<<<<< HEAD
    if test "$prte_hwloc_support" == "1"; then
        AS_IF([test "$prte_hwloc_header_given" != "1"],
              [PRTE_HWLOC_HEADER="<hwloc.h>"])
        AS_IF([test "$prte_hwloc_standard_header_location" != "yes"],
              [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_hwloc_CPPFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_hwloc_CPPFLAGS)])

        AS_IF([test "$prte_hwloc_standard_lib_location" != "yes"],
              [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_hwloc_LDFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_hwloc_LDFLAGS)])
||||||| parent of cddf773271 (Change the pcc wrapper compiler to a symlink to pmixcc)
    if test ! -z "$prte_hwloc_CPPFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_hwloc_CPPFLAGS)
        PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_hwloc_CPPFLAGS)
    fi
    if test ! -z "$prte_hwloc_LDFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_hwloc_LDFLAGS)
        PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_hwloc_LDFLAGS)
    fi
    if test ! -z "$prte_hwloc_LIBS"; then
=======
    if test ! -z "$prte_hwloc_CPPFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_hwloc_CPPFLAGS)
    fi
    if test ! -z "$prte_hwloc_LDFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_hwloc_LDFLAGS)
    fi
    if test ! -z "$prte_hwloc_LIBS"; then
>>>>>>> cddf773271 (Change the pcc wrapper compiler to a symlink to pmixcc)
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_hwloc_LIBS)
<<<<<<< HEAD
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_hwloc_LIBS)
        PRTE_HWLOC_HEADER="<hwloc.h>"
    else
        AC_MSG_WARN([PRRTE requires HWLOC topology library support, but])
        AC_MSG_WARN([an adequate version of that library was not found.])
        AC_MSG_WARN([Please reconfigure and point to a location where])
        AC_MSG_WARN([the HWLOC library can be found.])
        AC_MSG_ERROR([Cannot continue.])
||||||| parent of cddf773271 (Change the pcc wrapper compiler to a symlink to pmixcc)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_hwloc_LIBS)
=======
>>>>>>> cddf773271 (Change the pcc wrapper compiler to a symlink to pmixcc)
    fi

    AC_MSG_CHECKING([hwloc header])
    AC_DEFINE_UNQUOTED([PRTE_HWLOC_HEADER], [$PRTE_HWLOC_HEADER],
                       [Location of hwloc.h])
    AC_MSG_RESULT([$PRTE_HWLOC_HEADER])

    AC_DEFINE_UNQUOTED([PRTE_HAVE_HWLOC], [$prte_hwloc_support],
                   [Whether or not we have hwloc support])

    AC_DEFINE_UNQUOTED([PRTE_HWLOC_HEADER_GIVEN], [$prte_hwloc_header_given],
                       [Whether or not the hwloc header was given to us])

    AC_DEFINE_UNQUOTED([PRTE_HAVE_HWLOC_TOPOLOGY_DUP], [$prte_have_topology_dup],
                       [Whether or not hwloc_topology_dup is available])

    prte_hwloc_support_will_build=yes
    prte_hwloc_source=$prte_hwloc_dir

    PRTE_SUMMARY_ADD([[Required Packages]],[[HWLOC]], [prte_hwloc], [$prte_hwloc_support_will_build ($prte_hwloc_source)])

    PRTE_VAR_SCOPE_POP
])
