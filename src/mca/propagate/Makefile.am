#
# Copyright (c) 2017-2020 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# main library setup
noinst_LTLIBRARIES = libmca_propagate.la
libmca_propagate_la_SOURCES =

# local files
headers = propagate.h

libmca_propagate_la_SOURCES += $(headers)

# Conditionally install the header files
if WANT_INSTALL_HEADERS
prrtedir = $(prrteincludedir)/$(subdir)
nobase_prrte_HEADERS = $(headers)
endif

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
