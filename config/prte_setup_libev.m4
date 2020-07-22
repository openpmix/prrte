# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
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

# MCA_libev_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PRTE_LIBEV_CONFIG],[
    PRTE_VAR_SCOPE_PUSH([prte_libev_dir prte_libev_libdir prte_libev_standard_header_location prte_libev_standard_lib_location prte_check_libev_save_CPPFLAGS prte_check_libev_save_LDFLAGS prte_check_libev_save_LIBS])

    AC_ARG_WITH([libev],
                [AC_HELP_STRING([--with-libev=DIR],
                                [Search for libev headers and libraries in DIR ])])
    PRTE_CHECK_WITHDIR([libev], [$with_libev], [include/event.h])

    AC_ARG_WITH([libev-libdir],
                [AC_HELP_STRING([--with-libev-libdir=DIR],
                                [Search for libev libraries in DIR ])])
    PRTE_CHECK_WITHDIR([libev-libdir], [$with_livev_libdir], [libev.*])

    prte_libev_support=0

    AS_IF([test -n "$with_libev" && test "$with_libev" != "no"],
          [AC_MSG_CHECKING([for libev in])
           prte_check_libev_save_CPPFLAGS="$CPPFLAGS"
           prte_check_libev_save_LDFLAGS="$LDFLAGS"
           prte_check_libev_save_LIBS="$LIBS"
           if test "$with_libev" != "yes"; then
               prte_libev_dir=$with_libev/include
               prte_libev_standard_header_location=no
               prte_libev_standard_lib_location=no
               AS_IF([test -z "$with_libev_libdir" || test "$with_libev_libdir" = "yes"],
                     [if test -d $with_libev/lib; then
                          prte_libev_libdir=$with_libev/lib
                      elif test -d $with_libev/lib64; then
                          prte_libev_libdir=$with_libev/lib64
                      else
                          AC_MSG_RESULT([Could not find $with_libev/lib or $with_libev/lib64])
                          AC_MSG_ERROR([Can not continue])
                      fi
                      AC_MSG_RESULT([$prte_libev_dir and $prte_libev_libdir])],
                     [AC_MSG_RESULT([$with_libev_libdir])])
           else
               AC_MSG_RESULT([(default search paths)])
               prte_libev_standard_header_location=yes
               prte_libev_standard_lib_location=yes
           fi
           AS_IF([test ! -z "$with_libev_libdir" && test "$with_libev_libdir" != "yes"],
                 [prte_libev_libdir="$with_libev_libdir"
                  prte_libev_standard_lib_location=no])

           PRTE_CHECK_PACKAGE([prte_libev],
                              [event.h],
                              [ev],
                              [ev_async_send],
                              [],
                              [$prte_libev_dir],
                              [$prte_libev_libdir],
                              [prte_libev_support=1],
                              [prte_libev_support=0])
           CPPFLAGS="$prte_check_libev_save_CPPFLAGS"
           LDFLAGS="$prte_check_libev_save_LDFLAGS"
           LIBS="$prte_check_libev_save_LIBS"])

    AS_IF([test $prte_libev_support -eq 1],
          [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_libev_LIBS)
           PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_libev_LIBS)

           AS_IF([test "$prte_libev_standard_header_location" != "yes"],
                 [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_libev_CPPFLAGS)
                  PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_libev_CPPFLAGS)])
           AS_IF([test "$prte_libev_standard_lib_location" != "yes"],
                 [PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS $prte_libev_LDFLAGS)
                  PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_libev_LDFLAGS)])])

    AC_MSG_CHECKING([will libev support be built])
    if test $prte_libev_support -eq 1; then
        AC_MSG_RESULT([yes])
        PRTE_EVENT_HEADER="<event.h>"
        AC_DEFINE_UNQUOTED([PRTE_EVENT_HEADER], [$PRTE_EVENT_HEADER],
                           [Location of event.h])
        PRTE_SUMMARY_ADD([[Required Packages]],[[libev]],[libev],[$prte_libev_dir])
    else
        AC_MSG_RESULT([no])
        # if they asked us to use it, then this is an error
        AS_IF([test -n "$with_libev" && test "$with_libev" != "no"],
              [AC_MSG_WARN([LIBEV SUPPORT REQUESTED AND NOT FOUND])
               AC_MSG_ERROR([CANNOT CONTINUE])])
    fi

    AC_DEFINE_UNQUOTED([PRTE_HAVE_LIBEV], [$prte_libev_support], [Whether we are building against libev])

    PRTE_VAR_SCOPE_POP
])dnl
