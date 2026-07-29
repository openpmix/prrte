# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_tools

test_tools_SOURCES = \
    test_tools.c

test_tools_LDADD = $(top_builddir)/src/libprrte.la

TESTS = test_tools
