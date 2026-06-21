# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

# Offline mapping test harness.  This is a Python driver (not a compiled
# program) that runs "prterun --rtos donotlaunch --display map" over a
# matrix of map/rank/bind directives against every topology XML in
# test/topologies, and checks each map for correctness.  See SPEC.rst.
#
# The driver finds the freshly built prterun via top_builddir and the
# topologies via top_srcdir, and exits 77 (Automake "skip") when those
# prerequisites are absent -- so "make check" records a SKIP, not an
# error, on configurations that cannot run it.

AM_TESTS_ENVIRONMENT = \
	top_srcdir='$(top_srcdir)'; export top_srcdir; \
	top_builddir='$(abs_top_builddir)'; export top_builddir;

TEST_EXTENSIONS = .py
PY_LOG_COMPILER = $(PYTHON)

TESTS = run_offline_maps.py

EXTRA_DIST = \
	run_offline_maps.py \
	README.rst \
	SPEC.rst \
	IMPL-PLAN.rst \
	cases \
	inputs \
	golden
