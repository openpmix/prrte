# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_grpcomm

test_grpcomm_SOURCES = \
    test_grpcomm.c

test_grpcomm_LDADD = $(top_builddir)/src/libprrte.la

# A few cases reach at grpcomm's internals - the trackers, the signature
# classes, the epoch.  grpcomm is compiled straight into libprrte now that
# it is no longer an MCA component, so the only thing that can still put
# those out of reach is the library hiding them: compiled with
# -fvisibility=hidden they are no more linkable from here than a DSO's
# symbols were.
if !PRTE_HIDE_INTERNALS
test_grpcomm_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_GRPCOMM_INTERNALS=1
else
test_grpcomm_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_GRPCOMM_INTERNALS=0
endif

TESTS = test_grpcomm

# Run against the components in this build tree, not just an installed
# one: without this a --enable-mca-dso build has no components to find
# and every component-dependent case skips.
AM_TESTS_ENVIRONMENT = \
    PRTE_MCA_mca_base_component_path=@PRTE_COMPONENT_BUILD_PATH@; \
    export PRTE_MCA_mca_base_component_path;
