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
# Copyright (c) 2006-2007 Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2011-2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013      Mellanox Technologies, Inc.  All rights reserved.
# Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Use the PRRTE-provided wrapper compiler

CC = pcc

# Using -g is not necessary, but it is helpful for example programs,
# especially if users want to examine them with debuggers.

CFLAGS = -g

# Example programs to build

EXAMPLES = \
	client \
	client2 \
	debugger/direct \
	debugger/direct-multi \
	debugger/indirect \
	debugger/indirect-multi \
	debugger/attach \
	debugger/daemon \
	debugger/hello \
	debugger/stdincheck \
	dmodex \
	dynamic \
	fault \
	pub \
	tool \
	alloc \
	probe \
	target \
	hello \
	log \
	bad_exit \
	jctrl \
	launcher \
	showkeys \
	legacy \
	colocate \
	pset \
	nodeid \
	client-threaded

all: $(EXAMPLES)

# The usual "clean" target

clean:
	rm -f $(EXAMPLES) *~ *.o
