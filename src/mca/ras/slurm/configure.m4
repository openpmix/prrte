# -*- shell-script -*-
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
# Copyright (c) 2011-2013 Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2025-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PRTE_RAS_SLURM_CHECK_VERSION
# -----------------------------------------------------------
# Ask the Slurm client tools what version they are, and set
#
#   prte_ras_slurm_version        e.g. 24.11.6, or "unknown"
#   prte_ras_slurm_version_major  e.g. 24, or empty when unknown
#   prte_ras_slurm_version_minor  e.g. 11, or empty when unknown
#
# There is no library to interrogate: this component calls no Slurm function
# and links nothing (see Makefile.am), so the only thing on the machine that
# knows the version is the command line itself.  Several tools are tried
# because "--version" is not free of side effects -- every Slurm client
# establishes a configuration source first, so on a build node with the
# binaries but no slurm.conf the command exits 1 having printed
#
#   scontrol: fatal: Could not establish a configuration source
#
# and no version at all.  That case is *undetectable*, not "old": it is
# entirely ordinary to configure PRRTE somewhere that is not a Slurm client,
# and the caller below treats it accordingly.
AC_DEFUN([PRTE_RAS_SLURM_CHECK_VERSION],[
    PRTE_VAR_SCOPE_PUSH([prte_slurm_ver_tool prte_slurm_ver_path prte_slurm_ver_out])

    prte_ras_slurm_version=unknown
    prte_ras_slurm_version_major=
    prte_ras_slurm_version_minor=

    AC_MSG_CHECKING([Slurm version])
    for prte_slurm_ver_tool in scontrol srun sbatch sinfo; do
        prte_slurm_ver_path=
        PRTE_WHICH([$prte_slurm_ver_tool], [prte_slurm_ver_path])
        AS_IF([test -z "$prte_slurm_ver_path"], [continue])

        # Match only a line that IS a version report.  Slurm prefixes its
        # diagnostics with the tool name, so a "grep slurm" would happily
        # accept an error message.
        prte_slurm_ver_out=`$prte_slurm_ver_path --version 2>/dev/null | \
            sed -n ['s/^slurm[ 	][ 	]*\([0-9][0-9]*\.[0-9][0-9]*[0-9.a-zA-Z-]*\).*$/\1/p'] | \
            head -n 1`
        AS_IF([test -n "$prte_slurm_ver_out"],
              [prte_ras_slurm_version=$prte_slurm_ver_out
               prte_ras_slurm_version_major=`echo "$prte_slurm_ver_out" | cut -d. -f1`
               prte_ras_slurm_version_minor=`echo "$prte_slurm_ver_out" | cut -d. -f2`
               break])
    done

    AS_IF([test "$prte_ras_slurm_version" = "unknown"],
          [AC_MSG_RESULT([unknown (no Slurm command answered --version)])],
          [AC_MSG_RESULT([$prte_ras_slurm_version])])

    PRTE_VAR_SCOPE_POP
])dnl


