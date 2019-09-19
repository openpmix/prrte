dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
dnl Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2019      Research Organization for Information Science
dnl                         and Technology (RIST).  All rights reserved.
dnl
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl


# PRRTE_CHECK_COMPILER_VERSION_ID()
# ----------------------------------------------------
# Try to figure out the compiler's name and version to detect cases,
# where users compile PMIx with one version and compile the application
# with a different compiler.
#
AC_DEFUN([PRRTE_CHECK_COMPILER_VERSION_ID],
[
    PRRTE_CHECK_COMPILER(FAMILYID)
    PRRTE_CHECK_COMPILER_STRINGIFY(FAMILYNAME)
    PRRTE_CHECK_COMPILER(VERSION)
    PRRTE_CHECK_COMPILER_STRING(VERSION_STR)
])dnl


AC_DEFUN([PRRTE_CHECK_COMPILER], [
    lower=m4_tolower($1)
    AC_CACHE_CHECK([for compiler $lower], prrte_cv_compiler_[$1],
    [
            CPPFLAGS_orig=$CPPFLAGS
            CPPFLAGS="-I${top_srcdir}/src/include $CPPFLAGS"
            AC_TRY_RUN([
#include <stdio.h>
#include <stdlib.h>
#include "prrte_portable_platform.h"

int main (int argc, char * argv[])
{
    FILE * f;
    f=fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf (f, "%d", PLATFORM_COMPILER_$1);
    fclose(f);
    return 0;
}
            ], [
                eval prrte_cv_compiler_$1=`cat conftestval`;
            ], [
                eval prrte_cv_compiler_$1=0
            ], [
                eval prrte_cv_compiler_$1=0
            ])
            CPPFLAGS=$CPPFLAGS_orig
    ])
    AC_DEFINE_UNQUOTED([PRRTE_BUILD_PLATFORM_COMPILER_$1], $prrte_cv_compiler_[$1],
                       [The compiler $lower which PMIx was built with])
])dnl

AC_DEFUN([PRRTE_CHECK_COMPILER_STRING], [
    lower=m4_tolower($1)
    AC_CACHE_CHECK([for compiler $lower], prrte_cv_compiler_[$1],
    [
            CPPFLAGS_orig=$CPPFLAGS
            CPPFLAGS="-I${top_srcdir}/src/include $CPPFLAGS"
            AC_TRY_RUN([
#include <stdio.h>
#include <stdlib.h>
#include "prrte_portable_platform.h"

int main (int argc, char * argv[])
{
    FILE * f;
    f=fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf (f, "%s", PLATFORM_COMPILER_$1);
    fclose(f);
    return 0;
}
            ], [
                eval prrte_cv_compiler_$1=`cat conftestval`;
            ], [
                eval prrte_cv_compiler_$1=UNKNOWN
            ], [
                eval prrte_cv_compiler_$1=UNKNOWN
            ])
            CPPFLAGS=$CPPFLAGS_orig
    ])
    AC_DEFINE_UNQUOTED([PRRTE_BUILD_PLATFORM_COMPILER_$1], $prrte_cv_compiler_[$1],
                       [The compiler $lower which PMIx was built with])
])dnl




AC_DEFUN([PRRTE_CHECK_COMPILER_STRINGIFY], [
    lower=m4_tolower($1)
    AC_CACHE_CHECK([for compiler $lower], prrte_cv_compiler_[$1],
    [
            CPPFLAGS_orig=$CPPFLAGS
            CPPFLAGS="-I${top_srcdir}/src/include $CPPFLAGS"
            AC_TRY_RUN([
#include <stdio.h>
#include <stdlib.h>
#include "prrte_portable_platform.h"

int main (int argc, char * argv[])
{
    FILE * f;
    f=fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf (f, "%s", _STRINGIFY(PLATFORM_COMPILER_$1));
    fclose(f);
    return 0;
}
            ], [
                eval prrte_cv_compiler_$1=`cat conftestval`;
            ], [
                eval prrte_cv_compiler_$1=UNKNOWN
            ], [
                eval prrte_cv_compiler_$1=UNKNOWN
            ])
            CPPFLAGS=$CPPFLAGS_orig
    ])
    AC_DEFINE_UNQUOTED([PRRTE_BUILD_PLATFORM_COMPILER_$1], $prrte_cv_compiler_[$1],
                       [The compiler $lower which PRRTE was built with])
])dnl
