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
# Copyright (c) 2025-2026 Triad National Security, LLC. All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        ras_flux.h \
        ras_flux_component.c \
        ras_flux_module.c

EXTRA_DIST = help-ras-flux.txt

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_ras_flux_DSO
component_noinst =
component_install = prte_mca_ras_flux.la
else
component_noinst = libprtemca_ras_flux.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
prte_mca_ras_flux_la_SOURCES = $(sources)
prte_mca_ras_flux_la_CPPFLAGS = $(ras_flux_CPPFLAGS) $(ras_flux_jansson_CPPFLAGS)
prte_mca_ras_flux_la_LDFLAGS = -module -avoid-version $(ras_flux_LDFLAGS) $(ras_flux_jansson_LDFLAGS)
prte_mca_ras_flux_la_LIBADD = $(ras_flux_LIBS) $(ras_flux_jansson_LIBS)\
    $(top_builddir)/src/lib@PRTE_BINARY_PREFIX@prrte.la

noinst_LTLIBRARIES = $(component_noinst)
libprtemca_ras_flux_la_SOURCES =$(sources)
libprtemca_ras_flux_la_CPPFLAGS = $(ras_flux_CPPFLAGS) $(ras_flux_jansson_CPPFLAGS)
libprtemca_ras_flux_la_LDFLAGS = -module -avoid-version $(ras_flux_LDFLAGS) $(ras_flux_jansson_LDFLAGS)
libprtemca_ras_flux_la_LIBADD = $(ras_flux_LIBS) $(ras_flux_jansson_LIBS)
