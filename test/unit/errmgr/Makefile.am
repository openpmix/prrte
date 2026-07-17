# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_errmgr

test_errmgr_SOURCES = \
    test_errmgr.c

test_errmgr_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_errmgr
