# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2020      IBM Corporation.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2020-2021 Amazon.com, Inc. or its affiliates.  All Rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

#
# We have three modes for building libevent.
#
# First is an embedded libevent, where PRTE is being built into
# another library and assumes that libevent is available, that there
# is a single header (pointed to by --with-libevent-header) which
# includes all the Libevent bits, and that the right libevent
# configuration is used.  This mode is used when --enable-embeded-mode
# is specified to configure.
#
# Second is as a co-built libevent.  In this case, PRTE's CPPFLAGS
# will be set before configure to include the right -Is to pick up
# libevent headers and LIBS will point to where the .la file for
# libevent will exist.  When co-building, libevent's configure will be
# run already, but the library will not yet be built.  It is ok to run
# any compile-time (not link-time) tests in this mode.  This mode is
# used when the --with-libevent=cobuild option is specified.
#
# Third is an external package.  In this case, all compile and link
# time tests can be run.  This macro must do any CPPFLAGS/LDFLAGS/LIBS
# modifications it desires in order to compile and link against
# libevent.  This mode is used whenever the other modes are not used.
#
# This macro will change the environment in the following way:
#
#   * prte_libevent_support - set to 1 if libevent is found and works,
#         set to 0 otherwise
#
# This macro will create the following AC_DEFINEs:
#
#   * PRTE_EVENT_HEADER - name of the Libevent header to include.
#         Generally just <event.h>, unless embedded mode is enabled.
#         This define is shared with prte_setup_libev.
#
#   * PRTE_EVENT2_THREAD_HEADER - name of the Libevent thread header
#         to include. This is a historical artifact of how embed mode
#         works and can be removed if/when embed mode dies
#
# PRTE_LIBEVENT_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_LIBEVENT_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([libevent_build_mode PRTE_EVENT_HEADER PRTE_EVENT2_THREAD_HEADER libevent_build_result prte_libevent_source])

    AC_ARG_WITH([libevent],
                [AS_HELP_STRING([--with-libevent=DIR],
                                [Search for libevent headers and libraries in DIR ])])

    AC_ARG_WITH([libevent-libdir],
                [AS_HELP_STRING([--with-libevent-libdir=DIR],
                                [Search for libevent libraries in DIR ])])

    AC_ARG_WITH([libevent-header],
                [AS_HELP_STRING([--with-libevent-header=HEADER],
                                [The value that should be included in C files to include event.h.  This option only has meaning if --enable-embedded-mode is enabled.])])

    PRTE_EVENT_HEADER="<event.h>"
    PRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"

    prte_libevent_support=0
    prte_libevent_source=""
    libevent_build_mode=""

    # figure out our mode...
    AS_IF([test "$prte_mode" = "embedded"],
          [_PRTE_LIBEVENT_EMBEDDED_MODE(embedded)],
          [test "$with_libevent" = "cobuild"],
          [_PRTE_LIBEVENT_EMBEDDED_MODE(cobuild)],
          [_PRTE_LIBEVENT_EXTERNAL])

    AS_IF([test $prte_libevent_support -eq 1], [
        AC_MSG_CHECKING([libevent header])
        AC_DEFINE_UNQUOTED([PRTE_EVENT_HEADER], [$PRTE_EVENT_HEADER],
                           [Location of event.h])
        AC_MSG_RESULT([$PRTE_EVENT_HEADER])
        AC_MSG_CHECKING([libevent2/thread header])
        AC_DEFINE_UNQUOTED([PRTE_EVENT2_THREAD_HEADER], [$PRTE_EVENT2_THREAD_HEADER],
                           [Location of event2/thread.h])
        AC_MSG_RESULT([$PRTE_EVENT2_THREAD_HEADER])

        libevent_build_result="yes ($prte_libevent_source)"
    ], [libevent_build_result="no"])

    PRTE_SUMMARY_ADD([[Required Packages]], [[Libevent]], [prte_libevent], [$libevent_build_result])

    PRTE_VAR_SCOPE_POP
])

