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
# Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2019      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_LDFLAGS = $(prte_hwloc_LDFLAGS) $(prte_libevent_LDFLAGS) $(prte_pmix_LDFLAGS)

bin_PROGRAMS = prte-term

prte_term_SOURCES = \
        pterm.c

prte_term_LDADD = \
    $(prte_libevent_LIBS) \
    $(prte_hwloc_LIBS) \
    $(prte_pmix_LIBS) \
	$(top_builddir)/src/libprrte.la

if PRTE_WANT_LEGACY_TOOLS

install-exec-hook:
	(cd $(DESTDIR)$(bindir); rm -f pterm$(EXEEXT); $(LN_S) prte-term$(EXEEXT) pterm$(EXEEXT))

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/pterm$(EXEEXT)

endif
