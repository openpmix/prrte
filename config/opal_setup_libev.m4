# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
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
AC_DEFUN([OPAL_LIBEV_CONFIG],[
    OPAL_VAR_SCOPE_PUSH([opal_libev_dir opal_libev_libdir opal_libev_standard_header_location opal_libev_standard_lib_location])

    AC_ARG_WITH([libev],
                [AC_HELP_STRING([--with-libev=DIR],
                                [Search for libev headers and libraries in DIR ])])
    OPAL_CHECK_WITHDIR([libev], [$with_libev], [include/event.h])

    AC_ARG_WITH([libev-libdir],
                [AC_HELP_STRING([--with-libev-libdir=DIR],
                                [Search for libev libraries in DIR ])])
    OPAL_CHECK_WITHDIR([libev-libdir], [$with_livev_libdir], [libev.*])

    opal_libev_support=0

    AS_IF([test -n "$with_libev" && test "$with_libev" != "no"],
          [AC_MSG_CHECKING([for libev in])
           opal_check_libev_save_CPPFLAGS="$CPPFLAGS"
           opal_check_libeve_save_LDFLAGS="$LDFLAGS"
           opal_check_libev_save_LIBS="$LIBS"
           if test "$with_libev" != "yes"; then
               opal_libev_dir=$with_libev
               opal_libev_standard_header_location=no
               opal_libev_standard_lib_location=no
               AS_IF([test -z "$with_libev_libdir" || test "$with_libev_libdir" = "yes"],
                     [if test -d $with_libev/lib; then
                          opal_libev_libdir=$with_libev/lib
                      elif test -d $with_libev/lib64; then
                          opal_libev_libdir=$with_libev/lib64
                      else
                          AC_MSG_RESULT([Could not find $with_libev/lib or $with_libev/lib64])
                          AC_MSG_ERROR([Can not continue])
                      fi
                      AC_MSG_RESULT([$opal_libev_dir and $opal_libev_libdir])],
                     [AC_MSG_RESULT([$with_libev_libdir])])
           else
               AC_MSG_RESULT([(default search paths)])
               opal_libev_standard_header_location=yes
               opal_libev_standard_lib_location=yes
           fi
           AS_IF([test ! -z "$with_libev_libdir" && test "$with_libev_libdir" != "yes"],
                 [opal_libev_libdir="$with_libev_libdir"
                  opal_libev_standard_lib_location=no])

           OPAL_CHECK_PACKAGE([opal_libev],
                              [event.h],
                              [ev],
                              [event_base_new],
                              [],
                              [$opal_libev_dir],
                              [$opal_libev_libdir],
                              [opal_libev_support=1],
                              [opal_libev_support=0])
           CPPFLAGS="$opal_check_libev_save_CPPFLAGS"
           LDFLAGS="$opal_check_libev_save_LDFLAGS"
           LIBS="$opal_check_libev_save_LIBS"])

    AS_IF([test $opal_libev_support -eq 1],
          [LIBS="$LIBS $opal_libev_LIBS"

           AS_IF([test "$opal_libev_standard_header_location" != "yes"],
                 [CPPFLAGS="$CPPFLAGS $opal_libev_CPPFLAGS"])
           AS_IF([test "$opal_libev_standard_lib_location" != "yes"],
                 [LDFLAGS="$LDFLAGS $opal_libev_LDFLAGS"])])

    AC_MSG_CHECKING([will libev support be built])
    if test $opal_libev_support -eq 1; then
        AC_MSG_RESULT([yes])
        OPAL_EVENT_HEADER="<event.h>"
        AC_DEFINE_UNQUOTED([OPAL_EVENT_HEADER], [$OPAL_EVENT_HEADER],
                           [Location of event.h])
        OPAL_SUMMARY_ADD([[External Packages]],[[libev]],[libev],[$opal_libev_dir])
    else
        AC_MSG_RESULT([no])
    fi

    AC_DEFINE_UNQUOTED([OPAL_HAVE_LIBEV], [$opal_libev_support], [Whether we are building against libev])

    OPAL_VAR_SCOPE_POP
])dnl
