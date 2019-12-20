#
# Copyright (c) 2010-2011 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        errmgr_prted.h \
        errmgr_prted_component.c \
        errmgr_prted.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_errmgr_prted_DSO
component_noinst =
component_install = mca_errmgr_prted.la
else
component_noinst = libmca_errmgr_prted.la
component_install =
endif

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_errmgr_prted_la_SOURCES = $(sources)
mca_errmgr_prted_la_LDFLAGS = -module -avoid-version
mca_errmgr_prted_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_errmgr_prted_la_SOURCES =$(sources)
libmca_errmgr_prted_la_LDFLAGS = -module -avoid-version
