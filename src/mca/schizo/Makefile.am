#
# Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# main library setup
# the parameter sets installed with PRRTE, where a --tune file named by a
# relative path is looked for when it is not in the cwd
AM_CFLAGS = \
            -DPRTE_SCHIZO_PARAM_SETS_DIR="\"@AMCA_PARAM_SETS_DIR@\""

noinst_LTLIBRARIES = libprtemca_schizo.la
libprtemca_schizo_la_SOURCES =

# pkgdata setup
EXTRA_DIST =

# local files
headers = schizo.h
libprtemca_schizo_la_SOURCES += $(headers)

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
