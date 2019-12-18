#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2010-2014 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CFLAGS = \
            -DPRRTE_CONFIGURE_USER="\"@PRRTE_CONFIGURE_USER@\"" \
            -DPRRTE_CONFIGURE_HOST="\"@PRRTE_CONFIGURE_HOST@\"" \
            -DPRRTE_CONFIGURE_DATE="\"@PRRTE_CONFIGURE_DATE@\"" \
            -DPRRTE_BUILD_USER="\"$$USER\"" \
            -DPRRTE_BUILD_HOST="\"$${HOSTNAME:-`(hostname || uname -n) | sed 1q`}\"" \
            -DPRRTE_BUILD_DATE="\"`date`\"" \
            -DPRRTE_BUILD_CFLAGS="\"@CFLAGS@\"" \
            -DPRRTE_BUILD_CPPFLAGS="\"@CPPFLAGS@\"" \
            -DPRRTE_BUILD_CXXFLAGS="\"@CXXFLAGS@\"" \
            -DPRRTE_BUILD_CXXCPPFLAGS="\"@CXXCPPFLAGS@\"" \
            -DPRRTE_BUILD_FFLAGS="\"@FFLAGS@\"" \
            -DPRRTE_BUILD_FCFLAGS="\"@FCFLAGS@\"" \
            -DPRRTE_BUILD_LDFLAGS="\"@LDFLAGS@\"" \
            -DPRRTE_BUILD_LIBS="\"@LIBS@\"" \
            -DPRRTE_CC_ABSOLUTE="\"@PRRTE_CC_ABSOLUTE@\"" \
            -DPRRTE_CXX_ABSOLUTE="\"@PRRTE_CXX_ABSOLUTE@\"" \
            -DPRRTE_WRAPPER_EXTRA_CFLAGS="\"@PRRTE_WRAPPER_EXTRA_CFLAGS@\"" \
            -DPRRTE_WRAPPER_EXTRA_LDFLAGS="\"@PRRTE_WRAPPER_EXTRA_LDFLAGS@\"" \
            -DPRRTE_WRAPPER_EXTRA_LIBS="\"@PRRTE_WRAPPER_EXTRA_LIBS@\""

include $(top_srcdir)/Makefile.ompi-rules

man_pages = prte_info.1
EXTRA_DIST = $(man_pages:.1=.1in)

bin_PROGRAMS = prte_info

dist_prrtedata_DATA = help-prrte-info.txt

nodist_man_MANS = $(man_pages)

# Ensure that the man pages are rebuilt if the prrte_config.h file
# changes; a "good enough" way to know if configure was run again (and
# therefore the release date or version may have changed)
$(nodist_man_MANS): $(top_builddir)/src/include/prrte_config.h

prte_info_SOURCES = \
        pinfo.h \
        prte_info.c \
        output.c \
        param.c \
        components.c \
        version.c

prte_info_LDADD = \
    $(PRRTE_EXTRA_LTLIB) \
	$(top_builddir)/src/libprrte.la

clean-local:
	test -z "$(PRRTE_CXX_TEMPLATE_REPOSITORY)" || rm -rf $(PRRTE_CXX_TEMPLATE_REPOSITORY)

distclean-local:
	rm -f $(man_pages)
