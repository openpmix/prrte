# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2020      IBM Corporation.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRTE_LIBEVENT_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
# Attempt to find a libevent package.  If found, evaluate
# action-if-found.  Otherwise, evaluate action-if-not-found.
#
# Modifies the following in the environment:
#  * prte_libevent_CPPFLAGS
#  * prte_libevent_LDFLAGS
#  * prte_libevent_LIBS
#
# Adds the following to the wrapper compilers:
#  * CPPFLAGS: none
#  * LDLFGAS: add prte_libevent_LDFLAGS
#  * LIBS: add prte_libevent_LIBS
AC_DEFUN([PRTE_LIBEVENT_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([prte_event_dir prte_event_libdir prte_check_libevent_save_CPPFLAGS prte_check_libevent_save_LDFLAGS prte_check_libevent_save_LIBS])

    AC_ARG_WITH([libevent],
                [AS_HELP_STRING([--with-libevent=DIR],
                                [Search for libevent headers and libraries in DIR ])])
    AC_ARG_WITH([libevent-libdir],
                [AS_HELP_STRING([--with-libevent-libdir=DIR],
                                [Search for libevent libraries in DIR ])])
    AC_ARG_WITH([libevent-extra-libs],
                [AS_HELP_STRING([--with-libevent-extra-libs=LIBS],
                                [Add LIBS as dependencies of Libevent])])
    AC_ARG_ENABLE([libevent-lib-checks],
                   [AS_HELP_STRING([--disable-libevent-lib-checks],
                                   [If --disable-libevent-lib-checks is specified, configure will assume that -levent is available])])

    prte_libevent_support=1

    AS_IF([test "$with_libevent" = "no"],
          [AC_MSG_NOTICE([Libevent support disabled by user.])
           prte_libevent_support=0])

    AS_IF([test "$with_libevent_extra_libs" = "yes" -o "$with_libevent_extra_libs" = "no"],
	  [AC_MSG_ERROR([--with-libevent-extra-libs requires an argument other than yes or no])])

    AS_IF([test $prte_libevent_support -eq 1],
          [PRTE_CHECK_WITHDIR([libevent], [$with_libevent], [include/event.h])
           PRTE_CHECK_WITHDIR([libevent-libdir], [$with_libevent_libdir], [libevent.*])

           prte_check_libevent_save_CPPFLAGS="$CPPFLAGS"
           prte_check_libevent_save_LDFLAGS="$LDFLAGS"
           prte_check_libevent_save_LIBS="$LIBS"

           # get rid of any trailing slash(es)
           libevent_prefix=$(echo $with_libevent | sed -e 'sX/*$XXg')
           libeventdir_prefix=$(echo $with_libevent_libdir | sed -e 'sX/*$XXg')

           AS_IF([test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"],
                 [prte_event_dir="$libevent_prefix"],
                 [prte_event_dir=""])

           AS_IF([test ! -z "$libeventdir_prefix" -a "$libeventdir_prefix" != "yes"],
                 [prte_event_libdir="$libeventdir_prefix"],
                 [AS_IF([test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"],
                        [if test -d $libevent_prefix/lib64; then
                            prte_event_libdir=$libevent_prefix/lib64
                         elif test -d $libevent_prefix/lib; then
                            prte_event_libdir=$libevent_prefix/lib
                         else
                            AC_MSG_WARN([Could not find $libevent_prefix/lib or $libevent_prefix/lib64])
                            AC_MSG_ERROR([Can not continue])
                         fi
                        ],
                        [prte_event_libdir=""])])

           AS_IF([test "$enable_libevent_lib_checks" != "no"],
                 [PRTE_CHECK_PACKAGE([prte_libevent],
                                     [event.h],
                                     [event_core],
                                     [event_config_new],
                                     [-levent_pthreads $with_libevent_extra_libs],
                                     [$prte_event_dir],
                                     [$prte_event_libdir],
                                     [],
                                     [prte_libevent_support=0],
                                     [])],
                 [PRTE_FLAGS_APPEND_UNIQ([PRTE_FINAL_LIBS], [$with_libevent_extra_libs])])])

    # Check to see if the above check failed because it conflicted with LSF's libevent.so
    # This can happen if LSF's library is in the LDFLAGS envar or default search
    # path. The 'event_getcode4name' function is only defined in LSF's libevent.so and not
    # in Libevent's libevent.so
    if test $prte_libevent_support -eq 0; then
        AC_CHECK_LIB([event], [event_getcode4name],
                     [AC_MSG_WARN([===================================================================])
                      AC_MSG_WARN([Possible conflicting libevent.so libraries detected on the system.])
                      AC_MSG_WARN([])
                      AC_MSG_WARN([LSF provides a libevent.so that is not from Libevent in its])
                      AC_MSG_WARN([library path. It is possible that you have installed Libevent])
                      AC_MSG_WARN([on the system, but the linker is picking up the wrong version.])
                      AC_MSG_WARN([])
                      AC_MSG_WARN([You will need to address this linker path issue. One way to do so is])
                      AC_MSG_WARN([to make sure the libevent system library path occurs before the])
                      AC_MSG_WARN([LSF library path.])
                      AC_MSG_WARN([===================================================================])
                      ])
    fi

    if test $prte_libevent_support -eq 1; then
        # need to add resulting flags to global ones so we can
        # test for thread support
        PRTE_FLAGS_PREPEND_UNIQ([CPPFLAGS], [$prte_libevent_CPPFLAGS])
        PRTE_FLAGS_PREPEND_UNIQ([LDFLAGS], [$prte_libevent_LDFLAGS])
        PRTE_FLAGS_PREPEND_UNIQ([LIBS], [$prte_libevent_LIBS])

        # Check for general threading support
        AC_MSG_CHECKING([if libevent threads enabled])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
#include <event.h>
#include <event2/thread.h>
          ], [[
#if !(EVTHREAD_LOCK_API_VERSION >= 1)
#  error "No threads!"
#endif
          ]])],
          [AC_MSG_RESULT([yes])],
          [AC_MSG_RESULT([no])
           AC_MSG_WARN([PRTE rquires libevent to be compiled with thread support enabled])
           prte_libevent_support=0])
    fi

    if test $prte_libevent_support -eq 1; then
        AC_MSG_CHECKING([for libevent pthreads support])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
