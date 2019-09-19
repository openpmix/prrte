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
dnl Copyright (c) 2006-2016 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2015-2017 Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PRRTE_CHECK_TM_LIBS_FLAGS(prefix, [LIBS or LDFLAGS])
# ---------------------------------------------------
AC_DEFUN([PRRTE_CHECK_TM_LIBS_FLAGS],[
    PRRTE_VAR_SCOPE_PUSH([prrte_check_tm_flags])
    prrte_check_tm_flags=`$prrte_check_tm_pbs_config --libs`
    for prrte_check_tm_val in $prrte_check_tm_flags; do
        if test "`echo $prrte_check_tm_val | cut -c1-2`" = "-l"; then
            if test "$2" = "LIBS"; then
                $1_$2="$$1_$2 $prrte_check_tm_val"
            fi
        else
            if test "$2" = "LDFLAGS"; then
                $1_$2="$$1_$2 $prrte_check_tm_val"
            fi
        fi
    done
    PRRTE_VAR_SCOPE_POP
])


# PRRTE_CHECK_TM(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PRRTE_CHECK_TM],[
    if test -z $prrte_check_tm_happy ; then
	PRRTE_VAR_SCOPE_PUSH([prrte_check_tm_found prrte_check_tm_dir prrte_check_tm_pbs_config prrte_check_tm_LDFLAGS_save prrte_check_tm_CPPFLAGS_save prrte_check_tm_LIBS_save])

	AC_ARG_WITH([tm],
                    [AC_HELP_STRING([--with-tm(=DIR)],
                                    [Build TM (Torque, PBSPro, and compatible) support, optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])])
	PRRTE_CHECK_WITHDIR([tm], [$with_tm], [include/tm.h])

	prrte_check_tm_found=no
	AS_IF([test "$with_tm" = "no"],
              [prrte_check_tm_happy="no"],
              [prrte_check_tm_happy="yes"
               AS_IF([test ! -z "$with_tm" && test "$with_tm" != "yes"],
                     [prrte_check_tm_dir="$with_tm"],
                     [prrte_check_tm_dir=""])])

	AS_IF([test "$prrte_check_tm_happy" = "yes"],
              [AC_MSG_CHECKING([for pbs-config])
               prrte_check_tm_pbs_config="not found"
               AS_IF([test "$prrte_check_tm_dir" != "" && test -d "$prrte_check_tm_dir" && test -x "$prrte_check_tm_dir/bin/pbs-config"],
                     [prrte_check_tm_pbs_config="$prrte_check_tm_dir/bin/pbs-config"],
                     [AS_IF([pbs-config --prefix >/dev/null 2>&1],
                            [prrte_check_tm_pbs_config="pbs-config"])])
               AC_MSG_RESULT([$prrte_check_tm_pbs_config])])

	# If we have pbs-config, get the flags we need from there and then
	# do simplistic tests looking for the tm headers and symbols

	AS_IF([test "$prrte_check_tm_happy" = "yes" && test "$prrte_check_tm_pbs_config" != "not found"],
              [prrte_check_tm_CPPFLAGS=`$prrte_check_tm_pbs_config --cflags`
               PRRTE_LOG_MSG([prrte_check_tm_CPPFLAGS from pbs-config: $prrte_check_tm_CPPFLAGS], 1)

               PRRTE_CHECK_TM_LIBS_FLAGS([prrte_check_tm], [LDFLAGS])
               PRRTE_LOG_MSG([prrte_check_tm_LDFLAGS from pbs-config: $prrte_check_tm_LDFLAGS], 1)

               PRRTE_CHECK_TM_LIBS_FLAGS([prrte_check_tm], [LIBS])
               PRRTE_LOG_MSG([prrte_check_tm_LIBS from pbs-config: $prrte_check_tm_LIBS], 1)

               # Now that we supposedly have the right flags, try them out.

               prrte_check_tm_CPPFLAGS_save="$CPPFLAGS"
               prrte_check_tm_LDFLAGS_save="$LDFLAGS"
               prrte_check_tm_LIBS_save="$LIBS"

               CPPFLAGS="$CPPFLAGS $prrte_check_tm_CPPFLAGS"
               LIBS="$LIBS $prrte_check_tm_LIBS"
               LDFLAGS="$LDFLAGS $prrte_check_tm_LDFLAGS"

               AC_CHECK_HEADER([tm.h],
			       [AC_CHECK_FUNC([tm_finalize],
					      [prrte_check_tm_found="yes"])])

               CPPFLAGS="$prrte_check_tm_CPPFLAGS_save"
               LDFLAGS="$prrte_check_tm_LDFLAGS_save"
               LIBS="$prrte_check_tm_LIBS_save"])

	# If we don't have pbs-config, then we have to look around
	# manually.

	# Note that Torque 2.1.0 changed the name of their back-end
	# library to "libtorque".  So we have to check for both libpbs and
	# libtorque.  First, check for libpbs.

	prrte_check_package_$1_save_CPPFLAGS="$CPPFLAGS"
	prrte_check_package_$1_save_LDFLAGS="$LDFLAGS"
	prrte_check_package_$1_save_LIBS="$LIBS"

	AS_IF([test "$prrte_check_tm_found" = "no"],
              [AS_IF([test "$prrte_check_tm_happy" = "yes"],
                     [_PRRTE_CHECK_PACKAGE_HEADER([prrte_check_tm],
						 [tm.h],
						 [$prrte_check_tm_dir],
						 [prrte_check_tm_found="yes"],
						 [prrte_check_tm_found="no"])])

               AS_IF([test "$prrte_check_tm_found" = "yes"],
                     [_PRRTE_CHECK_PACKAGE_LIB([prrte_check_tm],
					      [pbs],
					      [tm_init],
					      [],
					      [$prrte_check_tm_dir],
					      [$prrte_check_tm_libdir],
					      [prrte_check_tm_found="yes"],
                                              [_PRRTE_CHECK_PACKAGE_LIB([prrte_check_tm],
					                               [pbs],
					                               [tm_init],
					                               [-lcrypto],
					                               [$prrte_check_tm_dir],
					                               [$prrte_check_tm_libdir],
					                               [prrte_check_tm_found="yes"],
					                               [_PRRTE_CHECK_PACKAGE_LIB([prrte_check_tm],
								                                [torque],
								                                [tm_init],
								                                [],
								                                [$prrte_check_tm_dir],
								                                [$prrte_check_tm_libdir],
								                                [prrte_check_tm_found="yes"],
								                                [prrte_check_tm_found="no"])])])])])

	CPPFLAGS="$prrte_check_package_$1_save_CPPFLAGS"
	LDFLAGS="$prrte_check_package_$1_save_LDFLAGS"
	LIBS="$prrte_check_package_$1_save_LIBS"

	if test "$prrte_check_tm_found" = "no" ; then
	    prrte_check_tm_happy=no
	fi

	PRRTE_SUMMARY_ADD([[Resource Managers]],[[Torque]],[$1],[$prrte_check_tm_happy])

	PRRTE_VAR_SCOPE_POP
    fi

    # Did we find the right stuff?
    AS_IF([test "$prrte_check_tm_happy" = "yes"],
          [$1_LIBS="[$]$1_LIBS $prrte_check_tm_LIBS"
	   $1_LDFLAGS="[$]$1_LDFLAGS $prrte_check_tm_LDFLAGS"
	   $1_CPPFLAGS="[$]$1_CPPFLAGS $prrte_check_tm_CPPFLAGS"
	   # add the TM libraries to static builds as they are required
	   $1_WRAPPER_EXTRA_LDFLAGS=[$]$1_LDFLAGS
	   $1_WRAPPER_EXTRA_LIBS=[$]$1_LIBS
	   $2],
          [AS_IF([test ! -z "$with_tm" && test "$with_tm" != "no"],
                 [AC_MSG_ERROR([TM support requested but not found.  Aborting])])
	   prrte_check_tm_happy="no"
	   $3])
])
