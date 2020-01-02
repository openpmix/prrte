#
# Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        reachable_netlink.h \
        reachable_netlink_component.c \
        reachable_netlink_module.c \
        libnl3_utils.h \
        libnl_utils.h \
        reachable_netlink_utils_common.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prrte_reachable_netlink_DSO
component_noinst =
component_install = mca_reachable_netlink.la
else
component_noinst = libmca_reachable_netlink.la
component_install =
endif

AM_CPPFLAGS = \
        $(prrte_reachable_netlink_CPPFLAGS) \
        -DPRRTE_HAVE_LIBNL3=$(PRRTE_HAVE_LIBNL3)

mcacomponentdir = $(prrtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_reachable_netlink_la_SOURCES = $(sources)
mca_reachable_netlink_la_LDFLAGS = -module -avoid-version
mca_reachable_netlink_la_LIBADD = $(top_builddir)/src/libprrte.la \
	$(prrte_reachable_netlink_LIBS)

noinst_LTLIBRARIES = $(component_noinst)
libmca_reachable_netlink_la_SOURCES =$(sources)
libmca_reachable_netlink_la_LDFLAGS = -module -avoid-version
libmca_reachable_netlink_la_LIBADD = $(prrte_reachable_netlink_LIBS)
