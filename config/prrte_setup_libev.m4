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
AC_DEFUN([PRRTE_LIBEV_CONFIG],[
    PRRTE_VAR_SCOPE_PUSH([prrte_libev_dir prrte_libev_libdir prrte_libev_standard_header_location prrte_libev_standard_lib_location])

    AC_ARG_WITH([libev],
                [AC_HELP_STRING([--with-libev=DIR],
                                [Search for libev headers and libraries in DIR ])])
    PRRTE_CHECK_WITHDIR([libev], [$with_libev], [include/event.h])

    AC_ARG_WITH([libev-libdir],
                [AC_HELP_STRING([--with-libev-libdir=DIR],
                                [Search for libev libraries in DIR ])])
    PRRTE_CHECK_WITHDIR([libev-libdir], [$with_livev_libdir], [libev.*])

    prrte_libev_support=0

    AS_IF([test -n "$with_libev" && test "$with_libev" != "no"],
          [AC_MSG_CHECKING([for libev in])
           prrte_check_libev_save_CPPFLAGS="$CPPFLAGS"
           prrte_check_libeve_save_LDFLAGS="$LDFLAGS"
           prrte_check_libev_save_LIBS="$LIBS"
           if test "$with_libev" != "yes"; then
               prrte_libev_dir=$with_libev/include
               prrte_libev_standard_header_location=no
               prrte_libev_standard_lib_location=no
               AS_IF([test -z "$with_libev_libdir" || test "$with_libev_libdir" = "yes"],
                     [if test -d $with_libev/lib; then
                          prrte_libev_libdir=$with_libev/lib
                      elif test -d $with_libev/lib64; then
                          prrte_libev_libdir=$with_libev/lib64
                      else
                          AC_MSG_RESULT([Could not find $with_libev/lib or $with_libev/lib64])
                          AC_MSG_ERROR([Can not continue])
                      fi
                      AC_MSG_RESULT([$prrte_libev_dir and $prrte_libev_libdir])],
                     [AC_MSG_RESULT([$with_libev_libdir])])
           else
               AC_MSG_RESULT([(default search paths)])
               prrte_libev_standard_header_location=yes
               prrte_libev_standard_lib_location=yes
           fi
           AS_IF([test ! -z "$with_libev_libdir" && test "$with_libev_libdir" != "yes"],
                 [prrte_libev_libdir="$with_libev_libdir"
                  prrte_libev_standard_lib_location=no])

           PRRTE_CHECK_PACKAGE([prrte_libev],
                              [event.h],
                              [ev],
                              [event_base_new],
                              [],
                              [$prrte_libev_dir],
                              [$prrte_libev_libdir],
                              [prrte_libev_support=1],
                              [prrte_libev_support=0])
           CPPFLAGS="$prrte_check_libev_save_CPPFLAGS"
           LDFLAGS="$prrte_check_libev_save_LDFLAGS"
           LIBS="$prrte_check_libev_save_LIBS"])

    AS_IF([test $prrte_libev_support -eq 1],
          [LIBS="$LIBS $prrte_libev_LIBS"
           PRRTE_WRAPPER_FLAGS_ADD(LIBS, $prrte_libev_LIBS)

           AS_IF([test "$prrte_libev_standard_header_location" != "yes"],
                 [CPPFLAGS="$CPPFLAGS $prrte_libev_CPPFLAGS"]
                  PRRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prrte_libev_CPPFLAGS))
           AS_IF([test "$prrte_libev_standard_lib_location" != "yes"],
                 [LDFLAGS="$LDFLAGS $prrte_libev_LDFLAGS"
                  PRRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prrte_libev_LDFLAGS)])])

    AC_MSG_CHECKING([will libev support be built])
    if test $prrte_libev_support -eq 1; then
        AC_MSG_RESULT([yes])
        PRRTE_EVENT_HEADER="<event.h>"
        AC_DEFINE_UNQUOTED([PRRTE_EVENT_HEADER], [$PRRTE_EVENT_HEADER],
                           [Location of event.h])
        PRRTE_SUMMARY_ADD([[Required Packages]],[[libev]],[libev],[$prrte_libev_dir])
    else
        AC_MSG_RESULT([no])
    fi

    AC_DEFINE_UNQUOTED([PRRTE_HAVE_LIBEV], [$prrte_libev_support], [Whether we are building against libev])

    PRRTE_VAR_SCOPE_POP
])dnl
