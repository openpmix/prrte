dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2018 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2008-2018 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
dnl Copyright (c) 2015-2018 Research Organization for Information Science
dnl                         and Technology (RIST).  All rights reserved.
dnl Copyright (c) 2014-2018 Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017      Amazon.com, Inc. or its affiliates.  All Rights
dnl                         reserved.
dnl Copyright (c) 2020      Google, LLC. All rights reserved.
dnl Copyright (c) 2020      Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

dnl This is a C test to see if 128-bit __atomic_compare_exchange_n()
dnl actually works (e.g., it compiles and links successfully on
dnl ARM64+clang, but returns incorrect answers as of August 2018).
AC_DEFUN([PRTE_ATOMIC_COMPARE_EXCHANGE_N_TEST_SOURCE],[[
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
typedef union {
    uint64_t fake@<:@2@:>@;
    __int128 real;
} prte128;
static void test1(void)
{
    // As of Aug 2018, we could not figure out a way to assign 128-bit
    // constants -- the compilers would not accept it.  So use a fake
    // union to assign 2 uin64_t's to make a single __int128.
    prte128 ptr      = { .fake = { 0xFFEEDDCCBBAA0099, 0x8877665544332211 }};
    prte128 expected = { .fake = { 0x11EEDDCCBBAA0099, 0x88776655443322FF }};
    prte128 desired  = { .fake = { 0x1122DDCCBBAA0099, 0x887766554433EEFF }};
    bool r = __atomic_compare_exchange_n(&ptr.real, &expected.real,
                                         desired.real, true,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    if ( !(r == false && ptr.real == expected.real)) {
        exit(1);
    }
}
static void test2(void)
{
    prte128 ptr =      { .fake = { 0xFFEEDDCCBBAA0099, 0x8877665544332211 }};
    prte128 expected = ptr;
    prte128 desired =  { .fake = { 0x1122DDCCBBAA0099, 0x887766554433EEFF }};
    bool r = __atomic_compare_exchange_n(&ptr.real, &expected.real,
                                         desired.real, true,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    if (!(r == true && ptr.real == desired.real)) {
        exit(2);
    }
}
int main(int argc, char** argv)
{
    test1();
    test2();
    return 0;
}
]])

dnl ------------------------------------------------------------------

dnl This is a C test to see if 128-bit __sync_bool_compare_and_swap()
dnl actually works (e.g., it compiles and links successfully on
dnl ARM64+clang, but returns incorrect answers as of August 2018).
AC_DEFUN([PRTE_SYNC_BOOL_COMPARE_AND_SWAP_TEST_SOURCE],[[
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
typedef union {
    uint64_t fake@<:@2@:>@;
    __int128 real;
} prte128;
static void test1(void)
{
    // As of Aug 2018, we could not figure out a way to assign 128-bit
    // constants -- the compilers would not accept it.  So use a fake
    // union to assign 2 uin64_t's to make a single __int128.
    prte128 ptr    = { .fake = { 0xFFEEDDCCBBAA0099, 0x8877665544332211 }};
    prte128 oldval = { .fake = { 0x11EEDDCCBBAA0099, 0x88776655443322FF }};
    prte128 newval = { .fake = { 0x1122DDCCBBAA0099, 0x887766554433EEFF }};
    bool r = __sync_bool_compare_and_swap(&ptr.real, oldval.real, newval.real);
    if (!(r == false && ptr.real != newval.real)) {
        exit(1);
    }
}
static void test2(void)
{
    prte128 ptr    = { .fake = { 0xFFEEDDCCBBAA0099, 0x8877665544332211 }};
    prte128 oldval = ptr;
    prte128 newval = { .fake = { 0x1122DDCCBBAA0099, 0x887766554433EEFF }};
    bool r = __sync_bool_compare_and_swap(&ptr.real, oldval.real, newval.real);
    if (!(r == true && ptr.real == newval.real)) {
        exit(2);
    }
}
int main(int argc, char** argv)
{
    test1();
    test2();
    return 0;
}
]])

dnl This is a C test to see if 128-bit __atomic_compare_exchange_n()
dnl actually works (e.g., it compiles and links successfully on
dnl ARM64+clang, but returns incorrect answers as of August 2018).
AC_DEFUN([PRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_TEST_SOURCE],[[
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdatomic.h>
typedef union {
    uint64_t fake@<:@2@:>@;
    _Atomic __int128 real;
    __int128 real2;
} prte128;
static void test1(void)
{
    // As of Aug 2018, we could not figure out a way to assign 128-bit
    // constants -- the compilers would not accept it.  So use a fake
    // union to assign 2 uin64_t's to make a single __int128.
    prte128 ptr      = { .fake = { 0xFFEEDDCCBBAA0099, 0x8877665544332211 }};
    prte128 expected = { .fake = { 0x11EEDDCCBBAA0099, 0x88776655443322FF }};
    prte128 desired  = { .fake = { 0x1122DDCCBBAA0099, 0x887766554433EEFF }};
    bool r = atomic_compare_exchange_strong (&ptr.real, &expected.real2,
                                             desired.real);
    if ( !(r == false && ptr.real == expected.real)) {
        exit(1);
    }
}
static void test2(void)
{
    prte128 ptr =      { .fake = { 0xFFEEDDCCBBAA0099, 0x8877665544332211 }};
    prte128 expected = ptr;
    prte128 desired =  { .fake = { 0x1122DDCCBBAA0099, 0x887766554433EEFF }};
    bool r = atomic_compare_exchange_strong (&ptr.real, &expected.real2,
                                             desired.real);
    if (!(r == true && ptr.real == desired.real)) {
        exit(2);
    }
}
int main(int argc, char** argv)
{
    test1();
    test2();
    return 0;
}
]])

dnl ------------------------------------------------------------------

dnl
dnl Check to see if a specific function is linkable.
dnl
dnl Check with:
dnl 1. No compiler/linker flags.
dnl 2. CFLAGS += -mcx16
dnl 3. LIBS += -latomic
dnl 4. Finally, if it links ok with any of #1, #2, or #3, actually try
dnl to run the test code (if we're not cross-compiling) and verify
dnl that it actually gives us the correct result.
dnl
dnl Note that we unfortunately can't use AC SEARCH_LIBS because its
dnl check incorrectly fails (because these functions are special compiler
dnl intrinsics -- SEARCH_LIBS tries with "check FUNC()", which the
dnl compiler complains doesn't match the internal prototype).  So we have
dnl to use our own LINK_IFELSE tests.  Indeed, since these functions are
dnl so special, we actually need a valid source code that calls the
dnl functions with correct arguments, etc.  It's not enough, for example,
dnl to do the usual "try to set a function pointer to the symbol" trick to
dnl determine if these functions are available, because the compiler may
dnl not implement these as actual symbols.  So just try to link a real
dnl test code.
dnl
dnl $1: function name to print
dnl $2: program to test
dnl $3: action if any of 1, 2, or 3 succeeds
dnl #4: action if all of 1, 2, and 3 fail
dnl
AC_DEFUN([PRTE_ASM_CHECK_ATOMIC_FUNC],[
    PRTE_VAR_SCOPE_PUSH([prte_asm_check_func_happy prte_asm_check_func_CFLAGS_save prte_asm_check_func_LIBS_save])
    prte_asm_check_func_CFLAGS_save=$CFLAGS
    prte_asm_check_func_LIBS_save=$LIBS
    dnl Check with no compiler/linker flags
    AC_MSG_CHECKING([for $1])
    AC_LINK_IFELSE([$2],
        [prte_asm_check_func_happy=1
         AC_MSG_RESULT([yes])],
        [prte_asm_check_func_happy=0
         AC_MSG_RESULT([no])])
    dnl If that didn't work, try again with CFLAGS+=mcx16
    AS_IF([test $prte_asm_check_func_happy -eq 0],
        [AC_MSG_CHECKING([for $1 with -mcx16])
         CFLAGS="$CFLAGS -mcx16"
         AC_LINK_IFELSE([$2],
             [prte_asm_check_func_happy=1
              AC_MSG_RESULT([yes])],
             [prte_asm_check_func_happy=0
              CFLAGS=$prte_asm_check_func_CFLAGS_save
              AC_MSG_RESULT([no])])
         ])
    dnl If that didn't work, try again with LIBS+=-latomic
    AS_IF([test $prte_asm_check_func_happy -eq 0],
        [AC_MSG_CHECKING([for $1 with -latomic])
         LIBS="$LIBS -latomic"
         AC_LINK_IFELSE([$2],
             [prte_asm_check_func_happy=1
              AC_MSG_RESULT([yes])],
             [prte_asm_check_func_happy=0
              LIBS=$prte_asm_check_func_LIBS_save
              AC_MSG_RESULT([no])])
         ])
    dnl If we have it, try it and make sure it gives a correct result.
    dnl As of Aug 2018, we know that it links but does *not* work on clang
    dnl 6 on ARM64.
    AS_IF([test $prte_asm_check_func_happy -eq 1],
        [AC_MSG_CHECKING([if $1() gives correct results])
         AC_RUN_IFELSE([$2],
              [AC_MSG_RESULT([yes])],
              [prte_asm_check_func_happy=0
               AC_MSG_RESULT([no])],
              [AC_MSG_RESULT([cannot test -- assume yes (cross compiling)])])
         ])
    dnl If we were unsuccessful, restore CFLAGS/LIBS
    AS_IF([test $prte_asm_check_func_happy -eq 0],
        [CFLAGS=$prte_asm_check_func_CFLAGS_save
         LIBS=$prte_asm_check_func_LIBS_save])
    dnl Run the user actions
    AS_IF([test $prte_asm_check_func_happy -eq 1], [$3], [$4])
    PRTE_VAR_SCOPE_POP
])

dnl ------------------------------------------------------------------

AC_DEFUN([PRTE_CHECK_SYNC_BUILTIN_CSWAP_INT128], [
  PRTE_VAR_SCOPE_PUSH([sync_bool_compare_and_swap_128_result])
  # Do we have __sync_bool_compare_and_swap?
  # Use a special macro because we need to check with a few different
  # CFLAGS/LIBS.
  PRTE_ASM_CHECK_ATOMIC_FUNC([__sync_bool_compare_and_swap],
      [AC_LANG_SOURCE(PRTE_SYNC_BOOL_COMPARE_AND_SWAP_TEST_SOURCE)],
      [sync_bool_compare_and_swap_128_result=1],
      [sync_bool_compare_and_swap_128_result=0])
  AC_DEFINE_UNQUOTED([PRTE_HAVE_SYNC_BUILTIN_CSWAP_INT128],
        [$sync_bool_compare_and_swap_128_result],
        [Whether the __sync builtin atomic compare and swap supports 128-bit values])
  PRTE_VAR_SCOPE_POP
])

AC_DEFUN([PRTE_CHECK_GCC_BUILTIN_CSWAP_INT128], [
  PRTE_VAR_SCOPE_PUSH([atomic_compare_exchange_n_128_result atomic_compare_exchange_n_128_CFLAGS_save atomic_compare_exchange_n_128_LIBS_save])
  atomic_compare_exchange_n_128_CFLAGS_save=$CFLAGS
  atomic_compare_exchange_n_128_LIBS_save=$LIBS
  # Do we have __sync_bool_compare_and_swap?
  # Use a special macro because we need to check with a few different
  # CFLAGS/LIBS.
  PRTE_ASM_CHECK_ATOMIC_FUNC([__atomic_compare_exchange_n],
      [AC_LANG_SOURCE(PRTE_ATOMIC_COMPARE_EXCHANGE_N_TEST_SOURCE)],
      [atomic_compare_exchange_n_128_result=1],
      [atomic_compare_exchange_n_128_result=0])
  # If we have it and it works, check to make sure it is always lock
  # free.
  AS_IF([test $atomic_compare_exchange_n_128_result -eq 1],
        [AC_MSG_CHECKING([if __int128 atomic compare-and-swap is always lock-free])
         AC_RUN_IFELSE([AC_LANG_PROGRAM([], [if (!__atomic_always_lock_free(16, 0)) { return 1; }])],
              [AC_MSG_RESULT([yes])],
              [atomic_compare_exchange_n_128_result=0
               # If this test fails, need to reset CFLAGS/LIBS (the
               # above tests atomically set CFLAGS/LIBS or not; this
               # test is running after the fact, so we have to undo
               # the side-effects of setting CFLAGS/LIBS if the above
               # tests passed).
               CFLAGS=$atomic_compare_exchange_n_128_CFLAGS_save
               LIBS=$atomic_compare_exchange_n_128_LIBS_save
               AC_MSG_RESULT([no])],
              [AC_MSG_RESULT([cannot test -- assume yes (cross compiling)])])
        ])
  AC_DEFINE_UNQUOTED([PRTE_HAVE_GCC_BUILTIN_CSWAP_INT128],
        [$atomic_compare_exchange_n_128_result],
        [Whether the __atomic builtin atomic compare swap is both supported and lock-free on 128-bit values])
  dnl If we could not find decent support for 128-bits __atomic let's
  dnl try the GCC _sync
  AS_IF([test $atomic_compare_exchange_n_128_result -eq 0],
      [PRTE_CHECK_SYNC_BUILTIN_CSWAP_INT128])
  PRTE_VAR_SCOPE_POP
])

AC_DEFUN([PRTE_CHECK_GCC_ATOMIC_BUILTINS], [
  if test -z "$prte_cv_have___atomic" ; then
    AC_MSG_CHECKING([for 32-bit GCC built-in atomics])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <stdint.h>
uint32_t tmp, old = 0;
uint64_t tmp64, old64 = 0;
__atomic_thread_fence(__ATOMIC_SEQ_CST);
__atomic_compare_exchange_n(&tmp, &old, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
__atomic_add_fetch(&tmp, 1, __ATOMIC_RELAXED);
__atomic_compare_exchange_n(&tmp64, &old64, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
__atomic_add_fetch(&tmp64, 1, __ATOMIC_RELAXED);]],
        [prte_cv_have___atomic=yes],
        [prte_cv_have___atomic=no])])
    AC_MSG_RESULT([$prte_cv_have___atomic])
    if test "$prte_cv_have___atomic" = "yes" ; then
    AC_MSG_CHECKING([for 64-bit GCC built-in atomics])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <stdint.h>
uint64_t tmp64, old64 = 0;
__atomic_compare_exchange_n(&tmp64, &old64, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
__atomic_add_fetch(&tmp64, 1, __ATOMIC_RELAXED);]],
            [prte_cv_have___atomic_64=yes],
            [prte_cv_have___atomic_64=no])])
    AC_MSG_RESULT([$prte_cv_have___atomic_64])
    if test "$prte_cv_have___atomic_64" = "yes" ; then
        AC_MSG_CHECKING([if 64-bit GCC built-in atomics are lock-free])
        AC_RUN_IFELSE([AC_LANG_PROGRAM([], [if (!__atomic_is_lock_free (8, 0)) { return 1; }])],
              [AC_MSG_RESULT([yes])],
              [AC_MSG_RESULT([no])
               prte_cv_have___atomic_64=no],
              [AC_MSG_RESULT([cannot test -- assume yes (cross compiling)])])
    fi
    else
    prte_cv_have___atomic_64=no
    fi
    # Check for 128-bit support
    PRTE_CHECK_GCC_BUILTIN_CSWAP_INT128
  fi
])

