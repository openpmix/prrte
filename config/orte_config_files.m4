# -*- shell-script -*-
#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2009-2010 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
#                         reserved.
# Copyright (c) 2015-2016 Intel, Inc. All rights reserved
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([ORTE_CONFIG_FILES],[
    AC_CONFIG_FILES([
        orte/Makefile
        orte/include/Makefile
        orte/etc/Makefile

        orte/tools/psrvd/Makefile
        orte/tools/prun/Makefile
        orte/tools/wrappers/Makefile
        orte/tools/wrappers/pcc-wrapper-data.txt
        orte/tools/wrappers/pcc.pc
        orte/tools/pinfo/Makefile
        orte/tools/psrvr/Makefile
    ])
])
