# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_iof

test_iof_SOURCES = \
    test_iof.c

test_iof_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_iof
