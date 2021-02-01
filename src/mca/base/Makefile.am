#
# Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
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
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Need this so that the base knows where to load dynamic components from
# (by default)

AM_CPPFLAGS = \
	$(LTDLINCL)

noinst_LTLIBRARIES = libprrte_mca_base.la

dist_prtedata_DATA = help-prte-mca-base.txt help-prte-mca-var.txt

# Source code files

headers = \
        base.h \
        prte_mca_base_component_repository.h \
        prte_mca_base_var.h \
        prte_mca_base_var_enum.h \
        prte_mca_base_var_group.h \
        prte_mca_base_vari.h \
        prte_mca_base_framework.h \
        prte_mca_base_alias.h

# Library

libprrte_mca_base_la_SOURCES = \
        $(headers) \
        prte_mca_base_close.c \
        prte_mca_base_component_compare.c \
        prte_mca_base_component_find.c \
        prte_mca_base_component_repository.c \
        prte_mca_base_components_open.c \
        prte_mca_base_components_close.c \
        prte_mca_base_components_select.c \
        prte_mca_base_list.c \
        prte_mca_base_open.c \
        prte_mca_base_var.c \
        prte_mca_base_var_enum.c \
        prte_mca_base_var_group.c \
        prte_mca_base_components_register.c \
        prte_mca_base_framework.c \
        prte_mca_base_alias.c

# Conditionally install the header files

if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/$(subdir)
prte_HEADERS = $(headers)
endif
