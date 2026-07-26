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

test_ras_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_ras
