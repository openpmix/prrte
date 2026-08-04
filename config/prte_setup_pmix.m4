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
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# Copyright (c) 2021-2022 Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

dnl $1 is the base cap name (i.e., what comes after "PMIX_CAP_")
dnl $2 is the action if happy
dnl $3 is the action if not happy
AC_DEFUN([PRTE_CHECK_PMIX_CAP],[

    AC_PREPROC_IFELSE(
        [AC_LANG_PROGRAM([#include <pmix_version.h>],
                         [#if !defined(PMIX_CAPABILITIES)
                          #error This PMIx does not have any capability flags
                          #endif
                          #if !defined(PMIX_CAP_$1)
                          #error This PMIx does not have the PMIX_CAP_$1 capability flag
                          #endif
                         ]
                        )
        ],
        [$2],
        [$3])

])

AC_DEFUN([PRTE_CHECK_PMIX],[

    PRTE_VAR_SCOPE_PUSH([prte_external_pmix_save_CPPFLAGS prte_pmix_support found_pmixcc])

    AC_ARG_WITH([pmix],
                [AS_HELP_STRING([--with-pmix(=DIR)],
                                [Where to find PMIx support, optionally adding DIR to the search path])])
    AC_ARG_WITH([pmix-libdir],
                [AS_HELP_STRING([--with-pmix-libdir=DIR],
                                [Look for libpmix in the given directory DIR, DIR/lib or DIR/lib64])])
    AC_ARG_WITH([pmix-extra-libs],
                [AS_HELP_STRING([--with-pmix-extra-libs=LIBS],
                                [Add LIBS as dependencies of pmix])])
    AC_ARG_ENABLE([pmix-lib-checks],
                  [AS_HELP_STRING([--disable-pmix-lib-checks],
                                  [If --disable-pmix-lib-checks is specified, configure will assume that -lpmix is available])])

    prte_pmix_support=1

    if test "$with_pmix" = "no"; then
        AC_MSG_WARN([PRTE requires PMIx support using])
        AC_MSG_WARN([an external copy that you supply.])
        AC_MSG_ERROR([Cannot continue])
    fi

    AS_IF([test "$with_pmix_extra_libs" = "yes" -o "$with_pmix_extra_libs" = "no"],
	  [AC_MSG_ERROR([--with-pmix-extra-libs requires an argument other than yes or no])])

    AS_IF([test "$enable_pmix_lib_checks" != "no"],
          [dnl Need to explicitly enable wrapper compiler to get the dependent libraries
           dnl when pkg-config is not available.
           pmix_USE_WRAPPER_COMPILER=1
           OAC_CHECK_PACKAGE([pmix],
                             [prte_pmix],
                             [pmix.h],
                             [pmix $with_pmix_extra_libs],
                             [PMIx_Init],
                             [],
                             [prte_pmix_support=0])],
          [PRTE_FLAGS_APPEND_UNIQ([PRTE_FINAL_LIBS], [$with_pmix_extra_libs])])

    AS_IF([test $prte_pmix_support -eq 0],
          [AC_MSG_WARN([PRRTE requires PMIx support using an external copy that you supply.])
           AC_MSG_ERROR([Cannot continue.])])

    prte_external_pmix_save_CPPFLAGS=$CPPFLAGS
    PRTE_FLAGS_PREPEND_UNIQ(CPPFLAGS, $prte_pmix_CPPFLAGS)

    # if the version file exists, then we need to parse it to find
    # the actual release series
    # NOTE: We have already read PRRTE's VERSION file, so we can use
    # $pmix_min_version.
    prte_pmix_min_num_version=PRTE_PMIX_NUMERIC_MIN_VERSION
    prte_pmix_min_version=PRTE_PMIX_MIN_VERSION
    prte_pmix_max_version=PRTE_PMIX_MAX_VERSION
    AC_MSG_CHECKING([version at or above v$prte_pmix_min_version])
    AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                        #include <pmix_version.h>
                                        #if (PMIX_NUMERIC_VERSION < $prte_pmix_min_num_version)
                                        #error "not version $prte_pmix_min_num_version or above"
                                        #endif
                                       ], [])],
                      [AC_MSG_RESULT([yes])],
                      [AC_MSG_RESULT(no)
                       AC_MSG_WARN([PRRTE requires PMIx v$prte_pmix_min_num_version or above.])
                       AC_MSG_ERROR([Please select a supported version and configure again])])

    # NOTE: We have already read PRRTE's VERSION file, so we can use
    # $pmix_max_version.
    prte_pmix_max_num_version=PRTE_PMIX_NUMERIC_MAX_VERSION
    prte_pmix_max_version=PRTE_PMIX_MAX_VERSION
    AC_MSG_CHECKING([version below v$prte_pmix_max_version])
    AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                                        #include <pmix_version.h>
                                        #if !(PMIX_NUMERIC_VERSION < $prte_pmix_max_num_version)
                                        #error "not below version $prte_pmix_max_num_version"
                                        #endif
                                       ], [])],
                      [AC_MSG_RESULT([yes])],
                      [AC_MSG_RESULT(no)
                       AC_MSG_WARN([PRRTE requires PMIx be below v$prte_pmix_max_num_version.])
                       AC_MSG_ERROR([Please select a supported version and configure again])])

    AC_CHECK_HEADER([src/util/pmix_argv.h], [],
                    [AC_MSG_ERROR([Could not find PMIx devel headers.  Can not continue.])])

    # restore the global flags
    CPPFLAGS=$prte_external_pmix_save_CPPFLAGS

    PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_CPPFLAGS, $prte_pmix_CPPFLAGS)
    PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LDFLAGS, $prte_pmix_LDFLAGS)
    PRTE_FLAGS_APPEND_UNIQ(PRTE_FINAL_LIBS, $prte_pmix_LIBS)

    AC_DEFINE_UNQUOTED([PRTE_PMIX_MINIMUM_VERSION],
                       [$prte_pmix_min_num_version],
                       [Minimum supported PMIx version])

    AC_DEFINE_UNQUOTED([PRTE_PMIX_MIN_VERSION_STRING],
                       ["$prte_pmix_min_version"],
                       [Minimum supported PMIx version])

    AC_DEFINE_UNQUOTED([PRTE_PMIX_MAX_VERSION_STRING],
                       ["$prte_pmix_max_version"],
                       [Maximum supported PMIx version])

    found_pmixcc=0
    PMIXCC_PATH="pmixcc"
    AS_IF([test -n "${with_pmix}"],
          [PMIXCC_PATH="${with_pmix}/bin/$PMIXCC_PATH"])
    PRTE_LOG_COMMAND([pmixcc_showme_results=`$PMIXCC_PATH --showme:version 2>&1`], [found_pmixcc=1])
    PRTE_LOG_MSG([pmixcc version: $pmixcc_showme_results])
    AS_IF([test $found_pmixcc -eq 0],
          [AC_MSG_WARN([Could not find $PMIXCC_PATH])
           PMIXCC_PATH=])
    AM_CONDITIONAL([PRTE_HAVE_PMIXCC], [test $found_pmixcc -eq 1])
    AC_SUBST([PMIXCC_PATH])

    # Check for any needed capabilities from the PMIx we found.
    #
    # Note: if the PMIx we found does not define capability flags,
    # then it definitely does not have the capability flags we're
    # looking for.

    prte_external_pmix_save_CPPFLAGS=$CPPFLAGS
    prte_external_pmix_save_LDFLAGS=$LDFLAGS
    prte_external_pmix_save_LIBS=$LIBS

    PRTE_FLAGS_APPEND_UNIQ(CPPFLAGS, $PRTE_FINAL_CPPFLAGS)
    PRTE_FLAGS_APPEND_UNIQ(LDFLAGS, $PRTE_FINAL_LDFLAGS)
    PRTE_FLAGS_APPEND_UNIQ(LIBS, $PRTE_FINAL_LIBS)

    AC_MSG_CHECKING([for functional form of GET_NUMBER macro])
    PRTE_CHECK_PMIX_CAP([GET_NUMBER_FN],
                        [AC_MSG_RESULT([yes])
                         prte_get_number_macro=1],
                        [AC_MSG_RESULT([no])
                         prte_get_number_macro=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_GET_NUMBER_FN],
                       [$prte_get_number_macro],
                       [Whether or not PMIx has the GET_NUMBER FN])

    AC_MSG_CHECKING([for support of stop progress thread API])
    PRTE_CHECK_PMIX_CAP([STOP_PRGTHRD],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_stop_progress_thread=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_stop_progress_thread=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_STOP_PRGTHRD],
                       [$prte_pmix_stop_progress_thread],
                       [Whether or not PMIx supports the stop progress thread API])

    AC_MSG_CHECKING([for support of version 2 server upcalls])
    PRTE_CHECK_PMIX_CAP([UPCALLS2],
                        [AC_MSG_RESULT([yes])
                         prte_server2_upcalls=1],
                        [AC_MSG_RESULT([no])
                         prte_server2_upcalls=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_SERVER2_UPCALLS],
                       [$prte_server2_upcalls],
                       [Whether or not PMIx supports server2 upcalls])

    AC_MSG_CHECKING([for in-memory show-help content compatibility])
    PRTE_CHECK_PMIX_CAP([INMEMHELP],
                        [AC_MSG_RESULT([yes])],
                        [AC_MSG_RESULT([no])
                         AC_MSG_WARN([Your PMIx version either does not have])
                         AC_MSG_WARN([the capabilities feature or does not])
                         AC_MSG_WARN([include the PMIX_CAP_INMEMHELP capability flag.])
                         AC_MSG_WARN([This PRRTE version requires that support])
                         AC_MSG_WARN([in order to build. Please point us to a])
                         AC_MSG_WARN([PMIx installation with the required capability.])
                         AC_MSG_ERROR([Cannot continue.])])

    AC_MSG_CHECKING([for PMIx regex2 API support])
    PRTE_CHECK_PMIX_CAP([REGEX2],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_have_regex2=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_have_regex2=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_HAVE_REGEX2],
                       [$prte_pmix_have_regex2],
                       [Whether or not PMIx supports the PMIx_generate_regex2 API])

    AC_MSG_CHECKING([for PMIx group fault-tolerance support])
    PRTE_CHECK_PMIX_CAP([GROUP_FT],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_have_group_ft=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_have_group_ft=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_HAVE_GROUP_FT],
                       [$prte_pmix_have_group_ft],
                       [Whether PMIx supports the group fault-tolerance feature set, including the PMIX_GROUP_CANCEL host operation])

    AC_MSG_CHECKING([for PMIx command-line qualifier value support])
    PRTE_CHECK_PMIX_CAP([CLI_QUAL_VALUE],
                        [AC_MSG_RESULT([yes])],
                        [AC_MSG_RESULT([no])
                         AC_MSG_WARN([PRRTE requires pmix_cli_qualifier_value(), which this])
                         AC_MSG_WARN([PMIx does not provide. It reads the value of a command])
                         AC_MSG_WARN([line qualifier - the text after its '=' - which cannot])
                         AC_MSG_WARN([be done correctly outside PMIx, as the option matcher])
                         AC_MSG_WARN([accepts any unambiguous prefix of a qualifier's name.])
                         AC_MSG_ERROR([Please update PMIx and configure again])])

    dnl The "pattern" qualifier on "--output file=NAME" hands the naming of
    dnl the output files to the user.  Expanding the pattern is PMIx's job -
    dnl PMIx owns the IOF sinks and is the only place that knows the rank and
    dnl namespace at the moment a file is opened - so without those helpers
    dnl PRRTE has nothing to hand the request to, and says so rather than
    dnl accepting a qualifier it cannot honor.
    AC_MSG_CHECKING([for PMIx output-file pattern support])
    PRTE_CHECK_PMIX_CAP([IOF_FILE_PATTERN],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_iof_file_pattern=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_iof_file_pattern=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_IOF_FILE_PATTERN],
                       [$prte_pmix_iof_file_pattern],
                       [Whether PMIx can expand an output-file name pattern])

    dnl The environment directives (--set-env, --prepend-env, --append-env,
    dnl --unset-env, -x) edit each other, so the order the user gave them in
    dnl is the value the process ends up with.  Recovering that order from a
    dnl parse result requires PMIx to have recorded where each value was
    dnl given: the result groups every occurrence of an option onto that
    dnl option's single instance, so an option repeated after another has
    dnl intervened cannot be placed from the grouped view alone.  Without the
    dnl stamps we fall back to that view, which is right for every command
    dnl line that does not interleave repeats.
    AC_MSG_CHECKING([for PMIx command-line occurrence order])
    PRTE_CHECK_PMIX_CAP([CLI_ORDER],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_cli_order=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_cli_order=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_CLI_ORDER],
                       [$prte_pmix_cli_order],
                       [Whether PMIx records where each command line option occurred])

    dnl A tool can attach to any daemon, not just the DVM master, and the
    dnl output of the job it launches has to reach it there.  The master is
    dnl the only process that sees all of a job's output, so it relays a
    dnl copy to the daemon hosting the tool - but that daemon must hand the
    dnl copy to its PMIx server WITHOUT emitting it, since the daemon that
    dnl actually hosts those processes has already written whatever the
    dnl output directives called for.  Two servers writing one --output file
    dnl is the failure that guards against.  PMIx has to be able to be told
    dnl that per delivery; without it we cannot relay safely and a tool on a
    dnl non-master daemon sees only the output of processes its own daemon
    dnl happens to host.
    AC_MSG_CHECKING([for PMIx per-delivery IOF local-output control])
    PRTE_CHECK_PMIX_CAP([IOF_DELIVER_LOCAL],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_iof_deliver_local=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_iof_deliver_local=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_IOF_DELIVER_LOCAL],
                       [$prte_pmix_iof_deliver_local],
                       [Whether PMIx honors PMIX_IOF_LOCAL_OUTPUT on an IOF delivery])

    dnl The iof has always had the shape of XON/XOFF back-pressure on stdin -
    dnl a daemon whose stdin sink passes PRTE_IOF_MAX_INPUT_BUFFERS tells the
    dnl HNP so - but the HNP could not act on it.  Stdin originates in a PMIx
    dnl server's read of its own stdin or in a tool's PMIx_IOF_push, and until
    dnl PMIx grew PMIx_server_IOF_flow_control there was no way to stop either
    dnl at the source: a refusal anywhere would have dropped bytes rather than
    dnl slowed the producer.  Without this the HNP consumes the message and
    dnl the daemon's sink simply queues, bounded only by iof_base_output_limit.
    AC_MSG_CHECKING([for PMIx IOF stdin flow control])
    PRTE_CHECK_PMIX_CAP([IOF_FLOW_CONTROL],
                        [AC_MSG_RESULT([yes])
                         prte_pmix_iof_flow_control=1],
                        [AC_MSG_RESULT([no])
                         prte_pmix_iof_flow_control=0])
    AC_DEFINE_UNQUOTED([PRTE_PMIX_IOF_FLOW_CONTROL],
                       [$prte_pmix_iof_flow_control],
                       [Whether PMIx supports PMIx_server_IOF_flow_control])

    AC_MSG_CHECKING([for LTO compatibility])
    PRTE_CHECK_PMIX_CAP([LTO],
                        [PRTE_PMIX_LTO_CAPABILITY=1
                         AC_MSG_RESULT([yes])],
                        [AC_MSG_RESULT([no])
                         AC_MSG_WARN([Your PMIx version either does not have])
                         AC_MSG_WARN([the capabilities feature or does not])
                         AC_MSG_WARN([include the PMIX_CAP_LTO capability flag])
                         AC_MSG_WARN([This build will not be compatible with the])
                         AC_MSG_WARN([LTO optimizer. All LTO-related flags will])
                         AC_MSG_WARN([be removed from the build])
                         PRTE_PMIX_LTO_CAPABILITY=0])

    AC_DEFINE_UNQUOTED([PRTE_PMIX_LTO_CAPABILITY],
                       [$PRTE_PMIX_LTO_CAPABILITY],
                       [Whether or not PMIx has the LTO capability flag set])

    dnl The elastic-DVM completion contract directs two PMIx events at the
    dnl process that requested a DVM size change: PMIX_DVM_IS_READY on success
    dnl and PMIX_ERR_DVM_MOD on failure.  These are plain #define'd status
    dnl codes, not behavioral capability flags, so probe for the symbols
    dnl themselves rather than a PMIX_CAP_* flag.  A PMIx that defines neither
    dnl leaves the completion notification compiled out (see
    dnl docs/plans/elastic_dvm/).
    AC_MSG_CHECKING([for PMIx DVM modification event codes])
    AC_PREPROC_IFELSE(
        [AC_LANG_PROGRAM([[#include <pmix.h>
#if !defined(PMIX_DVM_IS_READY) || !defined(PMIX_ERR_DVM_MOD)
#error DVM modification event codes not present
#endif
]], [[]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([PRTE_HAVE_DVM_MOD_EVENTS], [1],
                   [Whether PMIx defines the DVM modification event codes])],
        [AC_MSG_RESULT([no])
         AC_DEFINE([PRTE_HAVE_DVM_MOD_EVENTS], [0],
                   [Whether PMIx defines the DVM modification event codes])])

    # restore the global flags
    CPPFLAGS=$prte_external_pmix_save_CPPFLAGS
    LDFLAGS=$prte_external_pmix_save_LDFLAGS
    LIBS=$prte_external_pmix_save_LIBS

    PRTE_SUMMARY_ADD([Required Packages], [PMIx], [], [$prte_pmix_SUMMARY])

    PRTE_VAR_SCOPE_POP
])
