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

# ras/slurm is not built on every platform and can be removed with
# --without-slurm, so its symbols are not always there to link against.
# It is also in the default --enable-mca-dso list (config/prte_mca.m4),
# and a run-time loadable plugin puts nothing in libprrte for a test
# program to reference - on Darwin the module is a bundle, which cannot
# be linked into an executable at all.  Configure with
# --enable-mca-static=ras-slurm to build it into libprrte and get the
# Slurm coverage below.
if PRTE_HAVE_RAS_SLURM
if MCA_BUILD_prte_ras_slurm_DSO
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=0
else
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=1
endif
else
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=0
endif

test_ras_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_ras
