#
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# main library setup
noinst_LTLIBRARIES = libmca_prteif.la
libmca_prteif_la_SOURCES =

# local files
headers = prteif.h
libmca_prteif_la_SOURCES += $(headers)

# Conditionally install the header files
if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/src/mca/prteif
nobase_prte_HEADERS = $(headers)
endif

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
