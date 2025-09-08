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
# Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      NVIDIA Corporation.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2016      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016-2017 IBM Corporation. All rights reserved.
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# Copyright (c) 2023      Jeffrey M. Squyres.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = hostfile

noinst_LTLIBRARIES = libprrteutil.la

AM_CPPFLAGS = $(LTDLINCL)

include $(top_srcdir)/Makefile.prte-rules

EXTRA_DIST = \
        dash_host/help-dash-host.txt \
        hostfile/help-hostfile.txt \
        help-prte-util.txt \
        prte-convert-help.py


# Source code files

headers = \
        attr.h \
        bit_ops.h \
        prte_cmd_line.h \
        crc.h \
        daemon_init.h \
        dash_host/dash_host.h \
        error_strings.h \
        ethtool.h \
        error.h \
        malloc.h \
        name_fns.h \
        nidmap.h \
        numtostr.h \
        proc_info.h \
        session_dir.h \
        stacktrace.h \
        sys_limits.h \
        uri.h

sources = \
        attr.c \
        crc.c \
        daemon_init.c \
        dash_host/dash_host.c \
        error_strings.c \
        ethtool.c \
        error.c \
        malloc.c \
        name_fns.c \
        nidmap.c \
        numtostr.c \
        proc_info.c \
        session_dir.c \
        stacktrace.c \
        sys_limits.c \
        uri.c \
        prte_show_help_content.c

libprrteutil_la_SOURCES = $(headers) $(sources)

libprrteutil_la_LIBADD = \
        hostfile/libprrteutilhostfile.la
libprrteutil_la_DEPENDENCIES = \
        hostfile/libprrteutilhostfile.la

prte_show_help_content.c: prte-convert-help.py
if PRTE_PICKY_COMPILERS
	$(PRTE_V_GEN) $(PYTHON) $(abs_srcdir)/prte-convert-help.py \
		--root $(abs_top_srcdir) \
		--out prte_show_help_content.c \
		--purge
else
	$(PRTE_V_GEN) $(PYTHON) $(abs_srcdir)/prte-convert-help.py \
		--root $(abs_top_srcdir) \
		--out prte_show_help_content.c
endif


# Conditionally install the header files

if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/$(subdir)
prte_HEADERS = $(headers)
endif

clean-local:
	rm -f prte_show_help_content.c prte_show_help_content.lo

MAINTAINERCLEANFILES = \
	prte_show_help_content.c

distclean-local:
	rm -f prte_show_help_content.c
