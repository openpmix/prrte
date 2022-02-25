#
# Copyright (c) 2017-2020 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
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

if MCA_BUILD_prte_propagate_prperror_DSO
component_noinst =
component_install = prte_mca_propagate_prperror.la
else
component_noinst = libprte_mca_propagate_prperror.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
prte_mca_propagate_prperror_la_SOURCES = $(sources)
prte_mca_propagate_prperror_la_LDFLAGS = -module -avoid-version
prte_mca_propagate_prperror_la_LIBADD = $(top_builddir/src/libprrte.la)

noinst_LTLIBRARIES = $(component_noinst)
libprte_mca_propagate_prperror_la_SOURCES =$(sources)
libprte_mca_propagate_prperror_la_LDFLAGS = -module -avoid-version
