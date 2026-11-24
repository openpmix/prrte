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
# Copyright (c) 2011-2013 Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_plm_tm_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
#
# The component itself links nothing of TM's: the TM library is linked
# only into prte_tm_spawn, the helper the component runs, because some
# builds of it carry private copies of libraries PMIx also uses. So the
# check's flags are the helper's (plm_tm_helper_*), and the component is
# built whenever the helper can be.
AC_DEFUN([MCA_prte_plm_tm_CONFIG],[
    AC_CONFIG_FILES([src/mca/plm/tm/Makefile])

    # Only with PBS support: this launcher starts daemons only on the nodes
    # of the PBS job, which only ras/pbs reads, so building it without
    # ras/pbs would be a launcher with no allocation it can serve.
    # (--with-tm implies PBS support, and contradicting it is an error -
    # see PRTE_SETUP_PBS - so the TM check is skipped only where nobody
    # asked for PBS.)
    PRTE_CHECK_PBS([plm_tm_pbs], [plm_tm_pbs_good=1], [plm_tm_pbs_good=0])
    AS_IF([test "$plm_tm_pbs_good" = "1"],
          [PRTE_CHECK_TM([plm_tm_helper], [plm_tm_good=1], [plm_tm_good=0])],
          [plm_tm_good=0])

    AS_IF([test "$plm_tm_good" = "1" || test "$prte_testbuild_launchers" = "1"],
          [$1],
          [$2])

    # With --enable-testbuild-launchers and no TM, the helper is built
    # against stand-in definitions so the tree still links; it can do no
    # actual work
    AM_CONDITIONAL([PRTE_PLM_TM_HAVE_TM], [test "$plm_tm_good" = "1"])

    # The TM library is rarely where the run-time linker looks (OpenPBS
    # puts it in /opt/pbs/lib and its own commands find it by run path),
    # and the helper must start in whatever environment the job has. So
    # give it a run path to every directory it was linked from; libtool's
    # -R does that for a program.
    plm_tm_helper_RPATH=
    for plm_tm_flag in $plm_tm_helper_LDFLAGS; do
        AS_CASE([$plm_tm_flag],
                [-L*], [PRTE_APPEND([plm_tm_helper_RPATH], ["-R`echo $plm_tm_flag | cut -c3-`"])])
    done
    AS_UNSET([plm_tm_flag])

    AC_SUBST([plm_tm_helper_RPATH])
    AC_SUBST([plm_tm_helper_CPPFLAGS])
    AC_SUBST([plm_tm_helper_LDFLAGS])
    AC_SUBST([plm_tm_helper_LIBS])
])dnl
