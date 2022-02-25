#
# Copyright (c) 2007      Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2009      High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
	routed_binomial.h \
	routed_binomial.c \
	routed_binomial_component.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_routed_binomial_DSO
component_noinst =
component_install = prte_mca_routed_binomial.la
else
component_noinst = libprte_mca_routed_binomial.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
prte_mca_routed_binomial_la_SOURCES = $(sources)
prte_mca_routed_binomial_la_LDFLAGS = -module -avoid-version
prte_mca_routed_binomial_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libprte_mca_routed_binomial_la_SOURCES = $(sources)
libprte_mca_routed_binomial_la_LDFLAGS = -module -avoid-version

