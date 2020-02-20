dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2010-2015 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

dnl There will only be one component used in this framework, and it will
dnl be selected at configure time by priority.  Components must set
dnl their priorities in their configure.m4 file.

dnl We only want one winning component (vs. STOP_AT_FIRST_PRIORITY,
dnl which will allow all components of the same priority who succeed to
dnl win)
m4_define(MCA_prrte_prtedl_CONFIGURE_MODE, STOP_AT_FIRST)

AC_DEFUN([MCA_prrte_prtedl_CONFIG],[
    PRRTE_HAVE_DL_SUPPORT=0

    # If --disable-prtedlopen was used, then have all the components fail
    # (we still need to configure them all so that things like "make
    # dist" work", but we just want the MCA system to (artificially)
    # conclude that it can't build any of the components.
    AS_IF([test "$enable_prtedlopen" = "no"],
          [want_prtedl=0], [want_prtedl=1])

    MCA_CONFIGURE_FRAMEWORK([prtedl], [$want_prtedl])

    # If we found no suitable static prtedl component and prtedlopen support
    # was not specifically disabled, this is an error.
    AS_IF([test "$MCA_prrte_prtedl_STATIC_COMPONENTS" = "" && \
           test "$enable_prtedlopen" != "no"],
          [AC_MSG_WARN([Did not find a suitable static prrte prtedl component])
           AC_MSG_WARN([You might need to install libltld (and its headers) or])
           AC_MSG_WARN([specify --disable-prtedlopen to configure.])
           AC_MSG_ERROR([Cannot continue])])

    # If we have a winning component (which, per above, will only
    # happen if --disable-prtedlopen was *not* specified), do some more
    # logic.
    AS_IF([test "$MCA_prrte_prtedl_STATIC_COMPONENTS" != ""],
       [ # We had a winner -- w00t!

        PRRTE_HAVE_DL_SUPPORT=1
        # If we added any -L flags to ADD_LDFLAGS, then we (might)
        # need to add those directories to LD_LIBRARY_PATH.
        # Otherwise, if we try to AC RUN_IFELSE anything here in
        # configure, it might die because it can't find the libraries
        # we just linked against.
        PRRTE_VAR_SCOPE_PUSH([prrte_prtedl_base_found_l prrte_prtedl_base_token prrte_prtedl_base_tmp prrte_prtedl_base_dir])
        prrte_prtedl_base_found_l=0
        eval "prrte_prtedl_base_tmp=\$prrte_prtedl_${prrte_prtedl_winner}_ADD_LIBS"
        for prrte_prtedl_base_token in $prrte_prtedl_base_tmp; do
            case $prrte_prtedl_base_token in
            -l*) prrte_prtedl_base_found_l=1 ;;
            esac
        done
        AS_IF([test $prrte_prtedl_base_found_l -eq 1],
              [eval "prrte_prtedl_base_tmp=\$prrte_prtedl_${prrte_prtedl_winner}_ADD_LDFLAGS"
               for prrte_prtedl_base_token in $prrte_prtedl_base_tmp; do
                   case $prrte_prtedl_base_token in
                   -L*)
                       prrte_prtedl_base_dir=`echo $prrte_prtedl_base_token | cut -c3-`
                       export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$prrte_prtedl_base_dir
                       AC_MSG_WARN([Adding to LD_LIBRARY_PATH: $prrte_prtedl_base_dir])
                       ;;
                   esac
               done])
        PRRTE_VAR_SCOPE_POP
    ])

    AC_DEFINE_UNQUOTED([PRRTE_HAVE_DL_SUPPORT], [$PRRTE_HAVE_DL_SUPPORT],
                       [Whether the PRRTE DL framework is functional or not])
])
