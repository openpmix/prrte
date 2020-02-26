#
# Copyright (c) 2016      Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2020 IBM Corporation. All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
          schizo_entry_component.c \
          schizo_entry.h \
          schizo_entry.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_schizo_entry_DSO
component_noinst =
component_install = mca_schizo_entry.la
else
component_noinst = libmca_schizo_entry.la
component_install =
endif

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_schizo_entry_la_SOURCES = $(sources)
mca_schizo_entry_la_LDFLAGS = -module -avoid-version
mca_schizo_entry_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_schizo_entry_la_SOURCES = $(sources)
libmca_schizo_entry_la_LDFLAGS = -module -avoid-version

