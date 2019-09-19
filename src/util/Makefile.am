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
# Copyright (c) 2007-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      NVIDIA Corporation.  All rights reserved.
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2016      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016-2017 IBM Corporation. All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = keyval hostfile

dist_prrtedata_DATA = help-prrte-util.txt \
                      help-regex.txt \
                      hostfile/help-hostfile.txt \
                      dash_host/help-dash-host.txt

AM_LFLAGS = -Pprrte_show_help_yy
LEX_OUTPUT_ROOT = lex.prrte_show_help_yy

noinst_LTLIBRARIES = libprrteutil.la

AM_CPPFLAGS = $(LTDLINCL)

# Source code files

headers = \
        alfg.h \
        arch.h \
        argv.h \
        attr.h \
        basename.h \
	bipartite_graph.h \
	bipartite_graph_internal.h \
        bit_ops.h \
        cmd_line.h \
        context_fns.h \
        crc.h \
        daemon_init.h \
        dash_host/dash_host.h \
        error_strings.h \
	ethtool.h \
        error.h \
        fd.h \
        few.h \
        if.h \
        keyval_parse.h \
        listener.h \
        malloc.h \
        name_fns.h \
        net.h \
        nidmap.h \
        numtostr.h \
        os_dirpath.h \
        os_path.h \
        output.h \
        parse_options.h \
        path.h \
        printf.h \
        proc_info.h \
        prrte_environ.h \
        prrte_getcwd.h \
        prrte_pty.h \
        qsort.h \
        session_dir.h \
        show_help_lex.h \
        show_help.h \
        stacktrace.h \
        string_copy.h \
        sys_limits.h \
        uri.h

libprrteutil_la_SOURCES = \
        $(headers) \
        alfg.c \
        arch.c \
        argv.c \
        attr.c \
        basename.c \
        bipartite_graph.c \
        cmd_line.c \
        context_fns.c \
        crc.c \
        daemon_init.c \
        dash_host/dash_host.c \
        error_strings.c \
        ethtool.c \
        error.c \
        fd.c \
        few.c \
        if.c \
        keyval_parse.c \
        listener.c \
        malloc.c \
        name_fns.c \
        net.c \
        nidmap.c \
        numtostr.c \
        os_dirpath.c \
        os_path.c \
        output.c \
        parse_options.c \
        path.c \
        printf.c \
        proc_info.c \
        prrte_environ.c \
        prrte_getcwd.c \
        prrte_pty.c \
        qsort.c \
        session_dir.c \
        show_help.c \
        show_help_lex.l \
        stacktrace.c \
        string_copy.c \
        sys_limits.c \
        uri.c

libprrteutil_la_LIBADD = \
        keyval/libprrteutilkeyval.la \
	hostfile/libprrteutilhostfile.la
libprrteutil_la_DEPENDENCIES = \
        keyval/libprrteutilkeyval.la \
	hostfile/libprrteutilhostfile.la

# Conditionally install the header files

if WANT_INSTALL_HEADERS
prrtedir = $(prrteincludedir)/$(subdir)
prrte_HEADERS = $(headers)
endif

maintainer-clean-local:
	rm -f show_help_lex.c
