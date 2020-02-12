#
# Copyright (c) 2017-2020 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        errmgr_detector.h \
        errmgr_detector_component.c \
        errmgr_detector.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_errmgr_detector_DSO
component_noinst =
component_install = mca_errmgr_detector.la
else
component_noinst = libmca_errmgr_detector.la
component_install =
endif

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_errmgr_detector_la_SOURCES = $(sources)
mca_errmgr_detector_la_LDFLAGS = -module -avoid-version
mca_errmgr_detector_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_errmgr_detector_la_SOURCES =$(sources)
libmca_errmgr_detector_la_LDFLAGS = -module -avoid-version
