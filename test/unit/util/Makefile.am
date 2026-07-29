# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_util

test_util_SOURCES = \
    test_util.c

test_util_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_util