AC_DEFUN([PRTE_CHECK_C11_CSWAP_INT128], [
  PRTE_VAR_SCOPE_PUSH([atomic_compare_exchange_result atomic_compare_exchange_CFLAGS_save atomic_compare_exchange_LIBS_save])
  atomic_compare_exchange_CFLAGS_save=$CFLAGS
  atomic_compare_exchange_LIBS_save=$LIBS
  # Do we have C11 atomics on 128-bit integers?
  # Use a special macro because we need to check with a few different
  # CFLAGS/LIBS.
  PRTE_ASM_CHECK_ATOMIC_FUNC([atomic_compare_exchange_strong_16],
      [AC_LANG_SOURCE(PRTE_ATOMIC_COMPARE_EXCHANGE_STRONG_TEST_SOURCE)],
      [atomic_compare_exchange_result=1],
      [atomic_compare_exchange_result=0])
  # If we have it and it works, check to make sure it is always lock
  # free.
  AS_IF([test $atomic_compare_exchange_result -eq 1],
        [AC_MSG_CHECKING([if C11 __int128 atomic compare-and-swap is always lock-free])
         AC_RUN_IFELSE([AC_LANG_PROGRAM([#include <stdatomic.h>], [_Atomic __int128_t x; if (!atomic_is_lock_free(&x)) { return 1; }])],
              [AC_MSG_RESULT([yes])],
              [atomic_compare_exchange_result=0
               # If this test fails, need to reset CFLAGS/LIBS (the
               # above tests atomically set CFLAGS/LIBS or not; this
               # test is running after the fact, so we have to undo
               # the side-effects of setting CFLAGS/LIBS if the above
               # tests passed).
               CFLAGS=$atomic_compare_exchange_CFLAGS_save
               LIBS=$atomic_compare_exchange_LIBS_save
               AC_MSG_RESULT([no])],
              [AC_MSG_RESULT([cannot test -- assume yes (cross compiling)])])
        ])
  AC_DEFINE_UNQUOTED([PRTE_HAVE_C11_CSWAP_INT128],
        [$atomic_compare_exchange_result],
        [Whether C11 atomic compare swap is both supported and lock-free on 128-bit values])
  dnl If we could not find decent support for 128-bits atomic let's
  dnl try the GCC _sync
  AS_IF([test $atomic_compare_exchange_result -eq 0],
      [PRTE_CHECK_SYNC_BUILTIN_CSWAP_INT128])
  PRTE_VAR_SCOPE_POP
])

dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_TEXT
dnl
dnl Determine how to set current mode as text.
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_TEXT],[
    AC_MSG_CHECKING([directive for setting text section])
    prte_cv_asm_text=""
    if test "$prte_cv_c_compiler_vendor" = "microsoft" ; then
        # text section will be brought in with the rest of
        # header for MS - leave blank for now
        prte_cv_asm_text=""
    else
        case $host in
            *-aix*)
                prte_cv_asm_text=[".csect .text[PR]"]
            ;;
            *)
                prte_cv_asm_text=".text"
            ;;
        esac
    fi
    AC_MSG_RESULT([$prte_cv_asm_text])
    AC_DEFINE_UNQUOTED([PRTE_ASM_TEXT], ["$prte_cv_asm_text"],
                       [Assembly directive for setting text section])
    PRTE_ASM_TEXT="$prte_cv_asm_text"
    AC_SUBST(PRTE_ASM_TEXT)
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_GLOBAL
dnl
dnl Sets PRTE_ASM_GLOBAL to the value to prefix global values
dnl
dnl I'm sure if I don't have a test for this, there will be some
dnl dumb platform that uses something else
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_GLOBAL],[
    AC_MSG_CHECKING([directive for exporting symbols])
    prte_cv_asm_global=""
    if test "$prte_cv_c_compiler_vendor" = "microsoft" ; then
        prte_cv_asm_global="PUBLIC"
    else
        case $host in
            *)
                prte_cv_asm_global=".globl"
            ;;
        esac
    fi
    AC_MSG_RESULT([$prte_cv_asm_global])
    AC_DEFINE_UNQUOTED([PRTE_ASM_GLOBAL], ["$prte_cv_asm_global"],
                       [Assembly directive for exporting symbols])
    PRTE_ASM_GLOBAL="$prte_cv_asm_global"
    AC_SUBST(PRTE_AS_GLOBAL)
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_LSYM
dnl
dnl Sets PRTE_ASM_LSYM to the prefix value on a symbol to make it
dnl an internal label (jump target and whatnot)
dnl
dnl We look for L .L $ L$ (in that order) for something that both
dnl assembles and does not leave a label in the output of nm.  Fall
dnl back to L if nothing else seems to work :/
dnl
dnl #################################################################

