# -*- shell-script -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006-2012 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2009-2011 Oracle and/or its affiliates.  All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRRTE_CHECK_VISIBILITY
# --------------------------------------------------------
AC_DEFUN([PRRTE_CHECK_VISIBILITY],[
    AC_REQUIRE([AC_PROG_GREP])

    # Check if the compiler has support for visibility, like some
    # versions of gcc, icc Sun Studio cc.
    AC_ARG_ENABLE(visibility,
        AC_HELP_STRING([--enable-visibility],
            [enable visibility feature of certain compilers/linkers (default: enabled)]))

    prrte_visibility_define=0
    prrte_msg="whether to enable symbol visibility"

    if test "$enable_visibility" = "no"; then
        AC_MSG_CHECKING([$prrte_msg])
        AC_MSG_RESULT([no (disabled)])
    else
        CFLAGS_orig=$CFLAGS

        prrte_add=
        case "$prrte_c_vendor" in
        sun)
            # Check using Sun Studio -xldscope=hidden flag
            prrte_add=-xldscope=hidden
            CFLAGS="$PRRTE_CFLAGS_BEFORE_PICKY $prrte_add -errwarn=%all"
            ;;

        *)
            # Check using -fvisibility=hidden
            prrte_add=-fvisibility=hidden
            CFLAGS="$PRRTE_CFLAGS_BEFORE_PICKY $prrte_add -Werror"
            ;;
        esac

        AC_MSG_CHECKING([if $CC supports $prrte_add])
        AC_LINK_IFELSE([AC_LANG_PROGRAM([[
            #include <stdio.h>
            __attribute__((visibility("default"))) int foo;
            ]],[[fprintf(stderr, "Hello, world\n");]])],
            [AS_IF([test -s conftest.err],
                   [$GREP -iq visibility conftest.err
                    # If we find "visibility" in the stderr, then
                    # assume it doesn't work
                    AS_IF([test "$?" = "0"], [prrte_add=])])
            ], [prrte_add=])
        AS_IF([test "$prrte_add" = ""],
              [AC_MSG_RESULT([no])],
              [AC_MSG_RESULT([yes])])

        CFLAGS=$CFLAGS_orig
        PRRTE_VISIBILITY_CFLAGS=$prrte_add

        if test "$prrte_add" != "" ; then
            prrte_visibility_define=1
            AC_MSG_CHECKING([$prrte_msg])
            AC_MSG_RESULT([yes (via $prrte_add)])
        elif test "$enable_visibility" = "yes"; then
            AC_MSG_ERROR([Symbol visibility support requested but compiler does not seem to support it.  Aborting])
        else
            AC_MSG_CHECKING([$prrte_msg])
            AC_MSG_RESULT([no (unsupported)])
        fi
        unset prrte_add
    fi

    AC_DEFINE_UNQUOTED([PRRTE_C_HAVE_VISIBILITY], [$prrte_visibility_define],
            [Whether C compiler supports symbol visibility or not])
])
