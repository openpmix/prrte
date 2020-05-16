#
# Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

include $(top_srcdir)/Makefile.prte-rules

# main library setup
noinst_LTLIBRARIES = libmca_filem.la
libmca_filem_la_SOURCES =

# local files
headers = filem.h
libmca_filem_la_SOURCES += $(headers)

# Manual pages
MD_FILES = prte_filem.7.md
man_pages_from_md = $(MD_FILES:.7.md=.7)

EXTRA_DIST = $(MD_FILES) $(man_pages_from_md)

if PRTE_ENABLE_MAN_PAGES
man_MANS = $(man_pages_from_md)
endif

# Conditionally install the header files
if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/$(subdir)
nobase_prte_HEADERS = $(headers)
endif

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
