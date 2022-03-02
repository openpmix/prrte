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
# Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = \
	include \
	etc \
	util \
	mca/base \
	$(MCA_prte_FRAMEWORKS_SUBDIRS) \
	$(MCA_prte_FRAMEWORK_COMPONENT_STATIC_SUBDIRS) \
	. \
	$(MCA_prte_FRAMEWORK_COMPONENT_DSO_SUBDIRS)

DIST_SUBDIRS = \
	include \
	etc \
	util \
	mca/base \
	$(MCA_prte_FRAMEWORKS_SUBDIRS) \
	$(MCA_prte_FRAMEWORK_COMPONENT_ALL_SUBDIRS)

# Build the main PRTE library

lib_LTLIBRARIES = libprrte.la
libprrte_la_SOURCES =

libprrte_la_LIBADD = \
	mca/base/libprrte_mca_base.la \
	util/libprrteutil.la \
	$(MCA_prte_FRAMEWORK_LIBS)
libprrte_la_DEPENDENCIES = \
	mca/base/libprrte_mca_base.la \
	util/libprrteutil.la \
	$(MCA_prte_FRAMEWORK_LIBS)
libprrte_la_LDFLAGS = -version-info $(libprrte_so_version)
libprrte_la_CPPFLAGS =

# included subdirectory Makefile.am's and appended-to variables
headers =
noinst_LTLIBRARIES =
dist_prtedata_DATA =
libprrte_la_SOURCES += $(headers)

# Conditionally install the header files

if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/$(subdir)
nobase_prte_HEADERS = $(headers)
endif

# setup to catch extra files to be included in tarballs
EXTRA_DIST = Doxyfile

include runtime/Makefile.am
include tools/Makefile.am
include prted/Makefile.am
include mca/Makefile.am
include pmix/Makefile.am
include event/Makefile.am
include hwloc/Makefile.am
include rml/Makefile.am
