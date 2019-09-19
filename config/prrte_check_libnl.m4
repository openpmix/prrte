dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2015-2017 Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2017      Cisco Systems, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

dnl
dnl More libnl v1/v3 sadness: the two versions are not compatible
dnl and will not work correctly if simultaneously linked into the
dnl same applications.  Unfortunately, they *will* link into the
dnl same image!  On platforms like CentOS 7, libibverbs depends on
dnl libnl-3.so.200 and friends, so if libnl3-devel packages are not
dnl installed, but libnl-devel are, Open MPI should not try to use libnl.
dnl
dnl GROSS: libnl wants us to either use pkg-config (which we
dnl cannot assume is always present) or we need to look in a
dnl particular directory for the right libnl3 include files.  For
dnl now, just hard code the special path into this logic.
dnl
dnl _PRRTE_CHECK_PACKAGE_LIB() invokes PRRTE_LIBNL_SANITY_CHECK() in order
dnl to keep track of which libraries depend on libnl and which libraries
dnl depend on libnl3.
dnl Open MPI will not be able to build a component vs a given version of libnl
dnl if the other libnl version is required by some third party components.
dnl At the end of configure, display a summary of who is using what, and aborts
dnl if both libnl versions are required.

dnl PRRTE_LIBNL_SANITY_INIT()
dnl --------------------------------------------------------------------
AC_DEFUN([PRRTE_LIBNL_SANITY_INIT], [
    prrte_libnl_version=0
    prrte_libnlv1_libs=
    prrte_libnlv3_libs=
    AC_ARG_WITH([libnl],
                [AC_HELP_STRING([--with-libnl(=DIR)],
                                [Directory prefix for libnl (typically only necessary if libnl is installed in a location that the compiler/linker will not search by default)])])

    # The --with options carry two pieces of information: 1) do
    # you want a specific version of libnl, and 2) where that
    # version of libnl lives.  For simplicity, let's separate
    # those two pieces of information.
    case "$with_libnl" in
        no)
            # Nope, don't want it
            prrte_want_libnl=no
            ;;
        "")
            # Just try to build with libnl
            prrte_want_libnl=try
            prrte_libnl_location=
            ;;
        yes)
            # Yes, definitely want it
            prrte_want_libnl=yes
            prrte_libnl_location=
            ;;
        *)
            # Yes, definitely want it -- at a specific location
            prrte_want_libnl=yes
            prrte_libnl_location=$with_libnl
            ;;
    esac
])

dnl PRRTE_LIBNL_SANITY_FAIL_MSG(lib)
dnl
dnl Helper to pring a big warning message when we detect a libnl conflict.
dnl
dnl --------------------------------------------------------------------
AC_DEFUN([PRRTE_LIBNL_SANITY_FAIL_MSG], [
    AC_MSG_WARN([This is a configuration that is *known* to cause run-time crashes.])
    AC_MSG_WARN([This is an error in lib$1 (not Open MPI).])
    AC_MSG_WARN([Open MPI will therefore skip using lib$1.])
])

dnl PRRTE_LIBNL_SANITY_CHECK(lib, function, LIBS, libnl_check_ok)
dnl
dnl This macro is invoked from PRRTE_CHECK_PACKAGE to make sure that
dnl new libraries that are added to LIBS do not pull in conflicting
dnl versions of libnl.  E.g., if we already have a library in LIBS
dnl that pulls in libnl v3, if PRRTE_CHECK_PACKAGE is later called that
dnl pulls in a library that pulls in libnl v1, this macro will detect
dnl the conflict and will abort configure.
dnl
dnl We abort rather than silently ignore this library simply because
dnl we are now multiple levels deep in the M4 "call stack", and this
dnl layer does not know the intent of the user.  Hence, all we can do
dnl is abort with a hopefully helpful error message (that we aborted
dnl because Open MPI would have been built in a configuration that is
dnl known to crash).
dnl
dnl --------------------------------------------------------------------
AC_DEFUN([PRRTE_LIBNL_SANITY_CHECK], [
    PRRTE_VAR_SCOPE_PUSH([prrte_libnl_sane])
    prrte_libnl_sane=1
    case $host in
        *linux*)
            PRRTE_LIBNL_SANITY_CHECK_LINUX($1, $2, $3, prrte_libnl_sane)
            ;;
    esac

    $4=$prrte_libnl_sane
    PRRTE_VAR_SCOPE_POP([prrte_libnl_sane])
])

