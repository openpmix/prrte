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
# Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2011      Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_ess_lsf_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_prte_ess_lsf_CONFIG],[
    AC_CONFIG_FILES([src/mca/ess/lsf/Makefile])

    PRTE_CHECK_LSF([ess_lsf], [ess_lsf_good=1], [ess_lsf_good=0])

    # if check worked, set wrapper flags if so.
    # Evaluate succeed / fail
    #
    # --enable-testbuild-launchers also builds this component where LSF is
    # absent, so it is compiled somewhere.  Unlike the plm/ras launchers this
    # needs no stub headers and carries no unresolved symbols - as the
    # Makefile.am says, the plugin calls no LSF library function at all: the
    # module reads LSF_PM_TASKID and the component reads LSB_JOBID, and that
    # is the whole of its LSF dependency.  It had drifted into not compiling
    # at all under -Wall -Wextra -Werror precisely because no ordinary build
    # ever reaches it.
    AS_IF([test "$ess_lsf_good" = "1" || test "$prte_testbuild_launchers" = "1"],
          [$1],
          [$2])

    # No AC_SUBST of the wrapper flags: this component links nothing from
    # LSF, so its Makefile.am never referenced them.
])dnl
