# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_plm

test_plm_SOURCES = \
    test_plm.c

test_plm_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_plm
