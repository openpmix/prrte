# -*- makefile -*-
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
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# This makefile.am does not stand on its own - it is included from src/Makefile.am

include $(top_srcdir)/Makefile.prte-rules

headers += \
        include/constants.h \
        include/types.h \
        include/prte_frameworks.h \
        include/prte_config_top.h \
        include/prte_config_bottom.h \
        include/prte_portable_platform.h \
        include/prte_portable_platform_real.h \
        include/prte_stdint.h \
        include/prte_stdatomic.h \
        include/prte_socket_errno.h

# prte_config.h and version.h are deliberately absent from the list above.
# They are generated into the build tree, and AC_CONFIG_HEADERS /
# AC_CONFIG_FILES already own them.  They used to be collected into a
# nodist_headers variable that no _HEADERS variable ever referenced -- so
# they were never installed, which is one of the reasons the
# --with-devel-headers option that would have installed them produced a
# tree that could not be compiled.  That option is gone; see
# config/prte_configure_options.m4.

libprrte_la_SOURCES += \
        $(headers) \
        include/prte_frameworks.c
