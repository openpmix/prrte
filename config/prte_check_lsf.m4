dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2015      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017-2021 IBM Corporation.  All rights reserved.
dnl Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# check for lsf
# PRTE_CHECK_LSF(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_LSF],[
    AS_IF([test -z "$prte_check_lsf_happy"],[
       AC_ARG_WITH([lsf],
               [AS_HELP_STRING([--with-lsf(=DIR)],
                       [Build LSF support])])
       PRTE_CHECK_WITHDIR([lsf], [$with_lsf], [include/lsf/lsbatch.h])
       AC_ARG_WITH([lsf-libdir],
               [AS_HELP_STRING([--with-lsf-libdir=DIR],
                       [Search for LSF libraries in DIR])])
       PRTE_CHECK_WITHDIR([lsf-libdir], [$with_lsf_libdir], [libbat.*])

       AS_IF([test "$with_lsf" != "no"],[
          # Defaults
          prte_check_lsf_dir_msg="compiler default"
          prte_check_lsf_libdir_msg="linker default"

          # Save directory names if supplied
          AS_IF([test ! -z "$with_lsf" && test "$with_lsf" != "yes"],
                    [prte_check_lsf_dir="$with_lsf"
                     prte_check_lsf_dir_msg="$prte_check_lsf_dir (from --with-lsf)"])
          AS_IF([test ! -z "$with_lsf_libdir" && test "$with_lsf_libdir" != "yes"],
                    [prte_check_lsf_libdir="$with_lsf_libdir"
                     prte_check_lsf_libdir_msg="$prte_check_lsf_libdir (from --with-lsf-libdir)"])

          # If no directories were specified, look for LSF_LIBDIR,
          # LSF_INCLUDEDIR, and/or LSF_ENVDIR.
          AS_IF([test -z "$prte_check_lsf_dir" && test -z "$prte_check_lsf_libdir"],
                    [AS_IF([test ! -z "$LSF_ENVDIR" && test -z "$LSF_LIBDIR" && test -f "$LSF_ENVDIR/lsf.conf"],
                           [LSF_LIBDIR=`egrep ^LSF_LIBDIR= $LSF_ENVDIR/lsf.conf | cut -d= -f2-`])
                     AS_IF([test ! -z "$LSF_ENVDIR" && test -z "$LSF_INCLUDEDIR" && test -f "$LSF_ENVDIR/lsf.conf"],
                           [LSF_INCLUDEDIR=`egrep ^LSF_INCLUDEDIR= $LSF_ENVDIR/lsf.conf | cut -d= -f2-`])
                     AS_IF([test ! -z "$LSF_LIBDIR"],
                           [prte_check_lsf_libdir=$LSF_LIBDIR
                            prte_check_lsf_libdir_msg="$LSF_LIBDIR (from \$LSF_LIBDIR)"])
                     AS_IF([test ! -z "$LSF_INCLUDEDIR"],
                           [prte_check_lsf_dir=`dirname $LSF_INCLUDEDIR`
                            prte_check_lsf_dir_msg="$prte_check_lsf_dir (from \$LSF_INCLUDEDIR)"])])

          AS_IF([test "$with_lsf" = "no"],
                    [prte_check_lsf_happy="no"],
                    [prte_check_lsf_happy="yes"])

          prte_check_lsf_$1_save_CPPFLAGS="$CPPFLAGS"
          prte_check_lsf_$1_save_LDFLAGS="$LDFLAGS"
          prte_check_lsf_$1_save_LIBS="$LIBS"

          # liblsf requires yp_all, yp_get_default_domain, and ypprot_err
          # on Linux, Solaris, NEC, and Sony NEWSs these are found in libnsl
          # on AIX it should be in libbsd
          # on HP-UX it should be in libBSD
          # on IRIX < 6 it should be in libsun (IRIX 6 and later it is in libc)
          AS_IF([test "$prte_check_lsf_happy" = "yes"],
                [PRTE_SEARCH_LIBS_COMPONENT([yp_all_nsl], [yp_all], [nsl bsd BSD sun],
                              [prte_check_lsf_happy="yes"],
                              [prte_check_lsf_happy="no"])])

          # liblsf requires shm_open, shm_unlink, which are in librt
          AS_IF([test "$prte_check_lsf_happy" = "yes"],
                [PRTE_SEARCH_LIBS_COMPONENT([shm_open_rt], [shm_open], [rt],
                              [prte_check_lsf_happy="yes"],
                              [prte_check_lsf_happy="no"])])

          AS_IF([test "$prte_check_lsf_happy" = "yes"],
                [PRTE_CHECK_PACKAGE([ls_info_lsf],
                           [lsf/lsf.h],
                           [lsf],
                           [ls_info],
                           [$yp_all_nsl_LIBS $shm_open_rt_LIBS],
                           [$prte_check_lsf_dir],
                           [$prte_check_lsf_libdir],
                           [prte_check_lsf_happy="yes"],
                           [prte_check_lsf_happy="no"])])

          # test function of liblsb LSF package
          AS_IF([test "$prte_check_lsf_happy" = "yes"],
                    [AC_MSG_CHECKING([for LSF dir])
                     AC_MSG_RESULT([$prte_check_lsf_dir_msg])
                     AC_MSG_CHECKING([for LSF library dir])
                     AC_MSG_RESULT([$prte_check_lsf_libdir_msg])
                     PRTE_CHECK_PACKAGE([prte_check_lsf],
                        [lsf/lsbatch.h],
                        [bat],
                        [lsb_launch],
                        [$ls_info_lsf_LIBS $yp_all_nsl_LIBS $shm_open_rt_LIBS],
                        [$prte_check_lsf_dir],
                        [$prte_check_lsf_libdir],
                        [prte_check_lsf_happy="yes"],
                        [prte_check_lsf_happy="no"])])

          # Some versions of LSF ship with a libevent.so in their library path.
          # This is _not_ a copy of Libevent, but something specific to their project.
          # The Open MPI components should not need to link against LSF's libevent.so
          # However, the presence of it in the linker search path can cause a problem
          # if there is a system installed Libevent and Open MPI chooses the 'external'
          # event component prior to this stage.
          #
          # Add a check here to see if we are in a scenario where the two are conflicting.
          # In which case the earlier checks for successful compile of an LSF program will
          # have failed with messages like:
          #   lib64/libevent_pthreads.so: undefined reference to `evthread_set_condition_callbacks'
          #   lib64/libevent_pthreads.so: undefined reference to `event_mm_malloc_'
          #   lib64/libevent_pthreads.so: undefined reference to `event_mm_free_'
          #   lib64/libevent_pthreads.so: undefined reference to `evthread_set_id_callback'
          #   lib64/libevent_pthreads.so: undefined reference to `evthread_set_lock_callbacks'
          # Because it picked up -levent from LSF, but -levent_pthreads from Libevent.
          #
          # So look for a function that libevent_pthreads is looking for from libevent.so.
          # If it does appears then we have the correct libevent.so, otherwise then we picked
          # up the LSF version and a conflict has been detected.
          # If the external libevent component used 'event_core' instead of 'event'
          prte_check_lsf_event_conflict=na
          # Split libs into an array, see if -levent is in that list
          prte_check_lsf_libevent_present=`echo "$LIBS" | awk '{split([$]0, a, " "); {for (k in a) {if (a[[k]] == "-levent") {print a[[k]]}}}}' | wc -l | tr -d '[[:space:]]'`
          # (1) LSF check must have failed above. We need to know why...
          AS_IF([test "$prte_check_lsf_happy" = "no"],
                [# (2) If there is a -levent in the $LIBS then that might be the problem
                 AS_IF([test "$prte_check_lsf_libevent_present" != "0"],
                       [AS_IF([test "$prte_check_lsf_libdir" = "" ],
                              [],
                              [LDFLAGS="$LDFLAGS -L$prte_check_lsf_libdir"])
                        # Note that we do not want to set LIBS here to include -llsf since
                        # the check is not for an LSF library, but for the conflict with
                        # LDFLAGS.
                        # (3) Check to see if the -levent is from Libevent (check for a symbol it has)
                        AC_CHECK_LIB([event], [evthread_set_condition_callbacks],
                                     [AC_MSG_CHECKING([for libevent conflict])
                                      AC_MSG_RESULT([No. The correct libevent.so was linked.])
                                      prte_check_lsf_event_conflict=no],
                                     [# (4) The libevent.so is not from Libevent. Warn the user.
                                      AC_MSG_CHECKING([for libevent conflict])
                                      AC_MSG_RESULT([Yes. Detected a libevent.so that is not from Libevent.])
                                      prte_check_lsf_event_conflict=yes])
                       ],
                       [AC_MSG_CHECKING([for libevent conflict])
                        AC_MSG_RESULT([No. -levent is not being explicitly used.])
                        prte_check_lsf_event_conflict=na])],
                [AC_MSG_CHECKING([for libevent conflict])
                 AC_MSG_RESULT([No. LSF checks passed.])
                 prte_check_lsf_event_conflict=na])

          AS_IF([test "$prte_check_lsf_event_conflict" = "yes"],
                [AC_MSG_WARN([===================================================================])
                 AC_MSG_WARN([Conflicting libevent.so libraries detected on the system.])
                 AC_MSG_WARN([])
                 AC_MSG_WARN([A system-installed Libevent library was detected and the PRRTE])
                 AC_MSG_WARN([build system relies on an external Libevent in the linker search path.])
                 AC_MSG_WARN([If LSF is present on the system and in the default search path then])
                 AC_MSG_WARN([it _may be_ the source of the conflict.])
                 AC_MSG_WARN([LSF provides a libevent.so that is not from Libevent in its])
                 AC_MSG_WARN([library path. At this point the linker is attempting to resolve])
                 AC_MSG_WARN([Libevent symbols using the LSF library because of the lack of])
                 AC_MSG_WARN([an explicit linker path pointing to the system-installed Libevent.])
                 AC_MSG_WARN([])
                 AC_MSG_WARN([To resolve this issue try to explicitly pass the Libevent])
                 AC_MSG_WARN([library path on the configure line (--with-libevent-libdir).])
                 AC_MSG_WARN([===================================================================])
                ])

          CPPFLAGS="$prte_check_lsf_$1_save_CPPFLAGS"
          LDFLAGS="$prte_check_lsf_$1_save_LDFLAGS"
          LIBS="$prte_check_lsf_$1_save_LIBS"

       ],[prte_check_lsf_happy=no])

       PRTE_SUMMARY_ADD([[Resource Managers]],[[LSF]],[$1],[$prte_check_lsf_happy])
    ])

    AS_IF([test "$prte_check_lsf_happy" = "yes"],
          [$1_LIBS="[$]$1_LIBS $prte_check_lsf_LIBS"
           $1_LDFLAGS="[$]$1_LDFLAGS $prte_check_lsf_LDFLAGS"
           $1_CPPFLAGS="[$]$1_CPPFLAGS $prte_check_lsf_CPPFLAGS"
           # add the LSF libraries to static builds as they are required
           $1_WRAPPER_EXTRA_LDFLAGS=[$]$1_LDFLAGS
           $1_WRAPPER_EXTRA_LIBS=[$]$1_LIBS
           $2],
          [AS_IF([test ! -z "$with_lsf" && test "$with_lsf" != "no"],
                 [AC_MSG_WARN([LSF support requested (via --with-lsf) but not found.])
                  AC_MSG_ERROR([Aborting.])])
           $3])
])
