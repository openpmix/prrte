# -*- makefile -*-
#
# Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2009-2014 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012      Los Alamos National Security, LLC.
#                         All rights reserved.
# Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# This makefile.am does not stand on its own - it is included from src/Makefile.am

dist_prrtedata_DATA += runtime/help-prrte-runtime.txt

headers += \
        runtime/runtime.h \
	runtime/prrte_locks.h \
        runtime/prrte_globals.h \
        runtime/prrte_quit.h \
        runtime/runtime_internals.h \
        runtime/data_type_support/prrte_dt_support.h \
	runtime/prrte_wait.h \
        runtime/prrte_data_server.h \
        runtime/prrte_progress_threads.h

libprrte_la_SOURCES += \
        runtime/prrte_finalize.c \
        runtime/prrte_init.c \
        runtime/prrte_locks.c \
        runtime/prrte_globals.c \
        runtime/prrte_quit.c \
        runtime/data_type_support/prrte_dt_compare_fns.c \
        runtime/data_type_support/prrte_dt_copy_fns.c \
        runtime/data_type_support/prrte_dt_print_fns.c \
        runtime/data_type_support/prrte_dt_packing_fns.c \
        runtime/data_type_support/prrte_dt_unpacking_fns.c \
        runtime/prrte_mca_params.c \
        runtime/prrte_wait.c \
        runtime/prrte_data_server.c \
        runtime/prrte_progress_threads.c
