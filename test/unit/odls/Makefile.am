# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_odls

test_odls_SOURCES = \
    test_odls.c

test_odls_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_odls
