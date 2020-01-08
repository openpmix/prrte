#
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
          schizo_prrte_component.c \
          schizo_prrte.h \
          schizo_prrte.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_schizo_prrte_DSO
component_noinst =
component_install = mca_schizo_prrte.la
else
component_noinst = libmca_schizo_prrte.la
component_install =
endif

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_schizo_prrte_la_SOURCES = $(sources)
mca_schizo_prrte_la_LDFLAGS = -module -avoid-version
mca_schizo_prrte_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_schizo_prrte_la_SOURCES = $(sources)
libmca_schizo_prrte_la_LDFLAGS = -module -avoid-version
