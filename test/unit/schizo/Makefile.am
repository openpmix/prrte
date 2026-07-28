# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_schizo

test_schizo_SOURCES = \
    test_schizo_main.c  \
    test_helpers.c      \
    test_normalize.c    \
    test_directives.c   \
    test_sanity.c       \
    test_output.c       \
    test_personality.c

noinst_HEADERS = test_schizo.h

test_schizo_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_schizo

# Run against the components in this build tree, not just an installed one:
# the personality tests reach prte/ompi through the framework, which in a
# --enable-mca-dso build has nothing to open without this.
AM_TESTS_ENVIRONMENT = \
    PRTE_MCA_mca_base_component_path=@PRTE_COMPONENT_BUILD_PATH@; \
    export PRTE_MCA_mca_base_component_path;
