# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_rml

test_rml_SOURCES = \
    test_rml.c

test_rml_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_rml
