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
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = constants.h \
		  types.h \
		  frameworks.h \
		  hash_string.h \
		  prefetch.h \
		  prte_config_top.h \
		  prte_config_bottom.h \
		  prte_portable_platform.h \
		  prte_stdint.h \
		  prte_stdatomic.h \
		  prte_socket_errno.h \
		  align.h

nodist_headers = \
		  prte_config.h \
		  version.h

EXTRA_DIST = $(headers)

if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)
nobase_dist_prte_HEADERS = $(headers)
nobase_nodist_prte_HEADERS = $(nodist_headers)
endif
