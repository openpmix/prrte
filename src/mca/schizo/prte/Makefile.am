#
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2022      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

dist_prtedata_DATA = \
    help-schizo-prte.txt \
    help-schizo-prted.txt \
    help-schizo-prun.txt \
    help-schizo-pterm.txt \
    help-schizo-pinfo.txt \
    help-schizo-prterun.txt

sources = \
          schizo_prte_component.c \
          schizo_prte.h \
          schizo_prte.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_schizo_prte_DSO
component_noinst =
component_install = mca_schizo_prte.la
else
component_noinst = libmca_schizo_prte.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_schizo_prte_la_SOURCES = $(sources)
mca_schizo_prte_la_LDFLAGS = -module -avoid-version
mca_schizo_prte_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_schizo_prte_la_SOURCES = $(sources)
libmca_schizo_prte_la_LDFLAGS = -module -avoid-version
