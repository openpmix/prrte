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


AM_LFLAGS = -Pprrte_util_hostfile_
LEX_OUTPUT_ROOT = lex.prrte_util_hostfile_

noinst_LTLIBRARIES = libprrteutilhostfile.la

# nodist_man_MANS = prrte_hosts.7

# We are using $(am_dirstamp) instead of creating our own dirstamp since there
# is src code in util/hostfile directory is created.  The automake process
# creates the $(am_dirstamp), we found the use of this in the generated Makefile
# in the util/Makefile
$(nodist_man_MANS): $(am__dirstamp) $(top_builddir)/src/include/prrte_config.h

# EXTRA_DIST = $(nodist_man_MANS:.7=.7in)

# Source code files

libprrteutilhostfile_la_SOURCES = \
	hostfile_lex.h \
        hostfile_lex.l \
        hostfile.c

maintainer-clean-local:
	rm -f hostfile_lex.c