# _PRTE_CHECK_ASM_LSYM([variable-to-set])
# ---------------------------------------
AC_DEFUN([_PRTE_CHECK_ASM_LSYM],[
    AC_REQUIRE([AC_PROG_GREP])
    $1="L"
    for sym in L .L $ L$ ; do
        asm_result=0
        echo "configure: trying $sym" >&AS_MESSAGE_LOG_FD
        PRTE_TRY_ASSEMBLE([foobar$prte_cv_asm_label_suffix
${sym}mytestlabel$prte_cv_asm_label_suffix],
            [# ok, we succeeded at assembling.  see if we can nm,
             # throwing the results in a file
            if $NM conftest.$OBJEXT > conftest.out 2>&AS_MESSAGE_LOG_FD ; then
                if test "`$GREP mytestlabel conftest.out`" = "" ; then
                    # there was no symbol...  looks promising to me
                    $1="$sym"
                    asm_result=1
                elif test ["`$GREP ' [Nt] .*mytestlabel' conftest.out`"] = "" ; then
                    # see if we have a non-global-ish symbol
                    # but we should see if we can do better.
                    $1="$sym"
                fi
            else
                # not so much on the NM goodness :/
                echo "$NM failed.  Output from NM was:" >&AS_MESSAGE_LOG_FD
                cat conftest.out >&AS_MESSAGE_LOG_FD
                AC_MSG_WARN([$NM could not read object file])
            fi
            ])
        if test "$asm_result" = "1" ; then
            break
        fi
    done
    rm -f conftest.out
    unset asm_result sym
])