AC_DEFUN([_PRTE_LIBEVENT_EMBEDDED_MODE], [
    AC_MSG_CHECKING([for libevent])
    AC_MSG_RESULT([$1])

    AS_IF([test -n "$with_libevent_header" && test "$with_libevent_header" != "yes"],
          [PRTE_EVENT_HEADER="$with_libevent_header"
           PRTE_EVENT2_THREAD_HEADER="$with_libevent_header"])

    AS_IF([test "$1" == "cobuild"],
        [AC_MSG_CHECKING([if co-built libevent includes thread support])
         AC_TRY_COMPILE([#include <event.h>
#include <event2/thread.h>
           ],[
#if !(EVTHREAD_LOCK_API_VERSION >= 1)
#error "No threads!"
#endif
           ],[AC_MSG_RESULT([yes])],
           [AC_MSG_RESULT([no])
            AC_MSG_ERROR([No thread support in co-build libevent.  Aborting])])])

    prte_libevent_source=$1
    prte_libevent_support=1
])

AC_DEFUN([_PRTE_LIBEVENT_EXTERNAL],[
    PRTE_VAR_SCOPE_PUSH([prte_event_dir prte_event_libdir prte_event_defaults prte_check_libevent_save_CPPFLAGS prte_check_libevent_save_LDFLAGS prte_check_libevent_save_LIBS])

    prte_check_libevent_save_CPPFLAGS="$CPPFLAGS"
    prte_check_libevent_save_LDFLAGS="$LDFLAGS"
    prte_check_libevent_save_LIBS="$LIBS"
    prte_event_defaults=yes

    # get rid of the trailing slash(es)
    libevent_prefix=$(echo $with_libevent | sed -e 'sX/*$XXg')
    libeventdir_prefix=$(echo $with_libevent_libdir | sed -e 'sX/*$XXg')

    if test "$libevent_prefix" != "no"; then
        AC_MSG_CHECKING([for libevent in])
        if test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"; then
            prte_event_defaults=no
            prte_event_dir=$libevent_prefix/include
            if test -d $libevent_prefix/lib; then
                prte_event_libdir=$libevent_prefix/lib
            elif test -d $libevent_prefix/lib64; then
                prte_event_libdir=$libevent_prefix/lib64
            elif test -d $libevent_prefix; then
                prte_event_libdir=$libevent_prefix
            else
                AC_MSG_RESULT([Could not find $libevent_prefix/lib, $libevent_prefix/lib64, or $libevent_prefix])
                AC_MSG_ERROR([Can not continue])
            fi
            AC_MSG_RESULT([$prte_event_dir and $prte_event_libdir])
        else
            prte_event_defaults=yes
            prte_event_dir=/usr/include
            if test -d /usr/lib; then
                prte_event_libdir=/usr/lib
                AC_MSG_RESULT([(default search paths)])
            elif test -d /usr/lib64; then
                prte_event_libdir=/usr/lib64
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
                         [AC_MSG_WARN([External libevent does not have thread support])
                          AC_MSG_WARN([Prte requires libevent to be compiled with])
                          AC_MSG_WARN([thread support enabled])
                          prte_libevent_support=0])
        fi
        if test $prte_libevent_support -eq 1; then
            AC_CHECK_LIB([event_pthreads], [evthread_use_pthreads],
                         [],
                         [AC_MSG_WARN([External libevent does not have thread support])
                          AC_MSG_WARN([Prte requires libevent to be compiled with])
                          AC_MSG_WARN([thread support enabled])
                          prte_libevent_support=0])
        fi
    fi

    CPPFLAGS="$prte_check_libevent_save_CPPFLAGS"
    LDFLAGS="$prte_check_libevent_save_LDFLAGS"
    LIBS="$prte_check_libevent_save_LIBS"

    AC_MSG_CHECKING([will libevent support be built])
    if test $prte_libevent_support -eq 1; then
        AC_MSG_RESULT([yes])
        # Set output variables
        PRTE_EVENT_HEADER="<event.h>"
        PRTE_EVENT2_THREAD_HEADER="<event2/thread.h>"
        AC_DEFINE_UNQUOTED([PRTE_EVENT_HEADER], [$PRTE_EVENT_HEADER],
                           [Location of event.h])
        prte_libevent_source=$prte_event_dir
        AS_IF([test "$prte_event_defaults" = "no"],
              [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_libevent_CPPFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_libevent_CPPFLAGS)
               PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_libevent_LDFLAGS)
               PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_libevent_LDFLAGS)])
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_libevent_LIBS)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_libevent_LIBS)
    else
        AC_MSG_RESULT([no])
    fi

    PRTE_VAR_SCOPE_POP
])dnl
