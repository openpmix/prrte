#
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        errmgr_dvm.h \
        errmgr_dvm_component.c \
        errmgr_dvm.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_errmgr_dvm_DSO
component_noinst =
component_install = mca_errmgr_dvm.la
else
component_noinst = libmca_errmgr_dvm.la
component_install =
endif

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_errmgr_dvm_la_SOURCES = $(sources)
mca_errmgr_dvm_la_LDFLAGS = -module -avoid-version
mca_errmgr_dvm_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_errmgr_dvm_la_SOURCES =$(sources)
libmca_errmgr_dvm_la_LDFLAGS = -module -avoid-version
