# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_libevent_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRRTE_LIBEVENT_CONFIG],[
    PRRTE_VAR_SCOPE_PUSH([prrte_event_dir prrte_event_libdir prrte_event_defaults])

    AC_ARG_WITH([libevent],
                [AC_HELP_STRING([--with-libevent=DIR],
                                [Search for libevent headers and libraries in DIR ])])
    AC_ARG_WITH([libevent-header],
                [AC_HELP_STRING([--with-libevent-header=HEADER],
                                [The value that should be included in C files to include event.h])])

    AC_ARG_WITH([libevent-libdir],
                [AC_HELP_STRING([--with-libevent-libdir=DIR],
                                [Search for libevent libraries in DIR ])])

    prrte_libevent_support=0

    prrte_check_libevent_save_CPPFLAGS="$CPPFLAGS"
    prrte_check_libevent_save_LDFLAGS="$LDFLAGS"
    prrte_check_libevent_save_LIBS="$LIBS"

    if test "x$with_libevent_header" != "x"; then
        AS_IF([test "$with_libevent_header" = "yes"],
              [PRRTE_EVENT_HEADER="<event.h>"
               PRRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"],
              [PRRTE_EVENT_HEADER="\"$with_libevent_header\""
               PRRTE_EVENT2_THREAD_HEADER="\"$with_libevent_header\""])
        prrte_libevent_source="external header"
        prrte_libevent_support=1

    else
        # get rid of any trailing slash(es)
        libevent_prefix=$(echo $with_libevent | sed -e 'sX/*$XXg')
        libeventdir_prefix=$(echo $with_libevent_libdir | sed -e 'sX/*$XXg')

        if test "$libevent_prefix" != "no"; then
            AC_MSG_CHECKING([for libevent in])
            if test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"; then
                prrte_event_defaults=no
                prrte_event_dir=$libevent_prefix
                if test -d $libevent_prefix/lib; then
                    prrte_event_libdir=$libevent_prefix/lib
                elif test -d $libevent_prefix/lib64; then
                    prrte_event_libdir=$libevent_prefix/lib64
                elif test -d $libevent_prefix; then
                    prrte_event_libdir=$libevent_prefix
                else
                    AC_MSG_RESULT([Could not find $libevent_prefix/lib, $libevent_prefix/lib64, or $libevent_prefix])
                    AC_MSG_ERROR([Can not continue])
                fi
                AC_MSG_RESULT([$prrte_event_dir and $prrte_event_libdir])
            else
                prrte_event_defaults=yes
                prrte_event_dir=/usr
                if test -d /usr/lib; then
                    prrte_event_libdir=/usr/lib
                    AC_MSG_RESULT([(default search paths)])
                elif test -d /usr/lib64; then
                    prrte_event_libdir=/usr/lib64
                    AC_MSG_RESULT([(default search paths)])
                else
                    AC_MSG_RESULT([default paths not found])
                    prrte_libevent_support=0
                fi
            fi
            AS_IF([test ! -z "$libeventdir_prefix" && "$libeventdir_prefix" != "yes"],
                  [prrte_event_libdir="$libeventdir_prefix"])

            PRRTE_CHECK_PACKAGE([prrte_libevent],
                               [event.h],
                               [event],
                               [event_config_new],
                               [-levent -levent_pthreads],
                               [$prrte_event_dir],
                               [$prrte_event_libdir],
                               [prrte_libevent_support=1],
                               [prrte_libevent_support=0])

            AS_IF([test "$prrte_event_defaults" = "no"],
                  [PRRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prrte_libevent_CPPFLAGS)
                   PRRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prrte_libevent_LDFLAGS)])
            PRRTE_FLAGS_APPEND_UNIQ(LIBS, $prrte_libevent_LIBS)

            if test $prrte_libevent_support -eq 1; then
                # Ensure that this libevent has the symbol
                # "evthread_set_lock_callbacks", which will only exist if
                # libevent was configured with thread support.
                AC_CHECK_LIB([event], [evthread_set_lock_callbacks],
                             [],
                             [AC_MSG_WARN([libevent does not have thread support])
                              AC_MSG_WARN([PRRTE requires libevent to be compiled with])
                              AC_MSG_WARN([thread support enabled])
                              prrte_libevent_support=0])
            fi
            if test $prrte_libevent_support -eq 1; then
                AC_CHECK_LIB([event_pthreads], [evthread_use_pthreads],
                             [],
                             [AC_MSG_WARN([libevent does not have thread support])
                              AC_MSG_WARN([PRRTE requires libevent to be compiled with])
                              AC_MSG_WARN([thread support enabled])
                              prrte_libevent_support=0])
            fi
        fi
        prrte_libevent_source=$prrte_event_dir
        AS_IF([test "$prrte_event_defaults" = "no"],
              [PRRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prrte_libevent_CPPFLAGS)
               PRRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prrte_libevent_CPPFLAGS)
               PRRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prrte_libevent_LDFLAGS)
               PRRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prrte_libevent_LDFLAGS)])
        PRRTE_FLAGS_APPEND_UNIQ(LIBS, $prrte_libevent_LIBS)
        PRRTE_WRAPPER_FLAGS_ADD(LIBS, $prrte_libevent_LIBS)
        PRRTE_EVENT_HEADER="<event.h>"
        PRRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"
    fi

    AC_MSG_CHECKING([will libevent support be built])
    if test $prrte_libevent_support -eq 1; then
        AC_MSG_RESULT([yes])
        # Set output variables
        AC_DEFINE_UNQUOTED([PRRTE_EVENT_HEADER], [$PRRTE_EVENT_HEADER],
                           [Location of event.h])
        AC_DEFINE_UNQUOTED([PRRTE_EVENT2_THREAD_HEADER], [$PRRTE_EVENT2_THREAD_HEADER],
                           [Location of event2/thread.h])
        PRRTE_SUMMARY_ADD([[Required Packages]],[[Libevent]], [prrte_libevent], [yes ($prrte_libevent_source)])
    else
        AC_MSG_RESULT([no])
        CPPFLAGS="$prrte_check_libevent_save_CPPFLAGS"
        LDFLAGS="$prrte_check_libevent_save_LDFLAGS"
        LIBS="$prrte_check_libevent_save_LIBS"
    fi

    AC_DEFINE_UNQUOTED([PRRTE_HAVE_LIBEVENT], [$prrte_libevent_support], [Whether we are building against libevent])

    PRRTE_VAR_SCOPE_POP
])