# PRTE_RAS_SLURM_CHECK_EXTENSIONS
# -----------------------------------------------------------
# Decide whether to build the Slurm "extensions": the elastic modify surface
# (PMIX_ALLOC_EXTEND / RELEASE / REQ_CANCEL) and the "scontrol show job
# --json" parser it is built on.  Sets ras_slurm_extensions_good to 0 or 1.
#
# The default when the version cannot be determined is ENABLED, deliberately.
# Configuring on a machine that is not a Slurm client is the common case --
# build node, container, cross-compile -- and defaulting off there would
# silently remove a working feature from builds that have always had it, with
# nothing but a line of configure output to say so.  Erring the other way
# costs the same run-time failure this check exists to explain, and no more.
# --enable/--disable-slurm-extensions is the override in both directions.
AC_DEFUN([PRTE_RAS_SLURM_CHECK_EXTENSIONS],[
    PRTE_VAR_SCOPE_PUSH([prte_slurm_ext_msg prte_slurm_ext_numeric prte_slurm_ext_min])

    AC_ARG_ENABLE([slurm-extensions],
        [AS_HELP_STRING([--enable-slurm-extensions],
            [Build the Slurm elastic allocation extensions -- the PMIx allocation extend/release/cancel surface and the "scontrol show job --json" parser it needs.  They require Slurm 24.05 or later at run time (default: yes when the Slurm found at configure time is new enough, or when no Slurm can be found).  There is nothing to point this at: the component links no Slurm library, so this is a feature switch, not a package location])])

    # The oldest Slurm whose "scontrol show job --json" this component can
    # read.  Slurm 24.05 shipped data parser v0.0.41, which is where
    # job_resources acquired the nested form ras_slurm_jansson.c parses:
    #
    #     "job_resources": { "nodes": { "count": N, "list": "...",
    #                                   "allocation": [ ... ] } }
    #
    # Through 23.11 the same query answers with job_resources.nodes as a plain
    # string beside a flat "allocated_nodes" array, and every extend fails at
    # run time with "Failed to parse input JSON" -- a diagnostic that says
    # nothing about versions and sends people looking at PRRTE.
    prte_ras_slurm_min_major=24
    prte_ras_slurm_min_minor=5
    prte_ras_slurm_min_str=24.05

    PRTE_RAS_SLURM_CHECK_VERSION

    ras_slurm_extensions_good=0
    prte_slurm_ext_msg=

    AC_MSG_CHECKING([whether to build the Slurm elastic extensions])

    # Refuse a value we do not understand rather than silently treating it as
    # "no opinion" -- --enable-slurm-extensions=false would otherwise turn the
    # extensions ON, which is the opposite of what was asked for.
    AS_IF([test -n "$enable_slurm_extensions" && \
           test "$enable_slurm_extensions" != "yes" && \
           test "$enable_slurm_extensions" != "no"],
          [AC_MSG_ERROR([--enable-slurm-extensions takes "yes" or "no", not "$enable_slurm_extensions"])])

    AS_IF([test "$enable_slurm_extensions" = "no"],
          [prte_slurm_ext_msg="no (disabled by --disable-slurm-extensions)"],
          [AS_IF([test "$enable_slurm_extensions" = "yes"],
                 [ras_slurm_extensions_good=1
                  prte_slurm_ext_msg="yes (forced by --enable-slurm-extensions)"],
                 [# no user opinion: decide from what we found
                  AS_IF([test "$prte_testbuild_launchers" = "1"],
                        [# --enable-testbuild-launchers exists to COMPILE what
                         # this machine cannot run, so the version of the Slurm
                         # sitting next to it is beside the point.
                         ras_slurm_extensions_good=1
                         prte_slurm_ext_msg="yes (testbuild: compiled, not runnable)"],
                        [AS_IF([test "$prte_ras_slurm_version" = "unknown"],
                               [ras_slurm_extensions_good=1
                                prte_slurm_ext_msg="yes (no Slurm here to check; assuming $prte_ras_slurm_min_str or later at run time)"],
                               [prte_slurm_ext_numeric=`expr "$prte_ras_slurm_version_major" \* 100 + "$prte_ras_slurm_version_minor"`
                                prte_slurm_ext_min=`expr "$prte_ras_slurm_min_major" \* 100 + "$prte_ras_slurm_min_minor"`
                                AS_IF([test "$prte_slurm_ext_numeric" -ge "$prte_slurm_ext_min"],
                                      [ras_slurm_extensions_good=1
                                       prte_slurm_ext_msg="yes (Slurm $prte_ras_slurm_version)"],
                                      [prte_slurm_ext_msg="no (Slurm $prte_ras_slurm_version is older than $prte_ras_slurm_min_str)"])])])])])

    # The parser is written against jansson; without it there is nothing to
    # enable, whatever the scheduler's version.
    AS_IF([test "$ras_slurm_extensions_good" = "1" && \
           test "$ras_slurm_jansson_good" != "1" && \
           test "$prte_testbuild_launchers" != "1"],
          [ras_slurm_extensions_good=0
           prte_slurm_ext_msg="no (jansson not available -- see --with-jansson)"])

    AC_MSG_RESULT([$prte_slurm_ext_msg])

    # Do not drag jansson into the link when the sources that use it are not
    # being compiled.  Without this the component would carry a dependency it
    # has no caller for.
    AS_IF([test "$ras_slurm_extensions_good" = "0"],
          [ras_slurm_jansson_CPPFLAGS=
           ras_slurm_jansson_LDFLAGS=
           ras_slurm_jansson_LIBS=])

    # The flag the component gates on.  Defined to 0 or 1 always, never
    # #undef'd, so a misspelling in an "#if" is a compiler error rather than a
    # silently disabled feature.
    AC_DEFINE_UNQUOTED([PRTE_HAVE_SLURM_EXTENSIONS], [$ras_slurm_extensions_good],
        [Whether the Slurm elastic allocation extensions were built])
    AC_DEFINE_UNQUOTED([PRTE_SLURM_VERSION_STRING], ["$prte_ras_slurm_version"],
        [Version of the Slurm found when PRRTE was configured, or "unknown"])
    AC_DEFINE_UNQUOTED([PRTE_SLURM_MIN_EXT_VERSION], ["$prte_ras_slurm_min_str"],
        [Oldest Slurm whose JSON the elastic extensions can parse])

    # ...and the build gate proper: which sources compile.
    AM_CONDITIONAL([PRTE_WANT_SLURM_EXTENSIONS],
          [test "$ras_slurm_extensions_good" = "1"])

    PRTE_SUMMARY_ADD([Resource Managers], [Slurm elastic extensions], [],
                     [$prte_slurm_ext_msg])

    PRTE_VAR_SCOPE_POP
])dnl


# MCA_ras_slurm_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prte_ras_slurm_CONFIG],[
    AC_CONFIG_FILES([src/mca/ras/slurm/Makefile])

    PRTE_CHECK_SLURM([ras_slurm], [ras_slurm_good=1], [ras_slurm_good=0])
    PRTE_CHECK_JANSSON([ras_slurm_jansson], [ras_slurm_jansson_good=1], [ras_slurm_jansson_good=0])
    PRTE_RAS_SLURM_CHECK_EXTENSIONS

    # if check worked, set wrapper flags if so.
    # Evaluate succeed / fail
    AS_IF([test "$ras_slurm_good" = "1" || test "$prte_testbuild_launchers" = "1"],
          [$1],
          [$2])

    # Let the unit tests know whether this component's symbols will be
    # available to link against: it is NOT built on every platform (see
    # PRTE_CHECK_SLURM) and --without-slurm removes it entirely.
    AM_CONDITIONAL([PRTE_HAVE_RAS_SLURM],
          [test "$ras_slurm_good" = "1" || test "$prte_testbuild_launchers" = "1"])

    # set build flags to use in makefile
    AC_SUBST([ras_slurm_jansson_CPPFLAGS])
    AC_SUBST([ras_slurm_jansson_LDFLAGS])
    AC_SUBST([ras_slurm_jansson_LIBS])
])dnl