#include <event.h>
#include <event2/thread.h>
          ], [[
#if !defined(EVTHREAD_USE_PTHREADS_IMPLEMENTED) || !EVTHREAD_USE_PTHREADS_IMPLEMENTED
#  error "No pthreads!"
#endif
          ]])],
          [AC_MSG_RESULT([yes])],
          [AC_MSG_RESULT([no])
           AC_MSG_WARN([PRTE requires libevent to be compiled with pthread support enabled])
           prte_libevent_support=0])
    fi

    if test $prte_libevent_support -eq 1; then
        # Pin the "oldest supported" version to 2.0.21
        AC_MSG_CHECKING([if libevent version is 2.0.21 or greater])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <event2/event.h>]],
                                           [[
                                             #if defined(_EVENT_NUMERIC_VERSION) && _EVENT_NUMERIC_VERSION < 0x02001500
                                             #error "libevent API version is less than 0x02001500"
                                             #elif defined(EVENT__NUMERIC_VERSION) && EVENT__NUMERIC_VERSION < 0x02001500
                                             #error "libevent API version is less than 0x02001500"
                                             #endif
                                           ]])],
                          [AC_MSG_RESULT([yes])],
                          [AC_MSG_RESULT([no])
                           AC_MSG_WARN([libevent version is too old (2.0.21 or later required)])
                           prte_libevent_support=0])
    fi
    if test -z "$prte_event_dir"; then
        prte_libevent_source="Standard locations"
    else
        prte_libevent_source=$prte_event_dir
    fi

    # restore global flags
    CPPFLAGS="$prte_check_libevent_save_CPPFLAGS"
    LDFLAGS="$prte_check_libevent_save_LDFLAGS"
    LIBS="$prte_check_libevent_save_LIBS"

    AC_MSG_CHECKING([will libevent support be built])
    if test $prte_libevent_support -eq 1; then
        AC_MSG_RESULT([yes])
        PRTE_FLAGS_APPEND_UNIQ([PRTE_FINAL_CPPFLAGS], [$prte_libevent_CPPFLAGS])

        PRTE_FLAGS_APPEND_UNIQ([PRTE_FINAL_LDFLAGS], [$prte_libevent_LDFLAGS])

        PRTE_FLAGS_APPEND_UNIQ([PRTE_FINAL_LIBS], [$prte_libevent_LIBS])

        # Set output variables
        PRTE_SUMMARY_ADD([[Required Packages]],[[Libevent]], [prte_libevent], [yes ($prte_libevent_source)])

        $1
    else
        AC_MSG_RESULT([no])

        $2
    fi

    PRTE_VAR_SCOPE_POP
])
