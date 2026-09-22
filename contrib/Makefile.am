#
# Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2010      IBM Corporation.  All rights reserved.
# Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
# Copyright (c) 2013-2018 Los Alamos National Security, Inc.  All rights
#                         reserved.
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

amca_paramdir = $(AMCA_PARAM_SETS_DIR)
dist_amca_param_DATA = amca-param-sets/example.conf

EXTRA_DIST = \
	dist/make_dist_tarball \
	dist/make-authors.pl \
	dist/linux/prrte.spec \
	platform/optimized \
	$(MULTINODE_HARNESS)

# The multi-node test harness ships in the tarball, because the question it
# answers -- "does this PRRTE work on my machines" -- is asked by people who
# installed from a tarball at least as often as by people with a git clone.
# run-tests.sh drives it, cluster-setup.sh prepares a real cluster for it
# (see docs/testing/cluster.rst), and the .c files are the helper clients
# that cluster-setup.sh compiles: bare PMIx programs that put the runtime in
# states no ordinary application reaches.  scaletest.c lives in scaling/
# because it is a measurement rather than a test, and cluster-sweep.sh is
# what drives it across a real allocation.
#
# The container plumbing (Dockerfile, docker-compose.yml, build.sh) comes
# too: a tarball unpacked beside a Docker daemon can then run the container
# harness as well, and shipping half a directory is the kind of thing that
# is discovered by somebody following the docs and finding a file missing.
MULTINODE_HARNESS = \
	dockerswarm/AGENTS.md \
	dockerswarm/Dockerfile \
	dockerswarm/build.sh \
	dockerswarm/cluster-setup.sh \
	dockerswarm/connector.c \
	dockerswarm/dataserver.c \
	dockerswarm/devinfo.c \
	dockerswarm/docker-compose.yml \
	dockerswarm/elastic.c \
	dockerswarm/envspawn.c \
	dockerswarm/faulty.c \
	dockerswarm/fencer.c \
	dockerswarm/groupcon.c \
	dockerswarm/groupinv.c \
	dockerswarm/jobinfo.c \
	dockerswarm/mpinoop.c \
	dockerswarm/peerinfo.c \
	dockerswarm/pmixloop.c \
	dockerswarm/proctable.c \
	dockerswarm/run-tests.sh \
	dockerswarm/scaletest.sh \
	dockerswarm/slotinfo.c \
	dockerswarm/slowcat.c \
	dockerswarm/spawnloop.c \
	dockerswarm/spawnrepro.c \
	scaling/README.md \
	scaling/cluster-sweep.sh \
	scaling/scaletest.c
