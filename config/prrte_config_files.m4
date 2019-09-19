# -*- shell-script -*-
#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2019      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# This file is m4_included in the top-level configure.ac

AC_DEFUN([PRRTE_CONFIG_FILES],[
    AC_CONFIG_FILES([
        src/Makefile
        src/etc/Makefile
        src/include/Makefile
        src/mca/base/Makefile
        src/util/Makefile
        src/util/hostfile/Makefile
        src/util/keyval/Makefile
        src/tools/pcc/Makefile
        src/tools/pcc/pcc-wrapper-data.txt
        src/tools/prted/Makefile
        src/tools/prun/Makefile
        src/tools/prte_info/Makefile
        src/tools/prte/Makefile
    ])
])
