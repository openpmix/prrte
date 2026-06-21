# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

SUBDIRS = topologies offline unit attachtest

# These are PMIx client test programs used for manual and integration
# testing under prte/prun. They require a running DVM and job environment
# to execute meaningfully, so they are built but are deliberately not
# part of TESTS - "make check" must not attempt to run them.
#
# Historically these were built standalone via the installed "pcc" wrapper
# compiler and were never subject to the project's picky/-Werror CFLAGS.
# Override CFLAGS here (rather than AM_CFLAGS, which is overridden by the
# inherited CFLAGS in automake's compile rule) so picky warnings don't
# break the build; chasing them across every test program is out of scope.
CFLAGS = -g

EXTRA_DIST = \
	test.h \
	cycle_forever.sh

noinst_PROGRAMS = \
	double-get \
	get-nofence \
	get-immediate \
	abort \
	simple_spawn \
	hello \
	connect \
	loop_child \
	loop_spawn \
	reinit \
	cmspawn \
	qspawn \
	iostress \
	filegen \
	clichk \
	spawn_timeout

double_get_SOURCES = double-get.c
get_nofence_SOURCES = get-nofence.c
get_immediate_SOURCES = get-immediate.c
abort_SOURCES = abort.c
simple_spawn_SOURCES = simple_spawn.c
hello_SOURCES = hello.c
connect_SOURCES = connect.c
loop_child_SOURCES = loop_child.c
loop_spawn_SOURCES = loop_spawn.c
reinit_SOURCES = reinit.c
cmspawn_SOURCES = cmspawn.c
qspawn_SOURCES = qspawn.c
iostress_SOURCES = iostress.c
filegen_SOURCES = filegen.c
clichk_SOURCES = clichk.c
spawn_timeout_SOURCES = spawn_timeout.c

# chkfs relies on Linux-only APIs (mntent.h, /proc/mounts) and will not
# build on other platforms (e.g., macOS, BSD)
if PRTE_OS_LINUX
noinst_PROGRAMS += chkfs
chkfs_SOURCES = chkfs.c
endif