# PRTE_CHECK_ASM_LSYM()
# ---------------------
AC_DEFUN([PRTE_CHECK_ASM_LSYM],[
    AC_CACHE_CHECK([prefix for lsym labels],
                   [prte_cv_asm_lsym],
                   [_PRTE_CHECK_ASM_LSYM([prte_cv_asm_lsym])])
    AC_DEFINE_UNQUOTED([PRTE_ASM_LSYM], ["$prte_cv_asm_lsym"],
                       [Assembly prefix for lsym labels])
    PRTE_ASM_LSYM="$prte_cv_asm_lsym"
    AC_SUBST(PRTE_ASM_LSYM)
])dnl

dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_PROC
dnl
dnl Sets a cv-flag, if the compiler needs a proc/endp-definition to
dnl link with C.
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_PROC],[
    AC_CACHE_CHECK([if .proc/endp is needed],
                   [prte_cv_asm_need_proc],
                   [prte_cv_asm_need_proc="no"
                    PRTE_TRY_ASSEMBLE([
     .proc mysym
mysym:
     .endp mysym],
                          [prte_cv_asm_need_proc="yes"])
                    rm -f conftest.out])
    if test "$prte_cv_asm_need_proc" = "yes" ; then
       prte_cv_asm_proc=".proc"
       prte_cv_asm_endproc=".endp"
    else
       prte_cv_asm_proc="#"
       prte_cv_asm_endproc="#"
    fi
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_GSYM
dnl
dnl Sets PRTE_ASM_GSYM to the prefix value on a symbol to make it
dnl a global linkable from C.  Basically, an _ or not.
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_GSYM],[
    AC_CACHE_CHECK([prefix for global symbol labels],
                   [prte_cv_asm_gsym],
                   [_PRTE_CHECK_ASM_GSYM])
    if test "$prte_cv_asm_gsym" = "none" ; then
       AC_MSG_ERROR([Could not determine global symbol label prefix])
    fi
    AC_DEFINE_UNQUOTED([PRTE_ASM_GSYM], ["$prte_cv_asm_gsym"],
                       [Assembly prefix for gsym labels])
    PRTE_ASM_GSYM="$prte_cv_asm_gsym"
    AC_SUBST(PRTE_ASM_GSYM)
])

