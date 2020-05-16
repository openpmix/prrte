# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_zlib_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_ZLIB_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([prte_zlib_dir prte_zlib_libdir prte_zlib_standard_header_location prte_zlib_standard_lib_location])

    AC_ARG_WITH([zlib],
                [AC_HELP_STRING([--with-zlib=DIR],
                                [Search for zlib headers and libraries in DIR ])])

    AC_ARG_WITH([zlib-libdir],
                [AC_HELP_STRING([--with-zlib-libdir=DIR],
                                [Search for zlib libraries in DIR ])])

    prte_zlib_support=0
    if test "$with_zlib" != "no"; then
        AC_MSG_CHECKING([for zlib in])
        if test ! -z "$with_zlib" && test "$with_zlib" != "yes"; then
            prte_zlib_dir=$with_zlib
            prte_zlib_standard_header_location=no
            prte_zlib_standard_lib_location=no
            AS_IF([test -z "$with_zlib_libdir" || test "$with_zlib_libdir" = "yes"],
                  [if test -d $with_zlib/lib; then
                       prte_zlib_libdir=$with_zlib/lib
                   elif test -d $with_zlib/lib64; then
                       prte_zlib_libdir=$with_zlib/lib64
                   else
                       AC_MSG_RESULT([Could not find $with_zlib/lib or $with_zlib/lib64])
                       AC_MSG_ERROR([Can not continue])
                   fi
                   AC_MSG_RESULT([$prte_zlib_dir and $prte_zlib_libdir])],
                  [AC_MSG_RESULT([$with_zlib_libdir])])
        else
            AC_MSG_RESULT([(default search paths)])
            prte_zlib_standard_header_location=yes
            prte_zlib_standard_lib_location=yes
        fi
        AS_IF([test ! -z "$with_zlib_libdir" && test "$with_zlib_libdir" != "yes"],
              [prte_zlib_libdir="$with_zlib_libdir"
               prte_zlib_standard_lib_location=no])

        PRTE_CHECK_PACKAGE([prte_zlib],
                           [zlib.h],
                           [z],
                           [deflate],
                           [-lz],
                           [$prte_zlib_dir],
                           [$prte_zlib_libdir],
                           [prte_zlib_support=1],
                           [prte_zlib_support=0])
        if test $prte_zlib_support = "1"; then
            LIBS="$LIBS -lz"
            if test "$prte_zlib_standard_header_location" != "yes"; then
                CPPFLAGS="$CPPFLAGS $prte_zlib_CPPFLAGS"
            fi
            if test "$prte_zlib_standard_lib_location" != "yes"; then
                LDFLAGS="$LDFLAGS $prte_zlib_LDFLAGS"
            fi
        fi
    fi

    if test ! -z "$with_zlib" && test "$with_zlib" != "no" && test "$prte_zlib_support" != "1"; then
        AC_MSG_WARN([ZLIB SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    AC_MSG_CHECKING([will zlib support be built])
    if test "$prte_zlib_support" != "1"; then
        AC_MSG_RESULT([no])
    else
        AC_MSG_RESULT([yes])
    fi

    AC_DEFINE_UNQUOTED([PRTE_HAVE_ZLIB], [$prte_zlib_support],
                       [Whether or not we have zlib support])
    PRTE_VAR_SCOPE_POP
])dnl
