#
# Copyright (c) 2004-2010 The Trustees of Indiana University.
#                         All rights reserved.
# Copyright (c) 2014-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(prtecompress_zlib_CPPFLAGS)

sources = \
        prtecompress_zlib.h \
        prtecompress_zlib_component.c \
        prtecompress_zlib.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_prtecompress_zlib_DSO
component_noinst =
component_install = mca_prtecompress_zlib.la
else
component_noinst = libmca_prtecompress_zlib.la
component_install =
endif

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_prtecompress_zlib_la_SOURCES = $(sources)
mca_prtecompress_zlib_la_LDFLAGS = -module -avoid-version $(prtecompress_zlib_LDFLAGS)
mca_prtecompress_zlib_la_LIBADD = $(top_builddir)/src/libprrte.la $(prtecompress_zlib_LIBS)

noinst_LTLIBRARIES = $(component_noinst)
libmca_prtecompress_zlib_la_SOURCES = $(sources)
libmca_prtecompress_zlib_la_LDFLAGS = -module -avoid-version $(prtecompress_zlib_LDFLAGS)
libmca_prtecompress_zlib_la_LIBADD = $(prtecompress_zlib_LIBS)
