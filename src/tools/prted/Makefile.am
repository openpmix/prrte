#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

include $(top_srcdir)/Makefile.prrte-rules

MD_FILES = prted.1.md
man_pages_from_md = $(MD_FILES:.1.md=.1)
EXTRA_DIST = $(MD_FILES) $(man_pages_from_md)
AM_LDFLAGS = $(PRRTE_EXTRA_LIB_LDFLAGS) $(prrte_hwloc_LDFLAGS) $(prrte_libevent_LDFLAGS) $(prrte_pmix_LDFLAGS)

bin_PROGRAMS = prted

if PRRTE_ENABLE_MAN_PAGES
man_MANS = $(man_pages_from_md)
endif

prted_SOURCES = prted.c
# the following empty prted_LDFLAGS is used
#  so that the prted can be compiled statically
#  by simply changing the value of this from
#  nothing to -all-static in the Makefile.in
#  nice for systems that don't have all the shared
#  libraries on the computes
prted_LDFLAGS =
prted_LDADD = \
    $(PRRTE_EXTRA_LTLIB) \
    $(prrte_libevent_LIBS) \
    $(prrte_hwloc_LIBS) \
    $(prrte_pmix_LIBS) \
	$(top_builddir)/src/libprrte.la
