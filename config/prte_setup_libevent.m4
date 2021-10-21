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

# MCA_libevent_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_LIBEVENT_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([prte_event_dir prte_event_libdir prte_event_defaults prte_check_libevent_save_CPPFLAGS prte_check_libevent_save_LDFLAGS prte_check_libevent_save_LIBS])

    AC_ARG_WITH([libevent],
                [AS_HELP_STRING([--with-libevent=DIR],
                                [Search for libevent headers and libraries in DIR ])])
    AC_ARG_WITH([libevent-header],
                [AS_HELP_STRING([--with-libevent-header=HEADER],
                                [The value that should be included in C files to include event.h])])

    AC_ARG_WITH([libevent-libdir],
                [AS_HELP_STRING([--with-libevent-libdir=DIR],
                                [Search for libevent libraries in DIR ])])

    prte_libevent_support=0

    prte_check_libevent_save_CPPFLAGS="$CPPFLAGS"
    prte_check_libevent_save_LDFLAGS="$LDFLAGS"
    prte_check_libevent_save_LIBS="$LIBS"

    if test "x$with_libevent_header" != "x"; then
        AS_IF([test "$with_libevent_header" = "yes"],
              [PRTE_EVENT_HEADER="<event.h>"
               PRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"],
              [PRTE_EVENT_HEADER="\"$with_libevent_header\""
               PRTE_EVENT2_THREAD_HEADER="\"$with_libevent_header\""])
        prte_libevent_source="external header"
        prte_libevent_support=1

    else
        # get rid of any trailing slash(es)
        libevent_prefix=$(echo $with_libevent | sed -e 'sX/*$XXg')
        libeventdir_prefix=$(echo $with_libevent_libdir | sed -e 'sX/*$XXg')

        AS_IF([test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"],
              [prte_event_dir="$libevent_prefix"],
              [prte_event_dir=""])
        _PRTE_CHECK_PACKAGE_HEADER([prte_libevent], [event.h], [$prte_event_dir],
                                   [prte_libevent_support=1],
                                   [prte_libevent_support=0])
        if test $prte_libevent_support -eq 0 && test -z $prte_event_dir; then
            # try default locations
            if test -d /usr/include; then
                prte_event_dir=/usr
                _PRTE_CHECK_PACKAGE_HEADER([prte_libevent], [event.h], [$prte_event_dir],
                                           [prte_libevent_support=1],
                                           [prte_libevent_support=0])
            fi
            if test $prte_libevent_support -eq 0 && test -d /usr/local/include; then
                prte_event_dir=/usr/local
                _PRTE_CHECK_PACKAGE_HEADER([prte_libevent], [event.h], [$prte_event_dir],
                                           [prte_libevent_support=1],
                                           [prte_libevent_support=0])
            fi
        fi

        if test $prte_libevent_support -eq 1; then
            AS_IF([test ! -z "$libeventdir_prefix" && test "$libeventdir_prefix" != "yes"],
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
            _PRTE_CHECK_PACKAGE_LIB([prte_libevent], [event_core], [event_config_new],
                                    [-levent_pthreads], [$prte_event_dir],
                                    [$prte_event_libdir],
                                    [prte_libevent_support=1],
                                    [prte_libevent_support=0])

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
        fi

        if test $prte_libevent_support -eq 1; then
            # need to add resulting flags to global ones so we can
            # test for thread support
            if test ! -z "$prte_libevent_CPPFLAGS"; then
                PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_libevent_CPPFLAGS)
            fi
            if test ! -z "$prte_libevent_LDFLAGS"; then
                PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prte_libevent_LDFLAGS)
            fi
            if test ! -z "$prte_libevent_LIBS"; then
                PRTE_FLAGS_APPEND_UNIQ(LIBS, $prte_libevent_LIBS)
            fi

            # Ensure that this libevent has the symbol
            # "evthread_set_lock_callbacks", which will only exist if
            # libevent was configured with thread support.
            AC_CHECK_LIB([event_core], [evthread_set_lock_callbacks],
                         [],
                         [AC_MSG_WARN([libevent does not have thread support])
                          AC_MSG_WARN([PRTE requires libevent to be compiled with])
                          AC_MSG_WARN([thread support enabled])
                          prte_libevent_support=0])
        fi

        if test $prte_libevent_support -eq 1; then
            AC_CHECK_LIB([event_pthreads], [evthread_use_pthreads],
                         [],
                         [AC_MSG_WARN([libevent does not have thread support])
                          AC_MSG_WARN([PRTE requires libevent to be compiled with])
                          AC_MSG_WARN([thread support enabled])
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
        prte_libevent_source=$prte_event_dir
        PRTE_EVENT_HEADER="<event.h>"
        PRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"
    fi

    AC_MSG_CHECKING([will libevent support be built])
    if test $prte_libevent_support -eq 1; then
        AC_MSG_RESULT([yes])
        if test ! -z "$prte_libevent_CPPFLAGS"; then
            PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_libevent_CPPFLAGS)
            PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_libevent_CPPFLAGS)
        fi
        if test ! -z "$prte_libevent_LDFLAGS"; then
            PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_libevent_LDFLAGS)
            PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_libevent_LDFLAGS)
        fi
        if test ! -z "$prte_libevent_LIBS"; then
            PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_libevent_LIBS)
            PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_libevent_LIBS)
        fi
        # Set output variables
        AC_DEFINE_UNQUOTED([PRTE_EVENT_HEADER], [$PRTE_EVENT_HEADER],
                           [Location of event.h])
        AC_DEFINE_UNQUOTED([PRTE_EVENT2_THREAD_HEADER], [$PRTE_EVENT2_THREAD_HEADER],
                           [Location of event2/thread.h])
        PRTE_SUMMARY_ADD([[Required Packages]],[[Libevent]], [prte_libevent], [yes ($prte_libevent_source)])
    else
        AC_MSG_RESULT([no])
    fi

    # restore global flags
    CPPFLAGS="$prte_check_libevent_save_CPPFLAGS"
    LDFLAGS="$prte_check_libevent_save_LDFLAGS"
    LIBS="$prte_check_libevent_save_LIBS"

    AC_DEFINE_UNQUOTED([PRTE_HAVE_LIBEVENT], [$prte_libevent_support], [Whether we are building against libevent])

    PRTE_VAR_SCOPE_POP
])
