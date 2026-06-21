# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

# Offline mapping test harness.  This is a Python driver (not a compiled
# program) that runs "prterun --rtos donotlaunch --display map" over a
# matrix of map/rank/bind directives against every topology XML in
# test/topologies, and checks each map for correctness.  See SPEC.rst.
#
# This harness is deliberately NOT part of "make check".  It needs a freshly
# built prterun and runs well over a thousand mapping cases, so wiring it
# into the default test suite would make every build pay for it (and fail CI
# on environments that merely lack a usable binary).  Run it on demand with:
#
#     make check-offline
#
# The driver finds the freshly built prterun via top_builddir and the
# topologies via top_srcdir, and exits 77 (Automake "skip") when those
# prerequisites are absent.

check-offline:
	@top_srcdir='$(abs_top_srcdir)'; export top_srcdir; \
	top_builddir='$(abs_top_builddir)'; export top_builddir; \
	$(PYTHON) $(srcdir)/run_offline_maps.py

.PHONY: check-offline

EXTRA_DIST = \
	run_offline_maps.py \
	README.rst \
	SPEC.rst \
	IMPL-PLAN.rst \
	cases \
	inputs \
	golden
