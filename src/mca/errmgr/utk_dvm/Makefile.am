#
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2022      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        errmgr_utk_dvm.h \
        errmgr_utk_dvm_component.c \
        errmgr_utk_dvm.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_errmgr_utk_dvm_DSO
component_noinst =
component_install = mca_errmgr_utk_dvm.la
else
component_noinst = libmca_errmgr_utk_dvm.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_errmgr_utk_dvm_la_SOURCES = $(sources)
mca_errmgr_utk_dvm_la_LDFLAGS = -module -avoid-version
mca_errmgr_utk_dvm_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_errmgr_utk_dvm_la_SOURCES =$(sources)
libmca_errmgr_utk_dvm_la_LDFLAGS = -module -avoid-version