dnl
dnl Simple helper for PRRTE_LIBNL_SANITY_CHECK
dnl $1: library name
dnl $2: function
dnl $3: LIBS
dnl $4: output variable (1=ok, 0=not ok)
dnl
AC_DEFUN([PRRTE_LIBNL_SANITY_CHECK_LINUX], [
    PRRTE_VAR_SCOPE_PUSH([this_requires_v1 libnl_sane this_requires_v3 ldd_output result_msg])

    AC_LANG_PUSH(C)

    AC_MSG_CHECKING([if lib$1 requires libnl v1 or v3])
    cat > conftest_c.$ac_ext << EOF
extern void $2 (void);
int main(int argc, char *argv[[]]) {
    $2 ();
    return 0;
}
EOF

    this_requires_v1=0
    this_requires_v3=0
    result_msg=
    PRRTE_LOG_COMMAND([$CC -o conftest $CFLAGS $CPPFLAGS conftest_c.$ac_ext $LDFLAGS -l$1 $LIBS $3],
        [ldd_output=`ldd conftest`
         AS_IF([echo $ldd_output | grep -q libnl-3.so],
               [this_requires_v3=1
                result_msg="v3"])
         AS_IF([echo $ldd_output | grep -q libnl.so],
               [this_requires_v1=1
                result_msg="v1 $result_msg"])
         AC_MSG_RESULT([$result_msg])
         ],
        [AC_MSG_WARN([Could not link a simple program with lib $1])
        ])

    # Assume that our configuration is sane; this may get reset below
    libnl_sane=1

    # Note: in all the checks below, only add this library to the list
    # of libraries (for v1 or v3 as relevant) if we do not fail.
    # I.e., assume that a higher level will refuse to use this library
    # if we return failure.

    # Does this library require both v1 and v3?  If so, fail.
    AS_IF([test $this_requires_v1 -eq 1 && test $this_requires_v3 -eq 1],
          [AC_MSG_WARN([Unfortunately, lib$1 links to both libnl and libnl-3.])
           PRRTE_LIBNL_SANITY_FAIL_MSG($1)
           libnl_sane=0])

    # Does this library require v1, but some prior library required
    # v3?  If so, fail.
    AS_IF([test $libnl_sane -eq 1 && test $this_requires_v1 -eq 1],
          [AS_IF([test $prrte_libnl_version -eq 3],
                 [AC_MSG_WARN([libnl version conflict: $prrte_libnlv3_libs requires libnl-3 whereas $1 requires libnl])
                  PRRTE_LIBNL_SANITY_FAIL_MSG($1)
                  libnl_sane=0],
                 [prrte_libnlv1_libs="$prrte_libnlv1_libs $1"
                  PRRTE_UNIQ([prrte_libnlv1_libs])
                  prrte_libnl_version=1])
           ])

    # Does this library require v3, but some prior library required
    # v1?  If so, fail.
    AS_IF([test $libnl_sane -eq 1 && test $this_requires_v3 -eq 1],
          [AS_IF([test $prrte_libnl_version -eq 1],
                 [AC_MSG_WARN([libnl version conflict: $prrte_libnlv1_libs requires libnl whereas lib$1 requires libnl-3])
                  PRRTE_LIBNL_SANITY_FAIL_MSG($1)
                  libnl_sane=0],
                 [prrte_libnlv3_libs="$prrte_libnlv3_libs $1"
                  PRRTE_UNIQ([prrte_libnlv3_libs])
                  prrte_libnl_version=3])
          ])

    AC_LANG_POP(C)
    rm -f conftest conftest_c.$ac_ext

    $4=$libnl_sane

    PRRTE_VAR_SCOPE_POP([ldd_output libnl_sane this_requires_v1 this_requires_v3 result_msg])
])

