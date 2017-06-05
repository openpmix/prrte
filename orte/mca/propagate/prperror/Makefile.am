#
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        propagate_prperror.h \
        propagate_prperror_component.c \
        propagate_prperror.c 
# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_orte_propagate_prperror_DSO
component_noinst =
component_install = mca_propagate_prperror.la
else
component_noinst = libmca_propagate_prperror.la
component_install =
endif

mcacomponentdir = $(ortelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_propagate_prperror_la_SOURCES = $(sources)
mca_propagate_prperror_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(component_noinst)
libmca_propagate_prperror_la_SOURCES =$(sources)
libmca_propagate_prperror_la_LDFLAGS = -module -avoid-version
