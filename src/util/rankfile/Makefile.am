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
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#


AM_LFLAGS = -Pprte_util_rankfile_
LEX_OUTPUT_ROOT = lex.prte_util_rankfile_
# we do NOT want picky compilers down here
CFLAGS = $(PRTE_CFLAGS_BEFORE_PICKY)

noinst_LTLIBRARIES = libprrteutilrankfile.la

# Source code files

libprrteutilrankfile_la_SOURCES = \
	rankfile_lex.h \
        rankfile_lex.l \
        rankfile.h \
        rankfile.c

maintainer-clean-local:
	rm -f rankfile_lex.c
