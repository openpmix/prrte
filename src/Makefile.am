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
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2015      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2019      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = \
	docs \
	etc \
	util \
	$(MCA_prte_FRAMEWORKS_SUBDIRS) \
	$(MCA_prte_FRAMEWORK_COMPONENT_STATIC_SUBDIRS) \
	. \
	$(MCA_prte_FRAMEWORK_COMPONENT_DSO_SUBDIRS)

DIST_SUBDIRS = \
	docs \
	etc \
	util \
	$(MCA_prte_FRAMEWORKS_SUBDIRS) \
	$(MCA_prte_FRAMEWORK_COMPONENT_ALL_SUBDIRS)

# Build the main PRTE library

lib_LTLIBRARIES = libprrte.la
libprrte_la_SOURCES =

libprrte_la_LIBADD = \
	util/libprrteutil.la \
	$(MCA_prte_FRAMEWORK_LIBS)
libprrte_la_DEPENDENCIES = \
	util/libprrteutil.la \
	$(MCA_prte_FRAMEWORK_LIBS)
libprrte_la_LDFLAGS = -version-info $(libprrte_so_version)
libprrte_la_CPPFLAGS =

# included subdirectory Makefile.am's and appended-to variables.
#
# PRRTE's headers are not installed anywhere -- see
# config/prte_configure_options.m4 for why --with-devel-headers is gone.
# $(headers) stays because listing it in libprrte_la_SOURCES is what gets
# these files into the tarball and tracked as build dependencies.  There was
# also a $(nodist_headers), holding prte_config.h and version.h; with the
# install gone nothing reads it, and those two are already handled by
# AC_CONFIG_HEADERS and AC_CONFIG_FILES.
headers =
noinst_LTLIBRARIES =
EXTRA_DIST =
libprrte_la_SOURCES += $(headers)

include include/Makefile.am
include runtime/Makefile.am
include tools/Makefile.am
include prted/Makefile.am
include pmix/Makefile.am
include event/Makefile.am
include hwloc/Makefile.am
include rml/Makefile.am