AC_DEFUN([_PRTE_CHECK_ASM_GSYM],[
    prte_cv_asm_gsym="none"
    for sym in "_" "" "." ; do
        asm_result=0
        echo "configure: trying $sym" >&AS_MESSAGE_LOG_FD
cat > conftest_c.c <<EOF
#ifdef __cplusplus
extern "C" {
#endif
void gsym_test_func(void);
#ifdef __cplusplus
}
#endif
int
main()
{
    gsym_test_func();
    return 0;
}
EOF
        PRTE_TRY_ASSEMBLE([
$prte_cv_asm_text
$prte_cv_asm_proc ${sym}gsym_test_func
$prte_cv_asm_global ${sym}gsym_test_func
${sym}gsym_test_func${prte_cv_asm_label_suffix}
$prte_cv_asm_endproc ${sym}gsym_test_func
            ],
            [prte_compile="$CC $CFLAGS -I. conftest_c.c -c > conftest.cmpl 2>&1"
             if AC_TRY_EVAL(prte_compile) ; then
                # save the warnings
                 cat conftest.cmpl >&AS_MESSAGE_LOG_FD
                 prte_link="$CC $CFLAGS conftest_c.$OBJEXT conftest.$OBJEXT -o conftest  $LDFLAGS $LIBS > conftest.link 2>&1"
                 if AC_TRY_EVAL(prte_link) ; then
                     # save the warnings
                     cat conftest.link >&AS_MESSAGE_LOG_FD
                     asm_result=1
                 else
                     cat conftest.link >&AS_MESSAGE_LOG_FD
                     echo "configure: failed C program was: " >&AS_MESSAGE_LOG_FD
                     cat conftest_c.c >&AS_MESSAGE_LOG_FD
                     echo "configure: failed ASM program was: " >&AS_MESSAGE_LOG_FD
                     cat conftest.s >&AS_MESSAGE_LOG_FD
                     asm_result=0
                 fi
             else
                # save output and failed program
                 cat conftest.cmpl >&AS_MESSAGE_LOG_FD
                 echo "configure: failed C program was: " >&AS_MESSAGE_LOG_FD
                 cat conftest.c >&AS_MESSAGE_LOG_FD
                 asm_result=0
             fi],
            [asm_result=0])
        if test "$asm_result" = "1" ; then
            prte_cv_asm_gsym="$sym"
            break
        fi
    done
    rm -rf conftest.*
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_LABEL_SUFFIX
dnl
dnl Sets PRTE_ASM_LABEL_SUFFIX to the value to suffix for labels
dnl
dnl I'm sure if I don't have a test for this, there will be some
dnl dumb platform that uses something else
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_LABEL_SUFFIX],[
    AC_MSG_CHECKING([suffix for labels])
    prte_cv_asm_label_suffix=""
    case $host in
        *)
                prte_cv_asm_label_suffix=":"
        ;;
    esac
    AC_MSG_RESULT([$prte_cv_asm_label_suffix])
    AC_DEFINE_UNQUOTED([PRTE_ASM_LABEL_SUFFIX], ["$prte_cv_asm_label_suffix"],
                       [Assembly suffix for labels])
    PRTE_ASM_LABEL_SUFFIX="$prte_cv_asm_label_suffix"
    AC_SUBST(PRTE_AS_LABEL_SUFFIX)
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_ALIGN_LOG
dnl
dnl Sets PRTE_ASM_ALIGN_LOG to 1 if align is specified
dnl logarithmically, 0 otherwise
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_ALIGN_LOG],[
    AC_REQUIRE([AC_PROG_GREP])
    AC_CACHE_CHECK([if .align directive takes logarithmic value],
                   [prte_cv_asm_align_log],
                   [ PRTE_TRY_ASSEMBLE([        $prte_cv_asm_text
        .align 4
        $prte_cv_asm_global foo
        .byte 1
        .align 4
foo$prte_cv_asm_label_suffix
        .byte 2],
        [prte_asm_addr=[`$NM conftest.$OBJEXT | $GREP foo | sed -e 's/.*\([0-9a-fA-F][0-9a-fA-F]\).*foo.*/\1/'`]],
        [prte_asm_addr=""])
    # test for both 16 and 10 (decimal and hex notations)
    echo "configure: .align test address offset is $prte_asm_addr" >&AS_MESSAGE_LOG_FD
    if test "$prte_asm_addr" = "16" || test "$prte_asm_addr" = "10" ; then
       prte_cv_asm_align_log="yes"
    else
        prte_cv_asm_align_log="no"
    fi])
    if test "$prte_cv_asm_align_log" = "yes" || test "$prte_cv_asm_align_log" = "1" ; then
        prte_asm_align_log_result=1
    else
        prte_asm_align_log_result=0
    fi
    AC_DEFINE_UNQUOTED([PRTE_ASM_ALIGN_LOG],
                       [$asm_align_log_result],
                       [Assembly align directive expects logarithmic value])
    unset omp_asm_addr asm_result
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_TYPE
dnl
dnl Sets PRTE_ASM_TYPE to the prefix for the function type to
dnl set a symbol's type as function (needed on ELF for shared
dnl libraries).  If no .type directive is needed, sets PRTE_ASM_TYPE
dnl to an empty string
dnl
dnl We look for @ \# %
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_TYPE],[
        AC_CACHE_CHECK([prefix for function in .type],
                       [prte_cv_asm_type],
                       [_PRTE_CHECK_ASM_TYPE])
    AC_DEFINE_UNQUOTED([PRTE_ASM_TYPE], ["$prte_cv_asm_type"],
                       [How to set function type in .type directive])
    PRTE_ASM_TYPE="$prte_cv_asm_type"
    AC_SUBST(PRTE_ASM_TYPE)
])

AC_DEFUN([_PRTE_CHECK_ASM_TYPE],[
    prte_cv_asm_type=""
    case "${host}" in
    *-sun-solaris*)
        # GCC on solaris seems to accept just about anything, not
        # that what it defines actually works...  So just hardwire
        # to the right answer
        prte_cv_asm_type="#"
    ;;
    *)
        for type  in @ \# % ; do
            asm_result=0
            echo "configure: trying $type" >&AS_MESSAGE_LOG_FD
            PRTE_TRY_ASSEMBLE([     .type mysym, ${type}function
mysym:],
                 [prte_cv_asm_type="${type}"
                    asm_result=1])
            if test "$asm_result" = "1" ; then
                break
            fi
        done
    ;;
    esac
    rm -f conftest.out
    unset asm_result type
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_ASM_SIZE
dnl
dnl Sets PRTE_ASM_SIZE to 1 if we should set .size directives for
dnl each function, 0 otherwise.
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_ASM_SIZE],[
    AC_CACHE_CHECK([if .size is needed],
                   [prte_cv_asm_need_size],
                   [prte_cv_asm_need_size="no"
                    PRTE_TRY_ASSEMBLE([     .size mysym, 1],
                          [prte_cv_asm_need_size="yes"])
                    rm -f conftest.out])
    if test "$prte_cv_asm_need_size" = "yes" ; then
       prte_asm_size=1
    else
       prte_asm_size=0
    fi
    AC_DEFINE_UNQUOTED([PRTE_ASM_SIZE], ["$prte_asm_size"],
                       [Do we need to give a .size directive])
    PRTE_ASM_SIZE="$prte_asm_size"
    AC_SUBST(PRTE_ASM_TYPE)
    unset asm_result
])dnl


