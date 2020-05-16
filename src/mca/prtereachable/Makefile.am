#
# Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(LTDLINCL) $(prtereachable_CPPFLAGS)

# main library setup
noinst_LTLIBRARIES = libmca_prtereachable.la
libmca_prtereachable_la_SOURCES =

# pkgdata setup
dist_prtedata_DATA =

# local files
headers = prtereachable.h
libmca_prtereachable_la_SOURCES += $(headers)

# Conditionally install the header files
if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/$(subdir)
nobase_prte_HEADERS = $(headers)
endif

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
