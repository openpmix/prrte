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
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = prrte_config.h \
		  constants.h \
		  types.h \
		  frameworks.h \
		  hash_string.h \
		  prefetch.h \
		  prrte_config_top.h \
		  prrte_config_bottom.h \
		  prrte_stdint.h \
		  prrte_stdatomic.h \
		  align.h

nodist_headers = \
		  version.h

EXTRA_DIST = $(headers)

if WANT_INSTALL_HEADERS
prrtedir = $(prrteincludedir)
nobase_dist_prrte_HEADERS = $(headers)
nobase_nodist_prrte_HEADERS = $(nodist_headers)
endif
