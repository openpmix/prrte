# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_ess

test_ess_SOURCES = \
    test_ess.c

test_ess_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_ess
