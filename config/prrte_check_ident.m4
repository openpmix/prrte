dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
dnl Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl
dnl defines:
dnl   PRRTE_$1_USE_PRAGMA_IDENT
dnl   PRRTE_$1_USE_IDENT
dnl   PRRTE_$1_USE_CONST_CHAR_IDENT
dnl

# PRRTE_CHECK_IDENT(compiler-env, compiler-flags,
# file-suffix, lang) Try to compile a source file containing
# a #pragma ident, and determine whether the ident was
# inserted into the resulting object file
# -----------------------------------------------------------
AC_DEFUN([PRRTE_CHECK_IDENT], [
    AC_MSG_CHECKING([for $4 ident string support])

    prrte_pragma_ident_happy=0
    prrte_ident_happy=0
    prrte_static_const_char_happy=0
    _PRRTE_CHECK_IDENT(
        [$1], [$2], [$3],
        [[#]pragma ident], [],
        [prrte_pragma_ident_happy=1
         prrte_message="[#]pragma ident"],
        _PRRTE_CHECK_IDENT(
            [$1], [$2], [$3],
            [[#]ident], [],
            [prrte_ident_happy=1
             prrte_message="[#]ident"],
            _PRRTE_CHECK_IDENT(
                [$1], [$2], [$3],
                [[#]pragma comment(exestr, ], [)],
                [prrte_pragma_comment_happy=1
                 prrte_message="[#]pragma comment"],
                [prrte_static_const_char_happy=1
                 prrte_message="static const char[[]]"])))

    AC_DEFINE_UNQUOTED([PRRTE_$1_USE_PRAGMA_IDENT],
        [$prrte_pragma_ident_happy], [Use #pragma ident strings for $4 files])
    AC_DEFINE_UNQUOTED([PRRTE_$1_USE_IDENT],
        [$prrte_ident_happy], [Use #ident strings for $4 files])
    AC_DEFINE_UNQUOTED([PRRTE_$1_USE_PRAGMA_COMMENT],
        [$prrte_pragma_comment_happy], [Use #pragma comment for $4 files])
    AC_DEFINE_UNQUOTED([PRRTE_$1_USE_CONST_CHAR_IDENT],
        [$prrte_static_const_char_happy], [Use static const char[] strings for $4 files])

    AC_MSG_RESULT([$prrte_message])

    unset prrte_pragma_ident_happy prrte_ident_happy prrte_static_const_char_happy prrte_message
])

# _PRRTE_CHECK_IDENT(compiler-env, compiler-flags,
# file-suffix, header_prefix, header_suffix, action-if-success, action-if-fail)
# Try to compile a source file containing a #-style ident,
# and determine whether the ident was inserted into the
# resulting object file
# -----------------------------------------------------------
AC_DEFUN([_PRRTE_CHECK_IDENT], [
    eval prrte_compiler="\$$1"
    eval prrte_flags="\$$2"

    prrte_ident="string_not_coincidentally_inserted_by_the_compiler"
    cat > conftest.$3 <<EOF
$4 "$prrte_ident" $5
int main(int argc, char** argv);
int main(int argc, char** argv) { return 0; }
EOF

    # "strings" won't always return the ident string.  objdump isn't
    # universal (e.g., OS X doesn't have it), and ...other
    # complications.  So just try to "grep" for the string in the
    # resulting object file.  If the ident is found in "strings" or
    # the grep succeeds, rule that we have this flavor of ident.

    echo "configure:__oline__: $1" >&5
    prrte_output=`$prrte_compiler $prrte_flags -c conftest.$3 -o conftest.${OBJEXT} 2>&1 1>/dev/null`
    prrte_status=$?
    AS_IF([test $prrte_status = 0],
          [test -z "$prrte_output"
           prrte_status=$?])
    PRRTE_LOG_MSG([\$? = $prrte_status], 1)
    AS_IF([test $prrte_status = 0 && test -f conftest.${OBJEXT}],
          [prrte_output="`strings -a conftest.${OBJEXT} | grep $prrte_ident`"
           grep $prrte_ident conftest.${OBJEXT} 2>&1 1>/dev/null
           prrte_status=$?
           AS_IF([test "$prrte_output" != "" || test "$prrte_status" = "0"],
                 [$6],
                 [$7])],
          [PRRTE_LOG_MSG([the failed program was:])
           PRRTE_LOG_FILE([conftest.$3])
           $7])

    unset prrte_compiler prrte_flags prrte_output prrte_status
    rm -rf conftest.* conftest${EXEEXT}
])dnl
