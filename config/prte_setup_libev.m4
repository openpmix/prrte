# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2020      IBM Corporation.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_libev_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_LIBEV_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([prte_event_dir prte_event_libdir prte_event_defaults prte_check_libev_save_CPPFLAGS prte_check_libev_save_LDFLAGS prte_check_libev_save_LIBS])

    AC_ARG_WITH([libev],
                [AS_HELP_STRING([--with-libev=DIR],
                                [Search for libev headers and libraries in DIR ])],
                [], [with_libev=no])
    AC_ARG_WITH([libev-libdir],
                [AS_HELP_STRING([--with-libev-libdir=DIR],
                                [Search for libev libraries in DIR ])])

    prte_libev_support=0
    prte_event_defaults=yes

    prte_check_libev_save_CPPFLAGS="$CPPFLAGS"
    prte_check_libev_save_LDFLAGS="$LDFLAGS"
    prte_check_libev_save_LIBS="$LIBS"

    # get rid of any trailing slash(es)
    libev_prefix=$(echo $with_libev | sed -e 'sX/*$XXg')
    libevdir_prefix=$(echo $with_libev_libdir | sed -e 'sX/*$XXg')

    if test "$libev_prefix" != "no"; then
        AC_MSG_CHECKING([for libev in])
        if test ! -z "$libev_prefix" && test "$libev_prefix" != "yes"; then
            prte_event_defaults=no
            prte_event_dir=$libev_prefix
            if test -d $libev_prefix/lib64; then
                prte_event_libdir=$libev_prefix/lib64
            elif test -d $libev_prefix/lib; then
                prte_event_libdir=$libev_prefix/lib
            elif test -d $libev_prefix; then
                prte_event_libdir=$libev_prefix
            else
                AC_MSG_RESULT([Could not find $libev_prefix/lib64, $libev_prefix/lib, or $libev_prefix])
                AC_MSG_ERROR([Can not continue])
            fi
            AC_MSG_RESULT([$prte_event_dir and $prte_event_libdir])
        else
            prte_event_defaults=yes
            prte_event_dir=/usr
            if test -d /usr/lib64; then
                prte_event_libdir=/usr/lib64
                AC_MSG_RESULT([(default search paths)])
            elif test -d /usr/lib; then
                prte_event_libdir=/usr/lib
                AC_MSG_RESULT([(default search paths)])
            else
                AC_MSG_RESULT([default paths not found])
                prte_libev_support=0
            fi
        fi
        AS_IF([test ! -z "$libevdir_prefix" && "$libevdir_prefix" != "yes"],
              [prte_event_libdir="$libevdir_prefix"])

        PRTE_CHECK_PACKAGE([prte_libev],
                           [event.h],
                           [ev],
                           [ev_async_send],
                           [],
                           [$prte_event_dir],
                           [$prte_event_libdir],
                           [prte_libev_support=1],
                           [prte_libev_support=0])

        # need to add resulting flags to global ones so we can
        # test for thread support
        AS_IF([test "$prte_event_defaults" = "no"],
              [PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_libev_CPPFLAGS)
               PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prte_libev_LDFLAGS)])
        PRTE_FLAGS_APPEND_UNIQ(LIBS, $prte_libev_LIBS)
    fi
    prte_libev_source=$prte_event_dir

    AC_MSG_CHECKING([will libev support be built])
    if test $prte_libev_support -eq 1; then
        AC_MSG_RESULT([yes])
        AS_IF([test "$prte_event_defaults" = "no"],
              [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_libev_CPPFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_libev_CPPFLAGS)
               PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_libev_LDFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_libev_LDFLAGS)])
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_libev_LIBS)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_libev_LIBS)
        PRTE_EVENT_HEADER="<event.h>"
        # Set output variables
        AC_DEFINE_UNQUOTED([PRTE_EVENT_HEADER], [$PRTE_EVENT_HEADER],
                           [Location of event.h])
        PRTE_SUMMARY_ADD([[Required Packages]],[[Libev]], [prte_libev], [yes ($prte_libev_source)])
    else
        AC_MSG_RESULT([no])
    fi

    # restore global flags
    CPPFLAGS="$prte_check_libev_save_CPPFLAGS"
    LDFLAGS="$prte_check_libev_save_LDFLAGS"
    LIBS="$prte_check_libev_save_LIBS"

    AC_DEFINE_UNQUOTED([PRTE_HAVE_LIBEV], [$prte_libev_support], [Whether we are building against libev])

    PRTE_VAR_SCOPE_POP
])