# PRTE_CHECK_ASM_GNU_STACKEXEC(var)
# ----------------------------------
# sets shell variable var to the things necessary to
# disable execable stacks with GAS
AC_DEFUN([PRTE_CHECK_ASM_GNU_STACKEXEC], [
    AC_REQUIRE([AC_PROG_GREP])
    AC_CHECK_PROG([OBJDUMP], [objdump], [objdump])
    AC_CACHE_CHECK([if .note.GNU-stack is needed],
        [prte_cv_asm_gnu_stack_result],
        [AS_IF([test "$OBJDUMP" != ""],
            [ # first, see if a simple C program has it set
             cat >conftest.c <<EOF
int testfunc() {return 0; }
EOF
             PRTE_LOG_COMMAND([$CC $CFLAGS -c conftest.c -o conftest.$OBJEXT],
                 [$OBJDUMP -x conftest.$OBJEXT 2>&1 | $GREP '\.note\.GNU-stack' &> /dev/null && prte_cv_asm_gnu_stack_result=yes],
                 [PRTE_LOG_MSG([the failed program was:], 1)
                  PRTE_LOG_FILE([conftest.c])
                  prte_cv_asm_gnu_stack_result=no])
             if test "$prte_cv_asm_gnu_stack_result" != "yes" ; then
                 prte_cv_asm_gnu_stack_result="no"
             fi
             rm -rf conftest.*],
            [prte_cv_asm_gnu_stack_result="no"])])
    if test "$prte_cv_asm_gnu_stack_result" = "yes" ; then
        prte_cv_asm_gnu_stack=1
    else
        prte_cv_asm_gnu_stack=0
    fi
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_POWERPC_REG
dnl
dnl See if the notation for specifying registers is X (most everyone)
dnl or rX (OS X)
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_POWERPC_REG],[
    AC_MSG_CHECKING([if PowerPC registers have r prefix])
    PRTE_TRY_ASSEMBLE([$prte_cv_asm_text
        addi 1,1,0],
        [prte_cv_asm_powerpc_r_reg=0],
        [PRTE_TRY_ASSEMBLE([$prte_cv_asm_text
        addi r1,r1,0],
            [prte_cv_asm_powerpc_r_reg=1],
            [AC_MSG_ERROR([Can not determine how to use PPC registers])])])
    if test "$prte_cv_asm_powerpc_r_reg" = "1" ; then
        AC_MSG_RESULT([yes])
    else
        AC_MSG_RESULT([no])
    fi
    AC_DEFINE_UNQUOTED([PRTE_POWERPC_R_REGISTERS],
                       [$prte_cv_asm_powerpc_r_reg],
                       [Whether r notation is used for ppc registers])
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_POWERPC_64BIT
dnl
dnl On some powerpc chips (the PPC970 or G5), the OS usually runs in
dnl 32 bit mode, even though the hardware can do 64bit things.  If
dnl the compiler will let us, emit code for 64bit test and set type
dnl operations (on a long long).
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_POWERPC_64BIT],[
    if test "$ac_cv_sizeof_long" != "4" ; then
        # this function should only be called in the 32 bit case
        AC_MSG_ERROR([CHECK_POWERPC_64BIT called on 64 bit platform.  Internal error.])
    fi
    AC_MSG_CHECKING([for 64-bit PowerPC assembly support])
        case $host in
            *-darwin*)
                ppc64_result=0
                if test "$prte_cv_asm_powerpc_r_reg" = "1" ; then
                   ldarx_asm="        ldarx r1,r1,r1";
                else
                   ldarx_asm="        ldarx 1,1,1";
                fi
                PRTE_TRY_ASSEMBLE([$prte_cv_asm_text
        $ldarx_asm],
                    [ppc64_result=1],
                    [ppc64_result=0])
            ;;
            *)
                ppc64_result=0
            ;;
        esac
    if test "$ppc64_result" = "1" ; then
        AC_MSG_RESULT([yes])
        ifelse([$1],,:,[$1])
    else
        AC_MSG_RESULT([no])
        ifelse([$2],,:,[$2])
    fi
    unset ppc64_result ldarx_asm
])dnl


dnl #################################################################
dnl
dnl PRTE_CHECK_CMPXCHG16B
dnl
dnl #################################################################
AC_DEFUN([PRTE_CMPXCHG16B_TEST_SOURCE],[[
#include <stdint.h>
#include <assert.h>
union prte_counted_pointer_t {
    struct {
        uint64_t counter;
        uint64_t item;
    } data;
#if defined(HAVE___INT128) && HAVE___INT128
    __int128 value;
#elif defined(HAVE_INT128_T) && HAVE_INT128_T
    int128_t value;
#endif
};
typedef union prte_counted_pointer_t prte_counted_pointer_t;
int main(int argc, char* argv) {
    volatile prte_counted_pointer_t a;
    prte_counted_pointer_t b;
    a.data.counter = 0;
    a.data.item = 0x1234567890ABCDEF;
    b.data.counter = a.data.counter;
    b.data.item = a.data.item;
    /* bozo checks */
    assert(16 == sizeof(prte_counted_pointer_t));
    assert(a.data.counter == b.data.counter);
    assert(a.data.item == b.data.item);
    /*
     * the following test fails on buggy compilers
     * so far, with icc -o conftest conftest.c
     *  - intel icc 14.0.0.080 (aka 2013sp1)
     *  - intel icc 14.0.1.106 (aka 2013sp1u1)
     * older and more recents compilers work fine
     * buggy compilers work also fine but only with -O0
     */
#if (defined(HAVE___INT128) && HAVE___INT128) || (defined(HAVE_INT128_T) && HAVE_INT128_T)
    return (a.value != b.value);
#else
    return 0;
#endif
}
]])

AC_DEFUN([PRTE_CHECK_CMPXCHG16B],[
    PRTE_VAR_SCOPE_PUSH([cmpxchg16b_result])
    PRTE_ASM_CHECK_ATOMIC_FUNC([cmpxchg16b],
                               [AC_LANG_PROGRAM([[unsigned char tmp[16];]],
                                                [[__asm__ __volatile__ ("lock cmpxchg16b (%%rsi)" : : "S" (tmp) : "memory", "cc");]])],
                               [cmpxchg16b_result=1],
                               [cmpxchg16b_result=0])
    # If we have it, make sure it works.
    AS_IF([test $cmpxchg16b_result -eq 1],
          [AC_MSG_CHECKING([if cmpxchg16b_result works])
           AC_RUN_IFELSE([AC_LANG_SOURCE(PRTE_CMPXCHG16B_TEST_SOURCE)],
                         [AC_MSG_RESULT([yes])],
                         [cmpxchg16b_result=0
                          AC_MSG_RESULT([no])],
                         [AC_MSG_RESULT([cannot test -- assume yes (cross compiling)])])
          ])
    AC_DEFINE_UNQUOTED([PRTE_HAVE_CMPXCHG16B], [$cmpxchg16b_result],
        [Whether the processor supports the cmpxchg16b instruction])
    PRTE_VAR_SCOPE_POP
])dnl

