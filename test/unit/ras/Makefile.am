# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_ras

test_ras_SOURCES = \
    test_ras.c

# The test names no component symbol - it reaches ras/slurm through the
# framework, so it works whether that component was linked statically or
# loaded as a plugin, and needs nothing from this Makefile to do it.  All
# this conditional buys is a precise message when the component turns out
# not to be there: "not built on this platform" is expected, "built but the
# framework has not got it" is worth saying out loud.
if PRTE_HAVE_RAS_SLURM
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=1
else
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=0
endif

test_ras_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_ras

# Run against the components in this build tree, not just an installed
# one: without this a --enable-mca-dso build has no components to find
# and every component-dependent case skips.
AM_TESTS_ENVIRONMENT = \
    PRTE_MCA_mca_base_component_path=@PRTE_COMPONENT_BUILD_PATH@; \
    export PRTE_MCA_mca_base_component_path;
