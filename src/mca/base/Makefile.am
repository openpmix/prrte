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
# Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
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

dist_prrtedata_DATA = help-prrte-mca-base.txt help-prrte-mca-var.txt

# Source code files

headers = \
        base.h \
        prrte_mca_base_component_repository.h \
        prrte_mca_base_var.h \
	prrte_mca_base_var_enum.h \
        prrte_mca_base_var_group.h \
        prrte_mca_base_vari.h \
	prrte_mca_base_framework.h

# Library

libprrte_mca_base_la_SOURCES = \
        $(headers) \
        prrte_mca_base_close.c \
        prrte_mca_base_component_compare.c \
        prrte_mca_base_component_find.c \
        prrte_mca_base_component_repository.c \
        prrte_mca_base_components_open.c \
        prrte_mca_base_components_close.c \
        prrte_mca_base_components_select.c \
        prrte_mca_base_list.c \
        prrte_mca_base_open.c \
        prrte_mca_base_var.c \
	prrte_mca_base_var_enum.c \
        prrte_mca_base_var_group.c \
	prrte_mca_base_components_register.c \
	prrte_mca_base_framework.c

# Conditionally install the header files

if WANT_INSTALL_HEADERS
prrtedir = $(prrteincludedir)/$(subdir)
prrte_HEADERS = $(headers)
endif
