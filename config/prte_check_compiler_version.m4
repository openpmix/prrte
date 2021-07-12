dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
dnl Copyright (c) 2013-2017 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2019      Research Organization for Information Science
dnl                         and Technology (RIST).  All rights reserved.
dnl
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
dnl                         reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl


# PRTE_CHECK_COMPILER_VERSION_ID()
# ----------------------------------------------------
# Try to figure out the compiler's name and version to detect cases,
# where users compile PMIx with one version and compile the application
# with a different compiler.
#
AC_DEFUN([PRTE_CHECK_COMPILER_VERSION_ID],
[
    PRTE_CHECK_COMPILER(FAMILYID)
    PRTE_CHECK_COMPILER_STRINGIFY(FAMILYNAME)
    PRTE_CHECK_COMPILER(VERSION)
    PRTE_CHECK_COMPILER_STRING(VERSION_STR)
])dnl


AC_DEFUN([PRTE_CHECK_COMPILER], [
    lower=m4_tolower($1)
    AC_CACHE_CHECK([for compiler $lower], prte_cv_compiler_[$1],
    [
            CPPFLAGS_orig=$CPPFLAGS
            CPPFLAGS="-I${PRTE_TOP_SRCDIR}/src/include $CPPFLAGS"
AC_RUN_IFELSE([AC_LANG_SOURCE([[
#include <stdio.h>
#include <stdlib.h>
#include "prte_portable_platform.h"

int main (int argc, char * argv[])
{
    FILE * f;
    f=fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf (f, "%d", PLATFORM_COMPILER_$1);
    fclose(f);
    return 0;
}
            ]])],
            [
                eval prte_cv_compiler_$1=`cat conftestval`;
            ],
            [
                eval prte_cv_compiler_$1=0
            ],
            [
                eval prte_cv_compiler_$1=0
            ])
            CPPFLAGS=$CPPFLAGS_orig
    ])
    AC_DEFINE_UNQUOTED([PRTE_BUILD_PLATFORM_COMPILER_$1], $prte_cv_compiler_[$1],
                       [The compiler $lower which PMIx was built with])
])dnl

AC_DEFUN([PRTE_CHECK_COMPILER_STRING], [
    lower=m4_tolower($1)
    AC_CACHE_CHECK([for compiler $lower], prte_cv_compiler_[$1],
    [
            CPPFLAGS_orig=$CPPFLAGS
            CPPFLAGS="-I${PRTE_TOP_SRCDIR}/src/include $CPPFLAGS"
AC_RUN_IFELSE([AC_LANG_SOURCE([[
#include <stdio.h>
#include <stdlib.h>
#include "prte_portable_platform.h"

int main (int argc, char * argv[])
{
    FILE * f;
    f=fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf (f, "%s", PLATFORM_COMPILER_$1);
    fclose(f);
    return 0;
}
            ]])],
            [
                eval prte_cv_compiler_$1=`cat conftestval`;
            ],
            [
                eval prte_cv_compiler_$1=UNKNOWN
            ],
            [
                eval prte_cv_compiler_$1=UNKNOWN
            ])
            CPPFLAGS=$CPPFLAGS_orig
    ])
    AC_DEFINE_UNQUOTED([PRTE_BUILD_PLATFORM_COMPILER_$1], $prte_cv_compiler_[$1],
                       [The compiler $lower which PMIx was built with])
])dnl




AC_DEFUN([PRTE_CHECK_COMPILER_STRINGIFY], [
    lower=m4_tolower($1)
    AC_CACHE_CHECK([for compiler $lower], prte_cv_compiler_[$1],
    [
            CPPFLAGS_orig=$CPPFLAGS
            CPPFLAGS="-I${PRTE_TOP_SRCDIR}/src/include $CPPFLAGS"
            AC_RUN_IFELSE([AC_LANG_SOURCE([[
#include <stdio.h>
#include <stdlib.h>
#include "prte_portable_platform.h"

int main (int argc, char * argv[])
{
    FILE * f;
    f=fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf (f, "%s", _STRINGIFY(PLATFORM_COMPILER_$1));
    fclose(f);
    return 0;
}
            ]])], [
                eval prte_cv_compiler_$1=`cat conftestval`;
            ], [
                eval prte_cv_compiler_$1=UNKNOWN
            ], [
                eval prte_cv_compiler_$1=UNKNOWN
            ])
            CPPFLAGS=$CPPFLAGS_orig
    ])
    AC_DEFINE_UNQUOTED([PRTE_BUILD_PLATFORM_COMPILER_$1], $prte_cv_compiler_[$1],
                       [The compiler $lower which PRTE was built with])
])dnl
