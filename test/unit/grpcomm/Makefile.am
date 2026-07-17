# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_grpcomm

test_grpcomm_SOURCES = \
    test_grpcomm.c

test_grpcomm_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_grpcomm
