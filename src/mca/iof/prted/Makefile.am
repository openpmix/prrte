#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_iof_prted_DSO
component_noinst =
component_install = mca_iof_prted.la
else
component_noinst = libmca_iof_prted.la
component_install =
endif

prted_SOURCES = \
    iof_prted.c \
    iof_prted.h \
    iof_prted_component.c \
    iof_prted_read.c \
    iof_prted_receive.c

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_iof_prted_la_SOURCES = $(prted_SOURCES)
mca_iof_prted_la_LDFLAGS = -module -avoid-version
mca_iof_prted_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_iof_prted_la_SOURCES = $(prted_SOURCES)
libmca_iof_prted_la_LIBADD =
libmca_iof_prted_la_LDFLAGS = -module -avoid-version