dnl #################################################################
dnl
dnl PRTE_CHECK_INLINE_GCC
dnl
dnl Check if the compiler is capable of doing GCC-style inline
dnl assembly.  Some compilers emit a warning and ignore the inline
dnl assembly (xlc on OS X) and compile without error.  Therefore,
dnl the test attempts to run the emitted code to check that the
dnl assembly is actually run.  To run this test, one argument to
dnl the macro must be an assembly instruction in gcc format to move
dnl the value 0 into the register containing the variable ret.
dnl For PowerPC, this would be:
dnl
dnl   "li %0,0" : "=&r"(ret)
dnl
dnl For testing ia32 assembly, the assembly instruction xaddl is
dnl tested.  The xaddl instruction is used by some of the atomic
dnl implementations so it makes sense to test for it.  In addition,
dnl some compilers (i.e. earlier versions of Sun Studio 12) do not
dnl necessarily handle xaddl properly, so that needs to be detected
dnl during configure time.
dnl
dnl DEFINE PRTE_GCC_INLINE_ASSEMBLY to 0 or 1 depending on GCC
dnl                support
dnl
dnl #################################################################
AC_DEFUN([PRTE_CHECK_INLINE_C_GCC],[
    assembly="$1"
    asm_result="unknown"
    AC_MSG_CHECKING([if $CC supports GCC inline assembly])
    if test ! "$assembly" = "" ; then
        AC_RUN_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT],[[
int ret = 1;
int negone = -1;
__asm__ __volatile__ ($assembly);
return ret;
                                                             ]])],
                      [asm_result="yes"], [asm_result="no"],
                      [asm_result="unknown"])
    else
        assembly="test skipped - assuming no"
    fi
    # if we're cross compiling, just try to compile and figure good enough
    if test "$asm_result" = "unknown" ; then
        AC_LINK_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT],[[
int ret = 1;
int negone = -1;
__asm__ __volatile__ ($assembly);
return ret;
                                                              ]])],
                       [asm_result="yes"], [asm_result="no"])
    fi
    AC_MSG_RESULT([$asm_result])
    if test "$asm_result" = "yes" ; then
        PRTE_C_GCC_INLINE_ASSEMBLY=1
        prte_cv_asm_inline_supported="yes"
    else
        PRTE_C_GCC_INLINE_ASSEMBLY=0
    fi
    AC_DEFINE_UNQUOTED([PRTE_C_GCC_INLINE_ASSEMBLY],
                       [$PRTE_C_GCC_INLINE_ASSEMBLY],
                       [Whether C compiler supports GCC style inline assembly])
    unset PRTE_C_GCC_INLINE_ASSEMBLY assembly asm_result
])dnl

