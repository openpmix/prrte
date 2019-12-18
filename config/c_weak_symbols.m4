dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2014      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2014 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2019      Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# _PRRTE_C_WEAK_SYMBOLS(action_if_found, [action_if_not_found])
# ------------------------------------------------------------
AC_DEFUN([_PRRTE_C_WEAK_SYMBOLS],[
    # need two files because icc will incorrectly not create the
    # symbols if they are not used in the object file in which they
    # are defined.  Blah!
    # To get to compile with icc, have them in a separate header.
    cat > conftest_weak.h <<EOF
int real(int i);
int fake(int i);
EOF

    cat > conftest_weak.c <<EOF
#include "conftest_weak.h"
#pragma weak fake = real
int real(int i) { return i; }
EOF

    cat > conftest.c <<EOF
#include "conftest_weak.h"
int main() { return fake(3); }
EOF

# Try the compile
PRRTE_LOG_COMMAND(
    [$CC $CFLAGS  -c conftest_weak.c],
    [PRRTE_LOG_COMMAND(
        [$CC $CFLAGS  conftest.c conftest_weak.o -o conftest $LDFLAGS $LIBS],
        [prrte_c_weak_symbols_happy=1],
        [prrte_c_weak_symbols_happy=0])],
    [prrte_c_weak_symbols_happy=0])

    AS_IF([test "$prrte_c_weak_symbols_happy" = "1"], [$1], [$2])

    unset prrte_c_weak_symbols_happy
    rm -f conftest_weak.h conftest_weak.c conftest.c conftest
])


# PRRTE_C_WEAK_SYMBOLS()
# ---------------------
# sets PRRTE_C_HAVE_WEAK_SYMBOLS=1 if C compiler has support for weak symbols
AC_DEFUN([PRRTE_C_WEAK_SYMBOLS],[
    AC_CACHE_CHECK([for weak symbol support],
                   [prrte_cv_c_weak_symbols],
                   [_PRRTE_C_WEAK_SYMBOLS([prrte_cv_c_weak_symbols="yes"],
                                         [prrte_cv_c_weak_symbols="no"])])

    AS_IF([test "$prrte_cv_c_weak_symbols" = "yes"],
          [PRRTE_C_HAVE_WEAK_SYMBOLS=1], [PRRTE_C_HAVE_WEAK_SYMBOLS=0])
]) dnl

########################################################################

# _PRRTE_C_MACRO_WEAK_SYMBOLS(action_if_found, [action_if_not_found])
# ------------------------------------------------------------
AC_DEFUN([_PRRTE_C_MACRO_WEAK_SYMBOLS],[
    # need two files because icc will incorrectly not create the
    # symbols if they are not used in the object file in which they
    # are defined.  Blah!
    # To get to compile with icc, have them in a separate header.
    cat > conftest_weak.h <<EOF
int real(int i);
int fake1(int i);
int fake2(int i);

#define GENERATE_WEAK_PRAGMA(x) _Pragma(#x)
#define GENERATE_TWO_WEAK_PRAGMAS(real, fake) \
    GENERATE_WEAK_PRAGMA(weak fake##1 = real) \
    GENERATE_WEAK_PRAGMA(weak fake##2 = real)
EOF

    cat > conftest_weak.c <<EOF
#include "conftest_weak.h"
GENERATE_TWO_WEAK_PRAGMAS(real, fake)
int real(int i) { return i; }
EOF

    cat > conftest.c <<EOF
#include "conftest_weak.h"
int main() { return fake1(3) + fake2(3); }
EOF

# Try the compile
PRRTE_LOG_COMMAND(
    [$CC $CFLAGS  -c conftest_weak.c],
    [PRRTE_LOG_COMMAND(
        [$CC $CFLAGS  conftest.c conftest_weak.o -o conftest $LDFLAGS $LIBS],
        [prrte_c_macro_weak_symbols_happy=1],
        [prrte_c_macro_weak_symbols_happy=0])],
    [prrte_c_macro_weak_symbols_happy=0])

    AS_IF([test "$prrte_c_macro_weak_symbols_happy" = "1"], [$1], [$2])

    unset prrte_c_macro_weak_symbols_happy
    rm -f conftest_weak.h conftest_weak.c conftest.c conftest
])

# PRRTE_C_MACRO_WEAK_SYMBOLS
# ---------------------
# Sets PRRTE_C_HAVE_MACRO_WEAK_SYMBOLS=1 if C compiler has support for weak
# symbols
AC_DEFUN([PRRTE_C_MACRO_WEAK_SYMBOLS],[
    AC_CACHE_CHECK([for macro weak symbol support],
                   [prrte_cv_c_macro_weak_symbols],
                   [_PRRTE_C_MACRO_WEAK_SYMBOLS([prrte_cv_c_macro_weak_symbols="yes"],
                                               [prrte_cv_c_macro_weak_symbols="no"])])

    AS_IF([test "$prrte_cv_c_macro_weak_symbols" = "yes"],
          [PRRTE_C_HAVE_MACRO_WEAK_SYMBOLS=1],
          [PRRTE_C_HAVE_MACRO_WEAK_SYMBOLS=0])
]) dnl
