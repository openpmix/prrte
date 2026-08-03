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
# Copyright (c) 2011      Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2023      Triad National Security, LLC. All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_ess_pals_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prte_ess_pals_CONFIG],[
    AC_CONFIG_FILES([src/mca/ess/pals/Makefile])

    PRTE_CHECK_PALS([ess_pals], [ess_pals_good=1], [ess_pals_good=0])

    # if check worked, set wrapper flags if so.
    # Evaluate succeed / fail
    #
    # --enable-testbuild-launchers also builds this component where PALS is
    # absent, so it is compiled somewhere.  Unlike the plm/ras launchers this
    # needs no stub headers and carries no unresolved symbols: the module
    # reads PALS_NODEID and the component reads PALS_APID, and that is the
    # whole of its PALS dependency.  It had drifted into not compiling at all
    # under -Wall -Wextra -Werror precisely because no ordinary build ever
    # reaches it.
    AS_IF([test "$ess_pals_good" = "1" || test "$prte_testbuild_launchers" = "1"],
          [$1],
          [$2])

    # No AC_SUBST of the wrapper flags: this component links nothing from
    # PALS, so its Makefile.am never referenced them.
])dnl
