#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved.
#
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

dist_prtedata_DATA = help-rmaps-resilient.txt

sources = \
        rmaps_resilient.c \
        rmaps_resilient.h \
        rmaps_resilient_component.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_rmaps_resilient_DSO
component_noinst =
component_install = mca_rmaps_resilient.la
else
component_noinst = libmca_rmaps_resilient.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_rmaps_resilient_la_SOURCES = $(sources)
mca_rmaps_resilient_la_LDFLAGS = -module -avoid-version
mca_rmaps_resilient_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_rmaps_resilient_la_SOURCES =$(sources)
libmca_rmaps_resilient_la_LDFLAGS = -module -avoid-version
