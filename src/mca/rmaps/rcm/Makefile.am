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

dist_prtedata_DATA = help-rmaps-rcm.txt

sources = \
        rmaps_rcm.c \
        rmaps_rcm.h \
        rmaps_rcm_component.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_rmaps_rcm_DSO
component_noinst =
component_install = mca_rmaps_rcm.la
else
component_noinst = libmca_rmaps_rcm.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_rmaps_rcm_la_SOURCES = $(sources)
mca_rmaps_rcm_la_LDFLAGS = -module -avoid-version
mca_rmaps_rcm_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_rmaps_rcm_la_SOURCES =$(sources)
libmca_rmaps_rcm_la_LDFLAGS = -module -avoid-version
