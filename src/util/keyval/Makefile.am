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
# Copyright (c) 2016      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#


AM_LFLAGS = -Pprrte_util_keyval_yy
LEX_OUTPUT_ROOT = lex.prrte_util_keyval_yy

noinst_LTLIBRARIES = libprrteutilkeyval.la

# Source code files

libprrteutilkeyval_la_SOURCES = \
	keyval_lex.h \
        keyval_lex.l

maintainer-clean-local:
	rm -f keyval_lex.c
