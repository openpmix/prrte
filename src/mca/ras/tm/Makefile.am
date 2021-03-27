#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# The TM plugin does not call any Torque/PBS functions - it therefore
# does NOT need to link against any Torque/PBS libraries

# Use the top-level Makefile.options

dist_prtedata_DATA = help-ras-tm.txt


sources = \
        ras_tm.h \
        ras_tm_component.c \
        ras_tm_module.c


# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_ras_tm_DSO
lib =
lib_sources =
component = mca_ras_tm.la
component_sources = $(sources)
else
lib = libmca_ras_tm.la
lib_sources = $(sources)
component =
component_sources =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_ras_tm_la_SOURCES = $(component_sources)
mca_ras_tm_la_LDFLAGS = -module -avoid-version
mca_ras_tm_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(lib)
libmca_ras_tm_la_SOURCES = $(lib_sources)
libmca_ras_tm_la_LDFLAGS = -module -avoid-version