dnl #################################################################
dnl
dnl PRTE_CONFIG_ASM
dnl
dnl DEFINE PRTE_ASSEMBLY_ARCH to something in sys/architecture.h
dnl DEFINE PRTE_ASSEMBLY_FORMAT to string containing correct
dnl                             format for assembly (not user friendly)
dnl SUBST PRTE_ASSEMBLY_FORMAT to string containing correct
dnl                             format for assembly (not user friendly)
dnl
dnl #################################################################
AC_DEFUN([PRTE_CONFIG_ASM],[
    AC_REQUIRE([PRTE_SETUP_CC])
    AC_REQUIRE([AM_PROG_AS])
    AC_ARG_ENABLE([c11-atomics],[AS_HELP_STRING([--enable-c11-atomics],
                  [Enable use of C11 atomics if available (default: enabled)])])
    AC_ARG_ENABLE([builtin-atomics],
      [AS_HELP_STRING([--enable-builtin-atomics],
         [Enable use of GCC built-in atomics (default: autodetect)])])
    PRTE_CHECK_C11_CSWAP_INT128
    prte_cv_asm_builtin="BUILTIN_NO"
    PRTE_CHECK_GCC_ATOMIC_BUILTINS
    if test "x$enable_c11_atomics" != "xno" && test "$prte_cv_c11_supported" = "yes" ; then
        prte_cv_asm_builtin="BUILTIN_C11"
        PRTE_CHECK_C11_CSWAP_INT128
    elif test "x$enable_c11_atomics" = "xyes"; then
        AC_MSG_WARN([C11 atomics were requested but are not supported])
        AC_MSG_ERROR([Cannot continue])
    elif test "$enable_builtin_atomics" = "yes" ; then
    if test "$prte_cv_have___atomic" = "yes" ; then
       prte_cv_asm_builtin="BUILTIN_GCC"
    else
        AC_MSG_WARN([GCC built-in atomics requested but not found.])
        AC_MSG_ERROR([Cannot continue])
    fi
    fi
        PRTE_CHECK_ASM_PROC
        PRTE_CHECK_ASM_TEXT
        PRTE_CHECK_ASM_GLOBAL
        PRTE_CHECK_ASM_GNU_STACKEXEC
        PRTE_CHECK_ASM_LABEL_SUFFIX
        PRTE_CHECK_ASM_GSYM
        PRTE_CHECK_ASM_LSYM
        PRTE_CHECK_ASM_TYPE
        PRTE_CHECK_ASM_SIZE
        PRTE_CHECK_ASM_ALIGN_LOG
        # find our architecture for purposes of assembly stuff
        prte_cv_asm_arch="UNSUPPORTED"
        PRTE_GCC_INLINE_ASSIGN=""
    if test "$prte_cv_have___atomic_64" ; then
            PRTE_ASM_SUPPORT_64BIT=1
    else
        PRTE_ASM_SUPPORT_64BIT=0
    fi
        case "${host}" in
        x86_64-*x32)
            prte_cv_asm_arch="X86_64"
            PRTE_ASM_SUPPORT_64BIT=1
            PRTE_GCC_INLINE_ASSIGN='"xaddl %1,%0" : "=m"(ret), "+r"(negone) : "m"(ret)'
            ;;
        i?86-*|x86_64*|amd64*)
            if test "$ac_cv_sizeof_long" = "4" ; then
                prte_cv_asm_arch="IA32"
            else
                prte_cv_asm_arch="X86_64"
            fi
            PRTE_ASM_SUPPORT_64BIT=1
            PRTE_GCC_INLINE_ASSIGN='"xaddl %1,%0" : "=m"(ret), "+r"(negone) : "m"(ret)'
            PRTE_CHECK_CMPXCHG16B
            ;;
        aarch64*)
            prte_cv_asm_arch="ARM64"
            PRTE_ASM_SUPPORT_64BIT=1
            PRTE_ASM_ARM_VERSION=8
            PRTE_GCC_INLINE_ASSIGN='"mov %0, #0" : "=&r"(ret)'
            ;;
        armv7*|arm-*-linux-gnueabihf)
            prte_cv_asm_arch="ARM"
            PRTE_ASM_SUPPORT_64BIT=1
            PRTE_ASM_ARM_VERSION=7
            PRTE_GCC_INLINE_ASSIGN='"mov %0, #0" : "=&r"(ret)'
            ;;
        armv6*)
            prte_cv_asm_arch="ARM"
            PRTE_ASM_SUPPORT_64BIT=0
            PRTE_ASM_ARM_VERSION=6
            CCASFLAGS="$CCASFLAGS -march=armv7-a"
            PRTE_GCC_INLINE_ASSIGN='"mov %0, #0" : "=&r"(ret)'
            ;;
        powerpc-*|powerpc64-*|powerpcle-*|powerpc64le-*|rs6000-*|ppc-*)
            PRTE_CHECK_POWERPC_REG
            if test "$ac_cv_sizeof_long" = "4" ; then
                prte_cv_asm_arch="POWERPC32"
                # Note that on some platforms (Apple G5), even if we are
                # compiling in 32 bit mode (and therefore should assume
                # sizeof(long) == 4), we can use the 64 bit test and set
                # operations.
                PRTE_CHECK_POWERPC_64BIT(PRTE_ASM_SUPPORT_64BIT=1)
            elif test "$ac_cv_sizeof_long" = "8" ; then
                PRTE_ASM_SUPPORT_64BIT=1
                prte_cv_asm_arch="POWERPC64"
            else
                AC_MSG_ERROR([Could not determine PowerPC word size: $ac_cv_sizeof_long])
            fi
            PRTE_GCC_INLINE_ASSIGN='"1: li %0,0" : "=&r"(ret)'
            ;;
        *)
        if test "$prte_cv_have___atomic" = "yes" ; then
        prte_cv_asm_builtin="BUILTIN_GCC"
        else
        AC_MSG_ERROR([No atomic primitives available for $host])
        fi
        ;;
        esac
    if ! test -z "$PRTE_ASM_ARM_VERSION" ; then
        AC_DEFINE_UNQUOTED([PRTE_ASM_ARM_VERSION], [$PRTE_ASM_ARM_VERSION],
                               [What ARM assembly version to use])
    fi
    if test "$prte_cv_asm_builtin" = "BUILTIN_GCC" ; then
         AC_DEFINE([PRTE_C_GCC_INLINE_ASSEMBLY], [1],
           [Whether C compiler supports GCC style inline assembly])
    else
         AC_DEFINE_UNQUOTED([PRTE_ASM_SUPPORT_64BIT],
                [$PRTE_ASM_SUPPORT_64BIT],
                [Whether we can do 64bit assembly operations or not.  Should not be used outside of the assembly header files])
         AC_SUBST([PRTE_ASM_SUPPORT_64BIT])
         prte_cv_asm_inline_supported="no"
         # now that we know our architecture, try to inline assemble
         PRTE_CHECK_INLINE_C_GCC([$PRTE_GCC_INLINE_ASSIGN])
         # format:
         #   config_file-text-global-label_suffix-gsym-lsym-type-size-align_log-ppc_r_reg-64_bit-gnu_stack
         asm_format="default"
         asm_format="${asm_format}-${prte_cv_asm_text}-${prte_cv_asm_global}"
         asm_format="${asm_format}-${prte_cv_asm_label_suffix}-${prte_cv_asm_gsym}"
         asm_format="${asm_format}-${prte_cv_asm_lsym}"
         asm_format="${asm_format}-${prte_cv_asm_type}-${prte_asm_size}"
         asm_format="${asm_format}-${prte_asm_align_log_result}"
         if test "$prte_cv_asm_arch" = "POWERPC32" || test "$prte_cv_asm_arch" = "POWERPC64" ; then
             asm_format="${asm_format}-${prte_cv_asm_powerpc_r_reg}"
         else
             asm_format="${asm_format}-1"
         fi
         asm_format="${asm_format}-${PRTE_ASM_SUPPORT_64BIT}"
         prte_cv_asm_format="${asm_format}-${prte_cv_asm_gnu_stack}"
         # For the Makefile, need to escape the $ as $$.  Don't display
         # this version, but make sure the Makefile gives the right thing
         # when regenerating the files because the base has been touched.
         PRTE_ASSEMBLY_FORMAT=`echo "$prte_cv_asm_format" | sed -e 's/\\\$/\\\$\\\$/'`
        AC_MSG_CHECKING([for assembly format])
        AC_MSG_RESULT([$prte_cv_asm_format])
        AC_DEFINE_UNQUOTED([PRTE_ASSEMBLY_FORMAT], ["$PRTE_ASSEMBLY_FORMAT"],
                           [Format of assembly file])
        AC_SUBST([PRTE_ASSEMBLY_FORMAT])
      fi # if prte_cv_asm_builtin = BUILTIN_GCC
    result="PRTE_$prte_cv_asm_arch"
    PRTE_ASSEMBLY_ARCH="$prte_cv_asm_arch"
    AC_MSG_CHECKING([for assembly architecture])
    AC_MSG_RESULT([$prte_cv_asm_arch])
    AC_DEFINE_UNQUOTED([PRTE_ASSEMBLY_ARCH], [$result],
        [Architecture type of assembly to use for atomic operations and CMA])
    AC_SUBST([PRTE_ASSEMBLY_ARCH])
    # Check for RDTSCP support
    result=0
    AS_IF([test "$prte_cv_asm_arch" = "PRTE_X86_64" || test "$prte_cv_asm_arch" = "PRTE_IA32"],
          [AC_MSG_CHECKING([for RDTSCP assembly support])
           AC_LANG_PUSH([C])
           AC_RUN_IFELSE([AC_LANG_SOURCE([[
int main(int argc, char* argv[])
{
  unsigned int rax, rdx;
  __asm__ __volatile__ ("rdtscp\n": "=a" (rax), "=d" (rdx):: "%rax", "%rdx");
  return 0;
}
           ]])],
           [result=1
            AC_MSG_RESULT([yes])],
           [AC_MSG_RESULT([no])],
           [#cross compile not supported
            AC_MSG_RESULT(["no (cross compiling)"])])
           AC_LANG_POP([C])])
    AC_DEFINE_UNQUOTED([PRTE_ASSEMBLY_SUPPORTS_RDTSCP], [$result],
                       [Whether we have support for RDTSCP instruction])
    result="PRTE_$prte_cv_asm_builtin"
    PRTE_ASSEMBLY_BUILTIN="$prte_cv_asm_builtin"
    AC_MSG_CHECKING([for builtin atomics])
    AC_MSG_RESULT([$prte_cv_asm_builtin])
    AC_DEFINE_UNQUOTED([PRTE_ASSEMBLY_BUILTIN], [$result],
        [Whether to use builtin atomics])
    AC_SUBST([PRTE_ASSEMBLY_BUILTIN])
    PRTE_ASM_FIND_FILE
    unset result asm_format
])dnl


dnl #################################################################
dnl
dnl PRTE_ASM_FIND_FILE
dnl
dnl
dnl do all the evil mojo to provide a working assembly file
dnl
dnl #################################################################
AC_DEFUN([PRTE_ASM_FIND_FILE], [
    AC_REQUIRE([AC_PROG_GREP])
    AC_REQUIRE([AC_PROG_FGREP])
if test "$prte_cv_asm_arch" != "WINDOWS" && test "$prte_cv_asm_builtin" != "BUILTIN_GCC" && test "$prte_cv_asm_builtin" != "BUILTIN_OSX"  && test "$prte_cv_asm_inline_arch" = "no" ; then
    AC_MSG_ERROR([no atomic support available. exiting])
else
    # On windows with VC++, atomics are done with compiler primitives
    prte_cv_asm_file=""
fi
])dnl
