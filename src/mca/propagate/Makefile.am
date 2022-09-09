#
# Copyright (c) 2017-2020 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# main library setup
noinst_LTLIBRARIES = libprtemca_propagate.la
libprtemca_propagate_la_SOURCES =

# local files
headers = propagate.h

libprtemca_propagate_la_SOURCES += $(headers)

# Conditionally install the header files
prtedir = $(prteincludedir)/$(subdir)
nobase_prte_HEADERS = $(headers)

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
