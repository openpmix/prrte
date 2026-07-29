# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_prted

test_prted_SOURCES = \
    test_prted.c

test_prted_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_prted

# Run against the components in this build tree, not just an installed
# one: without this a --enable-mca-dso build has no components to find
# and the framework-dependent cases fail to resolve a mapper.
AM_TESTS_ENVIRONMENT = \
    PRTE_MCA_mca_base_component_path=@PRTE_COMPONENT_BUILD_PATH@; \
    export PRTE_MCA_mca_base_component_path;