dnl
dnl Check for libnl-3.
dnl
dnl Inputs:
dnl
dnl $1: prefix where to look for libnl-3
dnl $2: var name prefix of _CPPFLAGS and _LDFLAGS and _LIBS
dnl
dnl Outputs:
dnl
dnl - Set $2_CPPFLAGS necessary to compile with libnl-3
dnl - Set $2_LDFLAGS necessary to link with libnl-3
dnl - Set $2_LIBS necessary to link with libnl-3
dnl - Set PRRTE_HAVE_LIBNL3 1 if libnl-3 will be used
dnl
AC_DEFUN([PRRTE_CHECK_LIBNL_V3],[
    PRRTE_VAR_SCOPE_PUSH([CPPFLAGS_save prrte_tmp_CPPFLAGS LIBS_save LDFLAGS_save])
    AC_MSG_NOTICE([checking for libnl v3])

    AS_IF([test "$prrte_want_libnl" != "no"],
          [AS_IF([test -z "$prrte_libnl_location"],
                 [AC_MSG_CHECKING([for /usr/include/libnl3])
                  AS_IF([test -d "/usr/include/libnl3"],
                        [prrte_tmp_CPPFLAGS=-I/usr/include/libnl3
                         prrte_libnlv3_happy=1
                         AC_MSG_RESULT([found])],
                        [AC_MSG_RESULT([not found])
                         AC_MSG_CHECKING([for /usr/local/include/libnl3])
                         AS_IF([test -d "/usr/local/include/libnl3"],
                               [prrte_tmp_CPPFLAGS=-I/usr/local/include/netlink3
                                prrte_libnlv3_happy=1
                                AC_MSG_RESULT([found])],
                               [prrte_libnlv3_happy=0
                                AC_MSG_RESULT([not found])])])],
                 [AC_MSG_CHECKING([for $1/include/libnl3])
                  AS_IF([test -d "$1/include/libnl3"],
                        [prrte_tmp_CPPFLAGS="-I$1/include/libnl3"
                         prrte_libnlv3_happy=1
                         AC_MSG_RESULT([found])],
                        [prrte_libnlv3_happy=0
                         AC_MSG_RESULT([not found])])])
           CPPFLAGS_save=$CPPFLAGS
           CPPFLAGS="$prrte_tmp_CPPFLAGS $CPPFLAGS"

           # Random note: netlink/version.h is only in libnl v3 - it is not in libnl v1.
           # Also, nl_recvmsgs_report is only in libnl v3.
           AS_IF([test $prrte_libnlv3_happy -eq 1],
                 [PRRTE_CHECK_PACKAGE([$2],
                                     [netlink/version.h],
                                     [nl-3],
                                     [nl_recvmsgs_report],
                                     [],
                                     [$1],
                                     [],
                                     [],
                                     [prrte_libnlv3_happy=0])

                  # Note that PRRTE_CHECK_PACKAGE is going to add
                  # -I$dir/include into $2_CPPFLAGS.  But because libnl v3
                  # puts the headers in $dir/include/libnl3, we need to
                  # overwrite $2_CPPFLAGS with -I$dir/include/libnl3.  We can do
                  # this unconditionally; we don't have to check for
                  # success (checking for success occurs below).
                  $2_CPPFLAGS=$prrte_tmp_CPPFLAGS])

           # If we found libnl-3, we *also* need libnl-route-3
           LIBS_save=$LIBS
           LDFLAGS_save=$LDFLAGS
           AS_IF([test -n "$$2_LDFLAGS"],
                 [LDFLAGS="$$2_LDFLAGS $LDFLAGS"])
           AS_IF([test $prrte_libnlv3_happy -eq 1],
                 [AC_SEARCH_LIBS([nl_rtgen_request],
                                 [nl-route-3],
                                 [],
                                 [prrte_libnlv3_happy=0])])
           LIBS=$LIBS_save
           LDFLAGS=$LDFLAGS_save

           # Just because libnl* is evil, double check that the
           # netlink/version.h we found was for libnl v3.  As far as we
           # know, netlink/version.h only first appeared in version
           # 3... but let's really be sure.
           AS_IF([test $prrte_libnlv3_happy -eq 1],
                 [AC_MSG_CHECKING([to ensure these really are libnl v3 headers])
                  AS_IF([test -n "$$2_CPPFLAGS"],
                        [CPPFLAGS="$$2_CPPFLAGS $CPPFLAGS"])
                  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <netlink/netlink.h>
#include <netlink/version.h>
#ifndef LIBNL_VER_MAJ
#error "LIBNL_VER_MAJ not defined!"
#endif
/* to the best of our knowledge, version.h only exists in libnl v3 */
#if LIBNL_VER_MAJ != 3
#error "LIBNL_VER_MAJ != 3, I am sad"
#endif
                                     ]])],
                                    [AC_MSG_RESULT([yes])],
                                    [AC_MSG_RESULT([no])
                                     prrte_libnlv3_happy=0])])

           CPPFLAGS=$CPPFLAGS_save],

          [prrte_libnlv3_happy=0])

    # If we found everything
    AS_IF([test $prrte_libnlv3_happy -eq 1],
          [$2_LIBS="-lnl-3 -lnl-route-3"
           PRRTE_HAVE_LIBNL3=1],
          [# PRRTE_CHECK_PACKAGE(...,nl_recvmsgs_report,...) might have set the variables below
           # so reset them if libnl v3 cannot be used
           $2_CPPFLAGS=""
           $2_LDFLAGS=""
           $2_LIBS=""])

   PRRTE_VAR_SCOPE_POP
])

