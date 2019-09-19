dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2014      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PRRTE_CHECK_OS_FLAVOR_SPECIFIC()
# ----------------------------------------------------
# Helper macro from PRRTE-CHECK-OS-FLAVORS(), below.
# $1 = macro to look for
# $2 = suffix of env variable to set with results
AC_DEFUN([PRRTE_CHECK_OS_FLAVOR_SPECIFIC],
[
    AC_MSG_CHECKING([$1])
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
     [[#ifndef $1
      error: this isnt $1
      #endif
     ]])],
                      [prrte_found_$2=yes],
                      [prrte_found_$2=no])
    AC_MSG_RESULT([$prrte_found_$2])
])dnl

# PRRTE_CHECK_OS_FLAVORS()
# ----------------------------------------------------
# Try to figure out the various OS flavors out there.
#
AC_DEFUN([PRRTE_CHECK_OS_FLAVORS],
[
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__NetBSD__], [netbsd])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__FreeBSD__], [freebsd])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__OpenBSD__], [openbsd])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__DragonFly__], [dragonfly])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__386BSD__], [386bsd])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__bsdi__], [bsdi])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__APPLE__], [apple])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__linux__], [linux])
    PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__sun__], [sun])
    AS_IF([test "$prrte_found_sun" = "no"],
          PRRTE_CHECK_OS_FLAVOR_SPECIFIC([__sun], [sun]))

    AS_IF([test "$prrte_found_sun" = "yes"],
          [prrte_have_solaris=1
           CFLAGS="$CFLAGS -D_REENTRANT"
           CPPFLAGS="$CPPFLAGS -D_REENTRANT"],
          [prrte_have_solaris=0])
    AC_DEFINE_UNQUOTED([PRRTE_HAVE_SOLARIS],
                       [$prrte_have_solaris],
                       [Whether or not we have solaris])

    AS_IF([test "$prrte_found_apple" = "yes"],
          [prrte_have_apple=1],
          [prrte_have_apple=0])
    AC_DEFINE_UNQUOTED([PRRTE_HAVE_APPLE],
                       [$prrte_have_apple],
                       [Whether or not we have apple])

    # check for sockaddr_in (a good sign we have TCP)
    AC_CHECK_HEADERS([netdb.h netinet/in.h netinet/tcp.h])
    AC_CHECK_TYPES([struct sockaddr_in],
                   [prrte_found_sockaddr=yes],
                   [prrte_found_sockaddr=no],
                   [AC_INCLUDES_DEFAULT
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif])
])dnl
