# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_compress_zlib_CONFIG([action-if-can-compile],
#                          [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prrte_compress_zlib_CONFIG],[
    AC_CONFIG_FILES([src/mca/compress/zlib/Makefile])

    PRRTE_VAR_SCOPE_PUSH([prrte_zlib_dir prrte_zlib_libdir prrte_zlib_standard_lib_location prrte_zlib_standard_header_location prrte_check_zlib_save_CPPFLAGS prrte_check_zlib_save_LDFLAGS prrte_check_zlib_save_LIBS])

    AC_ARG_WITH([zlib],
                [AC_HELP_STRING([--with-zlib=DIR],
                                [Search for zlib headers and libraries in DIR ])])

    AC_ARG_WITH([zlib-libdir],
                [AC_HELP_STRING([--with-zlib-libdir=DIR],
                                [Search for zlib libraries in DIR ])])

    prrte_check_zlib_save_CPPFLAGS="$CPPFLAGS"
    prrte_check_zlib_save_LDFLAGS="$LDFLAGS"
    prrte_check_zlib_save_LIBS="$LIBS"

    prrte_zlib_support=0

    if test "$with_zlib" != "no"; then
        AC_MSG_CHECKING([for zlib in])
        if test ! -z "$with_zlib" && test "$with_zlib" != "yes"; then
            prrte_zlib_dir=$with_zlib
            prrte_zlib_source=$with_zlib
            prrte_zlib_standard_header_location=no
            prrte_zlib_standard_lib_location=no
            AS_IF([test -z "$with_zlib_libdir" || test "$with_zlib_libdir" = "yes"],
                  [if test -d $with_zlib/lib; then
                       prrte_zlib_libdir=$with_zlib/lib
                   elif test -d $with_zlib/lib64; then
                       prrte_zlib_libdir=$with_zlib/lib64
                   else
                       AC_MSG_RESULT([Could not find $with_zlib/lib or $with_zlib/lib64])
                       AC_MSG_ERROR([Can not continue])
                   fi
                   AC_MSG_RESULT([$prrte_zlib_dir and $prrte_zlib_libdir])],
                  [AC_MSG_RESULT([$with_zlib_libdir])])
        else
            AC_MSG_RESULT([(default search paths)])
            prrte_zlib_source=standard
            prrte_zlib_standard_header_location=yes
            prrte_zlib_standard_lib_location=yes
        fi
        AS_IF([test ! -z "$with_zlib_libdir" && test "$with_zlib_libdir" != "yes"],
              [prrte_zlib_libdir="$with_zlib_libdir"
               prrte_zlib_standard_lib_location=no])

        PRRTE_CHECK_PACKAGE([compress_zlib],
                           [zlib.h],
                           [z],
                           [deflate],
                           [-lz],
                           [$prrte_zlib_dir],
                           [$prrte_zlib_libdir],
                           [prrte_zlib_support=1],
                           [prrte_zlib_support=0])
    fi

    if test ! -z "$with_zlib" && test "$with_zlib" != "no" && test "$prrte_zlib_support" != "1"; then
        AC_MSG_WARN([ZLIB SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    AC_MSG_CHECKING([will zlib support be built])
    if test "$prrte_zlib_support" != "1"; then
        AC_MSG_RESULT([no])
    else
        AC_MSG_RESULT([yes])
    fi

    CPPFLAGS="$prrte_check_zlib_save_CPPFLAGS"
    LDFLAGS="$prrte_check_zlib_save_LDFLAGS"
    LIBS="$prrte_check_zlib_save_LIBS"

    AS_IF([test "$prrte_zlib_support" = "1"],
          [$1
           PRRTE_SUMMARY_ADD([[External Packages]],[[ZLIB]], [prrte_zlib], [yes ($prrte_zlib_source)])],
          [$2])

    # substitute in the things needed to build this component
    AC_SUBST([compress_zlib_CFLAGS])
    AC_SUBST([compress_zlib_CPPFLAGS])
    AC_SUBST([compress_zlib_LDFLAGS])
    AC_SUBST([compress_zlib_LIBS])

    PRRTE_VAR_SCOPE_POP
])dnl
