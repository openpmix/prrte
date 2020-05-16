# -*- shell-script -*-
#
# Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2015-2016 Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2017      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_prte_prtereachable_netlink_CONFIG([action-if-can-compile],
#                                   [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_prtereachable_netlink_CONFIG],[
    AC_CONFIG_FILES([src/mca/prtereachable/netlink/Makefile])

    PRTE_VAR_SCOPE_PUSH([prte_reachable_netlink_happy])

    prte_reachable_netlink_happy=1
    AC_CHECK_HEADER([linux/netlink.h], [],
                    [prte_reachable_netlink_happy=0], [
#include <sys/types.h>
#include <net/if.h>
])

    # this is terrible, but libnl-1 and libnl-3 are incompatible in
    # weird ways, and once there are libraries in LIBS for one, the
    # other is hard to get right.  So if someone has already decided
    # we have libnl version 1, get out.  Otherwise, see if we have
    # libnl-3, which is the only version supported by the netlink
    # component.
    AS_IF([test $prte_libnl_version -eq 1],
	      [prte_reachable_netlink_happy=0],
          [PRTE_CHECK_LIBNL_V3([$prte_libnl_location],
			                    [prte_reachable_netlink])
	       AS_IF([test "$PRTE_HAVE_LIBNL3" != "1"],
		         [prte_reachable_netlink_happy=0])])

    AS_IF([test $prte_reachable_netlink_happy -eq 1],
          [$1],
          [$2])

    AC_SUBST([prte_reachable_netlink_CPPFLAGS])
    AC_SUBST([prte_reachable_netlink_LDFLAGS])
    AC_SUBST([prte_reachable_netlink_LIBS])

    PRTE_VAR_SCOPE_POP()
])
