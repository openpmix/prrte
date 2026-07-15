# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)

check_PROGRAMS = test_rmaps

test_rmaps_SOURCES = \
    test_rmaps_main.c      \
    test_policy_parse.c    \
    test_resolve_options.c \
    test_dispatch.c        \
    test_round_robin.c     \
    test_ppr.c             \
    test_seq.c             \
    test_rank_file.c

test_rmaps_LDADD = $(top_builddir)/src/libprrte.la

# The synthetic hwloc topologies used to exercise map/rank/bind patterns
# without a live DVM now live in the shared test/topologies directory
# (see the "Testing" section, "Offline mapper harness", in AGENTS.md).

TESTS = test_rmaps
