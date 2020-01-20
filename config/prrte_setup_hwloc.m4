# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_hwloc_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRRTE_HWLOC_CONFIG],[
    PRRTE_VAR_SCOPE_PUSH([prrte_hwloc_dir prrte_hwloc_libdir prrte_hwloc_standard_lib_location prrte_hwloc_standard_header_location])

    AC_ARG_WITH([hwloc],
                [AC_HELP_STRING([--with-hwloc=DIR],
                                [Search for hwloc headers and libraries in DIR ])])

    AC_ARG_WITH([hwloc-libdir],
                [AC_HELP_STRING([--with-hwloc-libdir=DIR],
                                [Search for hwloc libraries in DIR ])])

    AC_ARG_WITH([hwloc-header],
                [AC_HELP_STRING([--with-hwloc-header=HEADER],
                                [The value that should be included in C files to include hwloc.h])])

    prrte_hwloc_support=0
    prrte_hwloc_header_given=0
    if test "x$with_hwloc_header" != "x"; then
        AS_IF([test "$with_hwloc_header" = "yes"],
              [PRRTE_HWLOC_HEADER="<hwloc.h>"],
              [PRRTE_HWLOC_HEADER="\"$with_hwloc_header\""
               prrte_hwloc_header_given=1])
        prrte_hwloc_support=1
        prrte_hwloc_source=embedded

    elif test "$with_hwloc" != "no"; then
        AC_MSG_CHECKING([for hwloc in])
        if test ! -z "$with_hwloc" && test "$with_hwloc" != "yes"; then
            prrte_hwloc_dir=$with_hwloc
            prrte_hwloc_standard_header_location=no
            prrte_hwloc_standard_lib_location=no
            AS_IF([test -z "$with_hwloc_libdir" || test "$with_hwloc_libdir" = "yes"],
                  [if test -d $with_hwloc/lib; then
                       prrte_hwloc_libdir=$with_hwloc/lib
                   elif test -d $with_hwloc/lib64; then
                       prrte_hwloc_libdir=$with_hwloc/lib64
                   else
                       AC_MSG_RESULT([Could not find $with_hwloc/lib or $with_hwloc/lib64])
                       AC_MSG_ERROR([Can not continue])
                   fi
                   AC_MSG_RESULT([$prrte_hwloc_dir and $prrte_hwloc_libdir])],
                  [AC_MSG_RESULT([$with_hwloc_libdir])])
        else
            prrte_hwloc_dir=/usr
            if test -d /usr/lib; then
                prrte_hwloc_libdir=/usr/lib
            elif test -d /usr/lib64; then
                prrte_hwloc_libdir=/usr/lib64
            else
                AC_MSG_RESULT([not found])
                AC_MSG_WARN([Could not find /usr/lib or /usr/lib64 - you may])
                AC_MSG_WARN([need to specify --with-hwloc_libdir=<path>])
                AC_MSG_ERROR([Can not continue])
            fi
            AC_MSG_RESULT([(default search paths)])
            prrte_hwloc_standard_header_location=yes
            prrte_hwloc_standard_lib_location=yes
        fi
        AS_IF([test ! -z "$with_hwloc_libdir" && test "$with_hwloc_libdir" != "yes"],
              [prrte_hwloc_libdir="$with_hwloc_libdir"
               prrte_hwloc_standard_lib_location=no])

        PRRTE_CHECK_PACKAGE([prrte_hwloc],
                           [hwloc.h],
                           [hwloc],
                           [hwloc_topology_init],
                           [-lhwloc],
                           [$prrte_hwloc_dir],
                           [$prrte_hwloc_libdir],
                           [prrte_hwloc_support=1],
                           [prrte_hwloc_support=0])

        if test ! -z "$with_hwloc" && test "$with_hwloc" != "no" && test "$prrte_hwloc_support" != "1"; then
            AC_MSG_WARN([HWLOC SUPPORT REQUESTED AND NOT FOUND])
            AC_MSG_ERROR([CANNOT CONTINUE])
        fi

        AS_IF([test "$prrte_hwloc_standard_header_location" != "yes"],
              [PRRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prrte_hwloc_CPPFLAGS)
               PRRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prrte_hwloc_CPPFLAGS)])

        AS_IF([test "$prrte_hwloc_standard_lib_location" != "yes"],
              [PRRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prrte_hwloc_LDFLAGS)
               PRRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prrte_hwloc_LDFLAGS)])
        PRRTE_FLAGS_APPEND_UNIQ(LIBS, $prrte_hwloc_LIBS)
        PRRTE_WRAPPER_FLAGS_ADD(LIBS, $prrte_hwloc_LIBS)
        PRRTE_HWLOC_HEADER="<hwloc.h>"
        prrte_hwloc_source=$prrte_hwloc_dir

        if test $prrte_hwloc_support = "1"; then
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
        fi
    fi

    AC_MSG_CHECKING([hwloc header])
    AC_DEFINE_UNQUOTED([PRRTE_HWLOC_HEADER], [$PRRTE_HWLOC_HEADER],
                       [Location of hwloc.h])
    AC_MSG_RESULT([$PRRTE_HWLOC_HEADER])

    AC_DEFINE_UNQUOTED([PRRTE_HAVE_HWLOC], [$prrte_hwloc_support],
                   [Whether or not we have hwloc support])

    AC_DEFINE_UNQUOTED([PRRTE_HWLOC_HEADER_GIVEN], [$prrte_hwloc_header_given],
                       [Whether or not the hwloc header was given to us])

    AC_MSG_CHECKING([will hwloc support be built])
    if test "$prrte_hwloc_support" != "1"; then
        AC_MSG_RESULT([no])
        prrte_hwloc_source=none
        prrte_hwloc_support_will_build=no
    else
        AC_MSG_RESULT([yes])
        prrte_hwloc_support_will_build=yes
    fi

    PRRTE_SUMMARY_ADD([[Required Packages]],[[HWLOC]], [prrte_hwloc], [$prrte_hwloc_support_will_build ($prrte_hwloc_source)])

    PRRTE_VAR_SCOPE_POP
])