dnl
dnl Check for libnl.
dnl
dnl Inputs:
dnl
dnl $1: prefix where to look for libnl
dnl $2: var name prefix of _CPPFLAGS and _LDFLAGS and _LIBS
dnl
dnl Outputs:
dnl
dnl - Set $2_CPPFLAGS necessary to compile with libnl
dnl - Set $2_LDFLAGS necessary to link with libnl
dnl - Set $2_LIBS necessary to link with libnl
dnl - Set PRRTE_HAVE_LIBNL3 0 if libnl will be used
dnl
AC_DEFUN([PRRTE_CHECK_LIBNL_V1],[
    AC_MSG_NOTICE([checking for libnl v1])

    AS_IF([test "$prrte_want_libnl" != "no"],
          [PRRTE_CHECK_PACKAGE([$2],
                              [netlink/netlink.h],
                              [nl],
                              [nl_connect],
                              [-lm],
                              [$1],
                              [],
                              [prrte_libnlv1_happy=1],
                              [prrte_libnlv1_happy=0])],
          [prrte_libnlv1_happy=0])

    AS_IF([test $prrte_libnlv1_happy -eq 1],
          [$2_LIBS="-lnl -lm"
           PRRTE_HAVE_LIBNL3=0])
])

dnl
dnl Summarize libnl and libnl3 usage,
dnl and abort if conflict is found
dnl
dnl Print the list of libraries that use libnl,
dnl the list of libraries that use libnl3,
dnl and aborts if both libnl and libnl3 are used.
dnl
AC_DEFUN([PRRTE_CHECK_LIBNL_SUMMARY],[
    AC_MSG_CHECKING([for libraries that use libnl v1])
    AS_IF([test -n "$prrte_libnlv1_libs"],
          [AC_MSG_RESULT([$prrte_libnlv1_libs])],
          [AC_MSG_RESULT([(none)])])
    AC_MSG_CHECKING([for libraries that use libnl v3])
    AS_IF([test -n "$prrte_libnlv3_libs"],
          [AC_MSG_RESULT([$prrte_libnlv3_libs])],
          [AC_MSG_RESULT([(none)])])
    AS_IF([test -n "$prrte_libnlv1_libs" && test -n "$prrte_libnlv3_libs"],
          [AC_MSG_WARN([libnl v1 and libnl v3 have been found as dependent libraries])
           AC_ERROR([This is a configuration that is known to cause run-time crashes])])
])
