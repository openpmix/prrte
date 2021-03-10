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
dnl Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2015-2017 Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PRTE_CHECK_TM_LIBS_FLAGS(prefix, [LIBS or LDFLAGS])
# ---------------------------------------------------
AC_DEFUN([PRTE_CHECK_TM_LIBS_FLAGS],[
    PRTE_VAR_SCOPE_PUSH([prte_check_tm_flags])
    prte_check_tm_flags=`$prte_check_tm_pbs_config --libs`
    for prte_check_tm_val in $prte_check_tm_flags; do
        if test "`echo $prte_check_tm_val | cut -c1-2`" = "-l"; then
            if test "$2" = "LIBS"; then
                $1_$2="$$1_$2 $prte_check_tm_val"
            fi
        else
            if test "$2" = "LDFLAGS"; then
                $1_$2="$$1_$2 $prte_check_tm_val"
            fi
        fi
    done
    PRTE_VAR_SCOPE_POP
])


# PRTE_CHECK_TM(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRTE_CHECK_TM],[
    if test -z $prte_check_tm_happy ; then
	PRTE_VAR_SCOPE_PUSH([prte_check_tm_found prte_check_tm_dir prte_check_tm_pbs_config prte_check_tm_LDFLAGS_save prte_check_tm_CPPFLAGS_save prte_check_tm_LIBS_save])

	AC_ARG_WITH([tm],
                    [AS_HELP_STRING([--with-tm(=DIR)],
                                    [Build TM (Torque, PBSPro, and compatible) support, optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])])
	PRTE_CHECK_WITHDIR([tm], [$with_tm], [include/tm.h])

	prte_check_tm_found=no
	AS_IF([test "$with_tm" = "no"],
              [prte_check_tm_happy="no"],
              [prte_check_tm_happy="yes"
               AS_IF([test ! -z "$with_tm" && test "$with_tm" != "yes"],
                     [prte_check_tm_dir="$with_tm"],
                     [prte_check_tm_dir=""])])

	AS_IF([test "$prte_check_tm_happy" = "yes"],
              [AC_MSG_CHECKING([for pbs-config])
               prte_check_tm_pbs_config="not found"
               AS_IF([test "$prte_check_tm_dir" != "" && test -d "$prte_check_tm_dir" && test -x "$prte_check_tm_dir/bin/pbs-config"],
                     [prte_check_tm_pbs_config="$prte_check_tm_dir/bin/pbs-config"],
                     [AS_IF([pbs-config --prefix >/dev/null 2>&1],
                            [prte_check_tm_pbs_config="pbs-config"])])
               AC_MSG_RESULT([$prte_check_tm_pbs_config])])

	# If we have pbs-config, get the flags we need from there and then
	# do simplistic tests looking for the tm headers and symbols

	AS_IF([test "$prte_check_tm_happy" = "yes" && test "$prte_check_tm_pbs_config" != "not found"],
              [prte_check_tm_CPPFLAGS=`$prte_check_tm_pbs_config --cflags`
               PRTE_LOG_MSG([prte_check_tm_CPPFLAGS from pbs-config: $prte_check_tm_CPPFLAGS], 1)

               PRTE_CHECK_TM_LIBS_FLAGS([prte_check_tm], [LDFLAGS])
               PRTE_LOG_MSG([prte_check_tm_LDFLAGS from pbs-config: $prte_check_tm_LDFLAGS], 1)

               PRTE_CHECK_TM_LIBS_FLAGS([prte_check_tm], [LIBS])
               PRTE_LOG_MSG([prte_check_tm_LIBS from pbs-config: $prte_check_tm_LIBS], 1)

               # Now that we supposedly have the right flags, try them out.

               prte_check_tm_CPPFLAGS_save="$CPPFLAGS"
               prte_check_tm_LDFLAGS_save="$LDFLAGS"
               prte_check_tm_LIBS_save="$LIBS"

               CPPFLAGS="$CPPFLAGS $prte_check_tm_CPPFLAGS"
               LIBS="$LIBS $prte_check_tm_LIBS"
               LDFLAGS="$LDFLAGS $prte_check_tm_LDFLAGS"

               AC_CHECK_HEADER([tm.h],
			       [AC_CHECK_FUNC([tm_finalize],
					      [prte_check_tm_found="yes"])])

               CPPFLAGS="$prte_check_tm_CPPFLAGS_save"
               LDFLAGS="$prte_check_tm_LDFLAGS_save"
               LIBS="$prte_check_tm_LIBS_save"])

	# If we don't have pbs-config, then we have to look around
	# manually.

	# Note that Torque 2.1.0 changed the name of their back-end
	# library to "libtorque".  So we have to check for both libpbs and
	# libtorque.  First, check for libpbs.

	prte_check_package_$1_save_CPPFLAGS="$CPPFLAGS"
	prte_check_package_$1_save_LDFLAGS="$LDFLAGS"
	prte_check_package_$1_save_LIBS="$LIBS"

	AS_IF([test "$prte_check_tm_found" = "no"],
              [AS_IF([test "$prte_check_tm_happy" = "yes"],
                     [_PRTE_CHECK_PACKAGE_HEADER([prte_check_tm],
						 [tm.h],
						 [$prte_check_tm_dir],
						 [prte_check_tm_found="yes"],
						 [prte_check_tm_found="no"])])

               AS_IF([test "$prte_check_tm_found" = "yes"],
                     [_PRTE_CHECK_PACKAGE_LIB([prte_check_tm],
					      [pbs],
					      [tm_init],
					      [],
					      [$prte_check_tm_dir],
					      [$prte_check_tm_libdir],
					      [prte_check_tm_found="yes"],
                                              [_PRTE_CHECK_PACKAGE_LIB([prte_check_tm],
					                               [pbs],
					                               [tm_init],
					                               [-lcrypto -lz],
					                               [$prte_check_tm_dir],
					                               [$prte_check_tm_libdir],
					                               [prte_check_tm_found="yes"],
					                               [_PRTE_CHECK_PACKAGE_LIB([prte_check_tm],
								                                [torque],
								                                [tm_init],
								                                [],
								                                [$prte_check_tm_dir],
								                                [$prte_check_tm_libdir],
								                                [prte_check_tm_found="yes"],
								                                [prte_check_tm_found="no"])])])])])

	CPPFLAGS="$prte_check_package_$1_save_CPPFLAGS"
	LDFLAGS="$prte_check_package_$1_save_LDFLAGS"
	LIBS="$prte_check_package_$1_save_LIBS"

	if test "$prte_check_tm_found" = "no" ; then
	    prte_check_tm_happy=no
	fi

	PRTE_SUMMARY_ADD([[Resource Managers]],[[Torque]],[$1],[$prte_check_tm_happy])

	PRTE_VAR_SCOPE_POP
    fi

    # Did we find the right stuff?
    AS_IF([test "$prte_check_tm_happy" = "yes"],
          [$1_LIBS="[$]$1_LIBS $prte_check_tm_LIBS"
	   $1_LDFLAGS="[$]$1_LDFLAGS $prte_check_tm_LDFLAGS"
	   $1_CPPFLAGS="[$]$1_CPPFLAGS $prte_check_tm_CPPFLAGS"
	   # add the TM libraries to static builds as they are required
	   $1_WRAPPER_EXTRA_LDFLAGS=[$]$1_LDFLAGS
	   $1_WRAPPER_EXTRA_LIBS=[$]$1_LIBS
	   $2],
          [AS_IF([test ! -z "$with_tm" && test "$with_tm" != "no"],
                 [AC_MSG_ERROR([TM support requested but not found.  Aborting])])
	   prte_check_tm_happy="no"
	   $3])
])
