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
# Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

EXTRA_DIST = \
        help-plm-tm.txt

sources = \
        plm_tm.h \
        plm_tm_component.c \
        plm_tm_module.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds). The component links nothing of TM's.

if MCA_BUILD_prte_plm_tm_DSO
lib =
lib_sources =
component = prte_mca_plm_tm.la
component_sources = $(sources)
else
lib = libprtemca_plm_tm.la
lib_sources = $(sources)
component =
component_sources =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component)
prte_mca_plm_tm_la_SOURCES = $(component_sources)
prte_mca_plm_tm_la_LDFLAGS = -module -avoid-version
prte_mca_plm_tm_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(lib)
libprtemca_plm_tm_la_SOURCES = $(lib_sources)
libprtemca_plm_tm_la_LDFLAGS = -module -avoid-version

# The helper the component runs, and the only thing that links the TM
# library. Internal to PRRTE, so in libexecdir, where a user browsing
# bin will not find it - the component looks for it in
# $(libexecdir)/prte (PRTE_PLM_TM_HELPER_DIR).
#
# Nothing else may be linked into it: configure's LIBS carries PMIx,
# libevent and hwloc, and automake appends LIBS to every link in this
# Makefile, so it is emptied here. The component loses nothing by that:
# what it needs comes from libprrte.la.
LIBS =
prtelibexecdir = $(libexecdir)/prte
prtelibexec_PROGRAMS = prte_tm_spawn
prte_tm_spawn_SOURCES = prte_tm_spawn.c
prte_tm_spawn_LDFLAGS = $(plm_tm_helper_LDFLAGS) $(plm_tm_helper_RPATH)
prte_tm_spawn_LDADD = $(plm_tm_helper_LIBS)
if PRTE_PLM_TM_HAVE_TM
prte_tm_spawn_CPPFLAGS = $(plm_tm_helper_CPPFLAGS) -DPRTE_PLM_TM_HAVE_TM=1
else
prte_tm_spawn_SOURCES += testbuild_tm.h testbuild_tm.c
prte_tm_spawn_CPPFLAGS = -DPRTE_PLM_TM_HAVE_TM=0
endif
