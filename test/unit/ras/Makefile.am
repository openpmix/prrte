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
if PRTE_HAVE_RAS_SLURM
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=1
else
test_ras_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_HAVE_RAS_SLURM=0
endif

test_ras_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_ras
