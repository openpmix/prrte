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

# A few cases reach at grpcomm/direct's own symbols - its module entry
# points and the classes it defines.  Those are linkable only when the
# component is compiled into libprrte; built as a DSO it is a separate
# object the test never links against.  Compile them in only then.
if !MCA_BUILD_prte_grpcomm_direct_DSO
test_grpcomm_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_GRPCOMM_DIRECT=1
else
test_grpcomm_CPPFLAGS = $(AM_CPPFLAGS) -DPRTE_TEST_GRPCOMM_DIRECT=0
endif

TESTS = test_grpcomm
