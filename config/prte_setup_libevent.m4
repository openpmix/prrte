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
    prte_event_defaults=yes

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

        if test "$libevent_prefix" != "no"; then
            AC_MSG_CHECKING([for libevent in])
            if test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"; then
                prte_event_defaults=no
                prte_event_dir=$libevent_prefix
                if test -d $libevent_prefix/lib64; then
                    prte_event_libdir=$libevent_prefix/lib64
                elif test -d $libevent_prefix/lib; then
                    prte_event_libdir=$libevent_prefix/lib
                elif test -d $libevent_prefix; then
                    prte_event_libdir=$libevent_prefix
                else
                    AC_MSG_RESULT([Could not find $libevent_prefix/lib, $libevent_prefix/lib64, or $libevent_prefix])
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
                    prte_libevent_support=0
                fi
            fi
            AS_IF([test ! -z "$libeventdir_prefix" && "$libeventdir_prefix" != "yes"],
                  [prte_event_libdir="$libeventdir_prefix"])

            PRTE_CHECK_PACKAGE([prte_libevent],
                               [event.h],
                               [event_core],
                               [event_config_new],
                               [-levent_pthreads],
                               [$prte_event_dir],
                               [$prte_event_libdir],
                               [prte_libevent_support=1],
                               [prte_libevent_support=0])

            # need to add resulting flags to global ones so we can
            # test for thread support
            AS_IF([test "$prte_event_defaults" = "no"],
                  [PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_libevent_CPPFLAGS)
                   PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prte_libevent_LDFLAGS)])
            PRTE_FLAGS_APPEND_UNIQ(LIBS, $prte_libevent_LIBS)

            if test $prte_libevent_support -eq 1; then
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
        fi
        prte_libevent_source=$prte_event_dir
    fi

    AC_MSG_CHECKING([will libevent support be built])
    if test $prte_libevent_support -eq 1; then
        AC_MSG_RESULT([yes])
        AS_IF([test "$prte_event_defaults" = "no"],
              [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_libevent_CPPFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_libevent_CPPFLAGS)
               PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_libevent_LDFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_libevent_LDFLAGS)])
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_libevent_LIBS)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_libevent_LIBS)
        PRTE_EVENT_HEADER="<event.h>"
        PRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"
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
