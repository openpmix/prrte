# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_filem

test_filem_SOURCES = \
    test_filem.c

test_filem_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_filem
