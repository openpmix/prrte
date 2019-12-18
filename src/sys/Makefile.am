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
# Copyright (c) 2011      Sandia National Laboratories. All rights reserved.
# Copyright (c) 2016-2018 Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2017      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# This makefile.am does not stand on its own - it is included from src/Makefile.am

headers += \
	sys/architecture.h \
	sys/atomic.h \
	sys/atomic_stdc.h \
	sys/atomic_impl.h \
	sys/timer.h \
    sys/cma.h

include sys/x86_64/Makefile.am
include sys/arm/Makefile.am
include sys/arm64/Makefile.am
include sys/ia32/Makefile.am
include sys/powerpc/Makefile.am
include sys/sparcv9/Makefile.am
include sys/sync_builtin/Makefile.am
include sys/gcc_builtin/Makefile.am
