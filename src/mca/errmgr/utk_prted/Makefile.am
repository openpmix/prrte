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
        errmgr_utk_prted.h \
        errmgr_utk_prted_component.c \
        errmgr_utk_prted.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_errmgr_utk_prted_DSO
component_noinst =
component_install = mca_errmgr_utk_prted.la
else
component_noinst = libmca_errmgr_utk_prted.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_errmgr_utk_prted_la_SOURCES = $(sources)
mca_errmgr_utk_prted_la_LDFLAGS = -module -avoid-version
mca_errmgr_utk_prted_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_errmgr_utk_prted_la_SOURCES =$(sources)
libmca_errmgr_utk_prted_la_LDFLAGS = -module -avoid-version
