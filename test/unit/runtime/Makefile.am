# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_runtime

test_runtime_SOURCES = \
    test_runtime.c

test_runtime_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_runtime

# Run against the components in this build tree, not just an installed
# one: without this a --enable-mca-dso build has no components to find.
AM_TESTS_ENVIRONMENT = \
    PRTE_MCA_mca_base_component_path=@PRTE_COMPONENT_BUILD_PATH@; \
    export PRTE_MCA_mca_base_component_path;
