#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A word of explanation, following the same reasoning as
# opal/mca/common/sm in Open MPI:
#
# Both ras/slurm and plm/slurm need to know what version of Slurm they are
# running under, and neither owns the answer.  Asking costs a popen() of a
# Slurm client, and two components asking separately is two places for the
# answer to drift -- so it is asked once, here.  There are two build shapes,
# and both end up with one copy of the code and data:
#
# 1. libmca_common_slurm.la is a shared library.  Linking it into every
#    component that needs it makes the OS linker load exactly one copy.
#
# 2. libmca_common_slurm.la is a static library.  It is rolled up into
#    libprrte, and also into each component that links it -- but every one of
#    those is itself linked against libprrte, and linkers universally know
#    how to resolve that down to a single copy.
#
# Note that building this common component statically while the components
# that use it are dynamic is *not* supported -- the same caveat Open MPI
# carries.

headers = \
        common_slurm.h

sources = \
        common_slurm.c

# As above, we will either have an installable or a noinst result.  The
# installable one has to follow the MCA prefix naming rules
# (libmca_<type>_<name>.la) because that is the name configure puts on
# libprrte's link line for the static case.
#
# To keep the components that link this library free of conditional logic, we
# *always* end up with a libtool library named libmca_common_slurm.la: when
# the noinst version is what was built, the all-local rule below sym links it
# to that name (libtool does the right things underneath).
lib_LTLIBRARIES =
noinst_LTLIBRARIES =
comp_inst = libmca_common_slurm.la
comp_noinst = libmca_common_slurm_noinst.la

if MCA_BUILD_prte_common_slurm_DSO
lib_LTLIBRARIES += $(comp_inst)
else
noinst_LTLIBRARIES += $(comp_noinst)
endif

libmca_common_slurm_la_SOURCES = $(headers) $(sources)
libmca_common_slurm_la_LDFLAGS = -version-info $(libmca_prte_common_slurm_so_version)
libmca_common_slurm_noinst_la_SOURCES = $(headers) $(sources)

# These two rules sym link the "noinst" libtool library filename to the
# installable filename in the static case (case 2 above).
all-local:
	if test -z "$(lib_LTLIBRARIES)"; then \
	  rm -f "$(comp_inst)"; \
	  $(LN_S) "$(comp_noinst)" "$(comp_inst)"; \
	fi

clean-local:
	if test -z "$(lib_LTLIBRARIES)"; then \
	  rm -f "$(comp_inst)"; \
	fi
