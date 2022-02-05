#
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CFLAGS = \
            -DDEFAULT_PARAM_FILE_PATH="\"@AMCA_PARAM_SETS_DIR@\""

dist_prtedata_DATA = \
    help-schizo-ompi.txt

sources = \
          schizo_ompi_component.c \
          schizo_ompi.h \
          schizo_ompi.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_schizo_ompi_DSO
component_noinst =
component_install = mca_schizo_ompi.la
else
component_noinst = libmca_schizo_ompi.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_schizo_ompi_la_SOURCES = $(sources)
mca_schizo_ompi_la_LDFLAGS = -module -avoid-version
mca_schizo_ompi_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_schizo_ompi_la_SOURCES = $(sources)
libmca_schizo_ompi_la_LDFLAGS = -module -avoid-version
