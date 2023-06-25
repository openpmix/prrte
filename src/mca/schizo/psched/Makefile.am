#
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CFLAGS = \
    -DPRTE_GREEK_VERSION="\"@PRTE_GREEK_VERSION@\""

dist_prtedata_DATA = \
    help-schizo-psched.txt

sources = \
          schizo_psched_component.c \
          schizo_psched.h \
          schizo_psched.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_schizo_psched_DSO
component_noinst =
component_install = prte_mca_schizo_psched.la
else
component_noinst = libprtemca_schizo_psched.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
prte_mca_schizo_psched_la_SOURCES = $(sources)
prte_mca_schizo_psched_la_LDFLAGS = -module -avoid-version
prte_mca_schizo_psched_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libprtemca_schizo_psched_la_SOURCES = $(sources)
libprtemca_schizo_psched_la_LDFLAGS = -module -avoid-version
