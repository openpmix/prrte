#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2015      Los Alamos National Security, LLC.  All rights
#                         reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2019      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
# Copyright (c) 2023      Triad National Security, LLC. All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        plm_pals.h \
        plm_pals_component.c \
        plm_pals_module.c

EXTRA_DIST = help-plm-pals.txt

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_plm_pals_DSO
component_noinst =
component_install = prte_mca_plm_pals.la
else
component_noinst = libprtemca_plm_pals.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
prte_mca_plm_pals_la_SOURCES = $(sources)
prte_mca_plm_pals_la_CPPFLAGS = $(plm_pals_CPPFLAGS)
prte_mca_plm_pals_la_LDFLAGS = -module -avoid-version $(plm_pals_LDFLAGS)
prte_mca_plm_pals_la_LIBADD = $(plm_pals_LIBS) \
    $(top_builddir)/src/libprrte.la   \
    $(PRTE_TOP_BUILDDIR)/src/mca/common/pals/libmca_common_pals.la

noinst_LTLIBRARIES = $(component_noinst)
libprtemca_plm_pals_la_SOURCES =$(sources)
libprtemca_plm_pals_la_CPPFLAGS = $(plm_pals_CPPFLAGS)
libprtemca_plm_pals_la_LDFLAGS = -module -avoid-version $(plm_pals_LDFLAGS)
libprtemca_plm_pals_la_LIBADD = $(plm_pals_LIBS)
