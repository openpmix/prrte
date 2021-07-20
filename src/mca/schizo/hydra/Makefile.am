#
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2021      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CFLAGS = \
            -DDEFAULT_PARAM_FILE_PATH="\"@AMCA_PARAM_SETS_DIR@\""

sources = \
          schizo_hydra_component.c \
          schizo_hydra.h \
          schizo_hydra.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_schizo_hydra_DSO
component_noinst =
component_install = mca_schizo_hydra.la
else
component_noinst = libmca_schizo_hydra.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_schizo_hydra_la_SOURCES = $(sources)
mca_schizo_hydra_la_LDFLAGS = -module -avoid-version
mca_schizo_hydra_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_schizo_hydra_la_SOURCES = $(sources)
libmca_schizo_hydra_la_LDFLAGS = -module -avoid-version
