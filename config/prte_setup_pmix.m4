# -*- shell-script ; indent-tabs-mode:nil -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2011-2014 Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2014-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2016      IBM Corporation.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([PRTE_CHECK_PMIX],[

    PRTE_VAR_SCOPE_PUSH([prte_external_pmix_save_CPPFLAGS prte_external_pmix_save_LDFLAGS prte_external_pmix_save_LIBS prte_external_pmix_version_found prte_external_pmix_version])

    AC_ARG_WITH([pmix],
                [AS_HELP_STRING([--with-pmix(=DIR)],
                                [Where to find PMIx support, optionally adding DIR to the search path])])

    AC_ARG_WITH([pmix-libdir],
                [AS_HELP_STRING([--with-pmix-libdir=DIR],
                                [Look for libpmix in the given directory DIR, DIR/lib or DIR/lib64])])

    AC_ARG_ENABLE([pmix-devel-support],
                  [AS_HELP_STRING([--enable-pmix-devel-support],
                                  [Add necessary wrapper flags to enable access to PMIx devel headers])])

    prte_pmix_support=0

    if test "$with_pmix" = "no"; then
        AC_MSG_WARN([PRTE requires PMIx support using])
        AC_MSG_WARN([an external copy that you supply.])
        AC_MSG_ERROR([Cannot continue])
    fi

    # get rid of any trailing slash(es)
    pmix_prefix=$(echo $with_pmix | sed -e 'sX/*$XXg')
    pmixdir_prefix=$(echo $with_pmix_libdir | sed -e 'sX/*$XXg')

    prte_pmix_header_given=0
    PRTE_PMIX_HEADER="<pmix.h>"
    # check for external pmix header */
    AS_IF([test ! -z "$with_pmix" && test "$with_pmix" != "yes"],
                 [pmix_ext_install_dir="$with_pmix"],
                 [pmix_ext_install_dir=""])
    _PRTE_CHECK_PACKAGE_HEADER([prte_pmix], [pmix.h], [$pmix_ext_install_dir],
                               [prte_pmix_support=1],
                               [prte_pmix_support=0])

    if test $prte_pmix_support -eq 0 && test -z "$pmix_ext_install_dir"; then
        # try default locations
        if test -d /usr/include; then
            pmix_ext_install_dir=/usr
            _PRTE_CHECK_PACKAGE_HEADER([prte_pmix], [pmix.h], [$pmix_ext_install_dir],
                                       [prte_pmix_support=1],
                                       [prte_pmix_support=0])
        fi
        if test $prte_pmix_support -eq 0 && test -d /usr/local/include; then
            pmix_ext_install_dir=/usr/local
            _PRTE_CHECK_PACKAGE_HEADER([prte_pmix], [pmix.h], [$pmix_ext_install_dir],
                                       [prte_pmix_support=1],
                                       [prte_pmix_support=0])
        fi
    fi

    if test $prte_pmix_support -eq 0; then
        AC_MSG_WARN([PRTE requires PMIx support using])
        AC_MSG_WARN([an external copy that you supply.])
        AC_MSG_WARN([A copy could not be found in $pmix_ext_install_dir.])
        AC_MSG_ERROR([Cannot continue])
    fi

    # check for external pmix library - the lib search already includes
    # default locations, so no need to separately check there
    AS_IF([test ! -z "$with_pmix_libdir" && test "$with_pmix_libdir" != "yes"],
                 [pmix_ext_install_libdir="$with_pmix_libdir"],
                 [AS_IF([test ! -z "$with_pmix" && test "$with_pmix" != "yes"],
                        [pmix_ext_install_libdir="$with_pmix"/lib],
                        [pmix_ext_install_libdir=""])])

    _PRTE_CHECK_PACKAGE_LIB([prte_pmix], [pmix], [PMIx_Init],
                            [], [$pmix_ext_install_dir],
                            [$pmix_ext_install_libdir],
                            [],
                            [AC_MSG_WARN([PRTE requires PMIx support using])
                             AC_MSG_WARN([an external copy that you supply.])
                             AC_MSG_WARN([The library was not found in $pmix_ext_install_libdir.])
                             AC_MSG_ERROR([Cannot continue])])

    prte_external_pmix_save_CPPFLAGS=$CPPFLAGS
    prte_external_pmix_save_LDFLAGS=$LDFLAGS
    prte_external_pmix_save_LIBS=$LIBS

    # need to add resulting flags to global ones so we can
    # test the version
    if test ! -z "$prte_pmix_CPPFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $prte_pmix_CPPFLAGS)
    fi
    if test ! -z "$prte_pmix_LDFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $prte_pmix_LDFLAGS)
    fi
    if test ! -z "$prte_pmix_LIBS"; then
        PRTE_FLAGS_APPEND_UNIQ(LIBS, $prte_pmix_LIBS)
    fi

    # if the pmix_version.h file does not exist, then
    # this must be from a pre-1.1.5 version
    _PRTE_CHECK_PACKAGE_HEADER([prte_pmix], [pmix_version.h], [$pmix_ext_install_dir],
                               [],
                               [AC_MSG_WARN([PRTE does not support PMIx versions])
                                AC_MSG_WARN([less than v4.1 as only PMIx-based tools])
                                AC_MSG_WARN([can connect to the server.])
                                AC_MSG_ERROR([Please select a newer version and configure again])])


    # if it does exist, then we need to parse it to find
    # the actual release series
    AC_MSG_CHECKING([version 4x])
    AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                        #include <pmix_version.h>
                                        #if (PMIX_VERSION_MAJOR < 4L)
                                        #error "not version 4 or above"
                                        #endif
                                       ], [])],
                      [AC_MSG_RESULT([found])
                       prte_external_pmix_version=4x
                       prte_external_pmix_version_found=4],
                      [AC_MSG_RESULT([not found])])

    AS_IF([test "$prte_external_pmix_version_found" = "4"],
          [AC_MSG_CHECKING([version 4.1 or greater])
            AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                                #include <pmix_version.h>
                                                #if (PMIX_VERSION_MAJOR == 4L && PMIX_VERSION_MINOR < 1L)
                                                #error "not version 4.1 or above"
                                                #endif
                                               ], [])],
                              [AC_MSG_RESULT([found])],
                              [AC_MSG_RESULT([not found])
                               prte_external_pmix_version_found=0])])

    # restore the global flags
    CPPFLAGS=$prte_external_pmix_save_CPPFLAGS
    LDFLAGS=$prte_external_pmix_save_LDFLAGS
    LIBS=$prte_external_pmix_save_LIBS

    AS_IF([test "$prte_external_pmix_version_found" = "0"],
          [AC_MSG_WARN([PRTE does not support PMIx versions])
           AC_MSG_WARN([less than v4.1 as only PMIx-based tools can])
           AC_MSG_WARN([can connect to the server.])
           AC_MSG_ERROR([Please select a newer version and configure again])])

    AS_IF([test "x$prte_external_pmix_version" = "x"],
          [AC_MSG_WARN([PMIx version information could not])
           AC_MSG_WARN([be detected])
           AC_MSG_ERROR([cannot continue])])

    if test ! -z "$prte_pmix_CPPFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_pmix_CPPFLAGS)
        PRTE_WRAPPER_FLAGS_ADD(CPPFLAGS, $prte_pmix_CPPFLAGS)
    fi
    if test ! -z "$prte_pmix_LDFLAGS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_pmix_LDFLAGS)
        PRTE_WRAPPER_FLAGS_ADD(LDFLAGS, $prte_pmix_LDFLAGS)
    fi
    if test ! -z "$prte_pmix_LIBS"; then
        PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_pmix_LIBS)
        PRTE_WRAPPER_FLAGS_ADD(LIBS, $prte_pmix_LIBS)
    fi

    AC_DEFINE_UNQUOTED([PRTE_PMIX_HEADER], [$PRTE_PMIX_HEADER], [PMIx header to use])

    PRTE_SUMMARY_ADD([[Required Packages]],[[PMIx]],[pmix],[yes ($pmix_ext_install_dir)])

    PRTE_VAR_SCOPE_POP
])
