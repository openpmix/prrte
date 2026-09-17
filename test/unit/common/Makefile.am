# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_common_slurm

test_common_slurm_SOURCES = \
    test_common_slurm.c

# Where the common library's symbols live depends on the build shape, and
# the two shapes are mutually exclusive: in the default static build
# configure has already rolled it into libprrte, so naming it again here
# would link a second copy of every object; in a DSO build it is a shared
# library of its own that libprrte does not link, so it has to be named.
# This is the same conditional the three slurm components carry -- see
# src/mca/common/AGENTS.md.
if MCA_BUILD_prte_common_slurm_DSO
test_common_slurm_LDADD = \
    $(top_builddir)/src/mca/common/slurm/libmca_common_slurm.la \
    $(top_builddir)/src/libprrte.la
else
test_common_slurm_LDADD = $(top_builddir)/src/libprrte.la
endif

TESTS = test_common_slurm
