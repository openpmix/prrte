dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2021 IBM Corporation.  All rights reserved.
dnl
dnl Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

AC_DEFUN([_PRTE_CFLAGS_FAIL_SEARCH],[
    AC_REQUIRE([AC_PROG_GREP])
    if test -s conftest.err ; then
        $GREP -iq $1 conftest.err
        if test "$?" = "0" ; then
            prte_cv_cc_[$2]=0
        fi
    fi
])

AC_DEFUN([_PRTE_CHECK_SPECIFIC_CFLAGS], [
AC_MSG_CHECKING(if $CC supports ([$1]))
            CFLAGS_orig=$CFLAGS
            PRTE_APPEND_UNIQ([CFLAGS], ["$1"])
            AC_CACHE_VAL(prte_cv_cc_[$2], [
                   AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [$3])],
                                   [
                                    prte_cv_cc_[$2]=1
                                    _PRTE_CFLAGS_FAIL_SEARCH("ignored\|not recognized\|not supported\|not compatible\|unrecognized\|unknown", [$2])
                                   ],
                                    prte_cv_cc_[$2]=1
                                    _PRTE_CFLAGS_FAIL_SEARCH("ignored\|not recognized\|not supported\|not compatible\|unrecognized\|unknown\|error", [$2])
                                 )])
            if test "$prte_cv_cc_[$2]" = "0" ; then
                CFLAGS="$CFLAGS_orig"
                AC_MSG_RESULT([no])
            else
                AC_MSG_RESULT([yes])
            fi
])


AC_DEFUN([_PRTE_CHECK_LTO_FLAG], [
    if test "$PRTE_PMIX_LTO_CAPABILITY" = "0"; then
        chkflg=`echo $1 | grep -- lto`
        if test -n "$chkflg"; then
            AC_MSG_WARN([Configure has detected the presence of one or more])
            AC_MSG_WARN([compiler directives involving the lto optimizer])
            AC_MSG_WARN([$2. PRRTE does not currently support such directives])
            AC_MSG_WARN([as they conflict with the plugin architecture of the])
            AC_MSG_WARN([PRRTE library. The directive is being ignored.])
            newflg=
            for item in $1; do
                chkflg=`echo $item | grep -- lto`
                if test ! -n "$chkflg"; then
                    newflg+="$item "
                fi
            done
            $2="$newflg"
        fi
    fi
])
