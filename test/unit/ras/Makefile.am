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

# A component compiled into libprrte can have its module named directly,
# which is the only way to check the vtable of one that declines to answer
# a query here.  Built as a DSO it is a separate object with no symbol to
# link against, so those checks compile out.
if MCA_BUILD_prte_ras_hosts_DSO
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_HOSTS=0
else
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_HOSTS=1
endif
if MCA_BUILD_prte_ras_simulator_DSO
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_SIMULATOR=0
else
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_SIMULATOR=1
endif
if MCA_BUILD_prte_ras_testrm_DSO
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_TESTRM=0
else
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_TESTRM=1
endif
if MCA_BUILD_prte_ras_bootstrap_DSO
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_BOOTSTRAP=0
else
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_BOOTSTRAP=1
endif
if MCA_BUILD_prte_ras_pmix_DSO
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_PMIX=0
else
test_ras_CPPFLAGS += -DPRTE_TEST_RAS_PMIX=1
endif

test_ras_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_ras
