dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
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
dnl Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
dnl Copyright (c) 2009      IBM Corporation.  All rights reserved.
dnl Copyright (c) 2009-2013 Los Alamos National Security, LLC.  All rights
dnl                         reserved.
dnl Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
dnl
dnl Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl


AC_DEFUN([PRTE_CONFIGURE_OPTIONS],[
prte_show_subtitle "PRTE Configuration options"

#
# Do we want prte's --prefix behavior to be enabled by default?
#
AC_MSG_CHECKING([if want prte "--prefix" behavior to be enabled by default])
AC_ARG_ENABLE([prte-prefix-by-default],
    [AS_HELP_STRING([--enable-prte-prefix-by-default],
        [Make "prte ..." behave exactly the same as "prte --prefix \$prefix" (where \$prefix is the value given to --prefix in configure)])])
if test "$enable_prte_prefix_by_default" = "yes"; then
    AC_MSG_RESULT([yes])
    prte_want_prte_prefix_by_default=1
else
    AC_MSG_RESULT([no])
    prte_want_prte_prefix_by_default=0
fi
AC_DEFINE_UNQUOTED([PRTE_WANT_PRTE_PREFIX_BY_DEFAULT],
                   [$prte_want_prte_prefix_by_default],
                   [Whether we want prte to effect "--prefix $prefix" by default])

#
# Is this a developer copy?
#

if test -d .git; then
    PRTE_DEVEL=1
else
    PRTE_DEVEL=0
fi

#
# Developer picky compiler options
#

AC_MSG_CHECKING([if want developer-level compiler pickyness])
AC_ARG_ENABLE(picky,
    AS_HELP_STRING([--enable-picky],
                   [enable developer-level compiler pickyness when building Open MPI (default: disabled, unless a .git directory is found in the build tree)]))
if test "$enable_picky" = "yes"; then
    AC_MSG_RESULT([yes])
    WANT_PICKY_COMPILER=1
else
    AC_MSG_RESULT([no])
    WANT_PICKY_COMPILER=0
fi
#################### Developer default override ####################
if test "$WANT_PICKY_COMPILER" = "0" && test -z "$enable_picky" && test "$PRTE_DEVEL" = 1; then
    WANT_PICKY_COMPILER=1
    echo "--> developer override: enable picky compiler by default"
fi
#################### Developer default override ####################

#
# Developer debugging
#
AC_MSG_CHECKING([if want developer-level debugging code])
AC_ARG_ENABLE(debug,
    AS_HELP_STRING([--enable-debug],
                   [enable developer-level debugging code (not for general MPI users!) (default: disabled)]))
if test "$enable_debug" = "yes"; then
    AC_MSG_RESULT([yes])
    WANT_DEBUG=1
else
    AC_MSG_RESULT([no])
    WANT_DEBUG=0
fi


if test "$WANT_DEBUG" = "0"; then
    CFLAGS="-DNDEBUG $CFLAGS"
    CXXFLAGS="-DNDEBUG $CXXFLAGS"
fi
AC_DEFINE_UNQUOTED(PRTE_ENABLE_DEBUG, $WANT_DEBUG,
    [Whether we want developer-level debugging code or not])

AC_ARG_ENABLE(debug-symbols,
    AS_HELP_STRING([--disable-debug-symbols],
        [Disable adding compiler flags to enable debugging symbols if --enable-debug is specified.  For non-debugging builds, this flag has no effect.]))

#
# Do we want to install the internal devel headers?
#
AC_MSG_CHECKING([if want to install project-internal header files])
AC_ARG_WITH(devel-headers,
    AS_HELP_STRING([--with-devel-headers],
                   [Normal PRTE users/applications do not need this.  Developer headers are only necessary for authors doing deeper integration (default: disabled).]))
if test "$with_devel_headers" = "yes"; then
    AC_MSG_RESULT([yes])
    WANT_INSTALL_HEADERS=1
else
    AC_MSG_RESULT([no])
    WANT_INSTALL_HEADERS=0
fi
AM_CONDITIONAL(WANT_INSTALL_HEADERS, test "$WANT_INSTALL_HEADERS" = 1)

#
# Do we want the pretty-print stack trace feature?
#

AC_MSG_CHECKING([if want pretty-print stacktrace])
AC_ARG_ENABLE([pretty-print-stacktrace],
    [AS_HELP_STRING([--enable-pretty-print-stacktrace],
                    [Pretty print stacktrace on process signal (default: enabled)])])
if test "$enable_pretty_print_stacktrace" = "no" ; then
    AC_MSG_RESULT([no])
    WANT_PRETTY_PRINT_STACKTRACE=0
else
    AC_MSG_RESULT([yes])
    WANT_PRETTY_PRINT_STACKTRACE=1
fi
AC_DEFINE_UNQUOTED([PRTE_WANT_PRETTY_PRINT_STACKTRACE],
                   [$WANT_PRETTY_PRINT_STACKTRACE],
                   [if want pretty-print stack trace feature])


#
# Do we want PTY support?
#

AC_MSG_CHECKING([if want pty support])
AC_ARG_ENABLE(pty-support,
    AS_HELP_STRING([--enable-pty-support],
                   [Enable/disable PTY support for STDIO forwarding.  (default: enabled)]))
if test "$enable_pty_support" = "no" ; then
    AC_MSG_RESULT([no])
    PRTE_ENABLE_PTY_SUPPORT=0
else
    AC_MSG_RESULT([yes])
    PRTE_ENABLE_PTY_SUPPORT=1
fi
AC_DEFINE_UNQUOTED([PRTE_ENABLE_PTY_SUPPORT], [$PRTE_ENABLE_PTY_SUPPORT],
                   [Whether user wants PTY support or not])


#
# Do we want to allow DLOPEN?
#

AC_MSG_CHECKING([if want dlopen support])
AC_ARG_ENABLE([dlopen],
    [AS_HELP_STRING([--enable-dlopen],
                    [Whether build should attempt to use dlopen (or
                     similar) to dynamically load components.
                     Disabling dlopen implies --disable-mca-dso.
                     (default: enabled)])])
if test "$enable_dlopen" = "no" ; then
    PRTE_ENABLE_DLOPEN_SUPPORT=0
    AC_MSG_RESULT([no])
else
    PRTE_ENABLE_DLOPEN_SUPPORT=1
    AC_MSG_RESULT([yes])
fi
AC_DEFINE_UNQUOTED(PRTE_ENABLE_DLOPEN_SUPPORT, $PRTE_ENABLE_DLOPEN_SUPPORT,
    [Whether we want to enable dlopen support])


#
# Do we want to show component load error messages by default?
#

AC_MSG_CHECKING([for default value of mca_base_component_show_load_errors])
AC_ARG_ENABLE([show-load-errors-by-default],
    [AS_HELP_STRING([--enable-show-load-errors-by-default],
                    [Set the default value for the MCA parameter
                     mca_base_component_show_load_errors (but can be
                     overridden at run time by the usual
                     MCA-variable-setting mechansism).  This MCA variable
                     controls whether warnings are displayed when an MCA
                     component fails to load at run time due to an error.
                     (default: enabled in --enable-debug builds, meaning that
                      mca_base_component_show_load_errors is enabled
                      by default when configured with --enable-debug])])
if test "$enable_show_load_errors_by_default" = "no" ; then
    PRTE_SHOW_LOAD_ERRORS_DEFAULT=0
    AC_MSG_RESULT([disabled by default])
else
    PRTE_SHOW_LOAD_ERRORS_DEFAULT=$WANT_DEBUG
    if test "$WANT_DEBUG" = "1"; then
        AC_MSG_RESULT([enabled by default])
    else
        AC_MSG_RESULT([disabled by default])
    fi
fi
AC_DEFINE_UNQUOTED(PRTE_SHOW_LOAD_ERRORS_DEFAULT, $PRTE_SHOW_LOAD_ERRORS_DEFAULT,
                   [Default value for mca_base_component_show_load_errors MCA variable])


#
# Handle embedded version strings
#
AC_MSG_CHECKING([if a proxy version string for prte is required])
AC_ARG_WITH(proxy-version-string,
    AS_HELP_STRING([--with-proxy-version-string],
                   [Return the provided string when prte is used in proxy mode and the version is requested]))
if test -n "$with_proxy_version_string"; then
    AC_MSG_RESULT([yes])
    PRTE_PROXY_VERSION_STRING=$with_proxy_version_string
else
    AC_MSG_RESULT([no])
    PRTE_PROXY_VERSION_STRING=$PRTE_VERSION
fi
AC_DEFINE_UNQUOTED(PRTE_PROXY_VERSION_STRING, "$PRTE_PROXY_VERSION_STRING",
                   [Version string to be returned by prte when in proxy mode])

#
# Save the actual version in an external header file so that
# packages that use us can know what version we are
#
prtemajor=${PRTE_MAJOR_VERSION}L
prteminor=${PRTE_MINOR_VERSION}L
prterelease=${PRTE_RELEASE_VERSION}L
prtenumeric=$(printf 0x%4.4x%2.2x%2.2x $PRTE_MAJOR_VERSION $PRTE_MINOR_VERSION $PRTE_RELEASE_VERSION)
AC_SUBST(prtemajor)
AC_SUBST(prteminor)
AC_SUBST(prterelease)
AC_SUBST(prtenumeric)

AC_MSG_CHECKING([if a proxy package name for prte is required])
AC_ARG_WITH(proxy-package-name,
    AS_HELP_STRING([--with-proxy-package-name],
                   [Return the provided string when prte is used in proxy mode and the package name is requested]))
if test -n "$with_proxy_package_name"; then
    AC_MSG_RESULT([yes])
    PRTE_PROXY_PACKAGE_NAME=$with_proxy_package_name
else
    AC_MSG_RESULT([no])
    PRTE_PROXY_PACKAGE_NAME="PMIx Reference RunTime Environment"
fi
AC_DEFINE_UNQUOTED(PRTE_PROXY_PACKAGE_NAME, "$PRTE_PROXY_PACKAGE_NAME",
                   [Package name to be returned by prte when in proxy mode])

AC_MSG_CHECKING([if a proxy bugreport path for prte is required])
AC_ARG_WITH(proxy-bugreport,
    AS_HELP_STRING([--with-proxy-bugreport],
                   [Return the provided string when prte is used in proxy mode and the PACKAGE_BUGREPORT is requested]))
if test -n "$with_proxy_bugreport"; then
    AC_MSG_RESULT([yes])
    PRTE_PROXY_BUGREPORT=$with_proxy_bugreport
else
    AC_MSG_RESULT([no])
    PRTE_PROXY_BUGREPORT=https://github.com/openpmix/prte/
fi
AC_DEFINE_UNQUOTED(PRTE_PROXY_BUGREPORT, "$PRTE_PROXY_BUGREPORT",
                   [Bugreport string to be returned by prte when in proxy mode])


#
# Support per-user config files?
#
AC_ARG_ENABLE([per-user-config-files],
   [AS_HELP_STRING([--enable-per-user-config-files],
      [Disable per-user configuration files, to save disk accesses during job start-up.  This is likely desirable for large jobs.  Note that this can also be achieved by environment variables at run-time.  (default: enabled)])])
if test "$enable_per_user_config_files" = "no" ; then
  result=0
else
  result=1
fi
AC_DEFINE_UNQUOTED([PRTE_WANT_HOME_CONFIG_FILES], [$result],
     [Enable per-user config files])

#
# Do we want to enable IPv6 support?
#
AC_MSG_CHECKING([if want IPv6 support])
AC_ARG_ENABLE([ipv6],
    [AS_HELP_STRING([--enable-ipv6],
        [Enable IPv6 support, but only if the underlying system supports it (default: disabled)])])
if test "$enable_ipv6" = "yes"; then
    AC_MSG_RESULT([yes])
    prte_want_ipv6=1
else
    AC_MSG_RESULT([no])
    prte_want_ipv6=0
fi
AC_DEFINE_UNQUOTED([PRTE_ENABLE_IPV6], [$prte_want_ipv6],
                   [Enable IPv6 support, but only if the underlying system supports it])


# Add any extra lib?
AC_ARG_WITH([prte-extra-lib],
            AS_HELP_STRING([--with-prte-extra-lib=LIB],
                           [Link the output PRTE library to this extra lib (used in embedded mode)]))
AC_MSG_CHECKING([for extra lib])
AS_IF([test ! -z "$with_prte_extra_lib"],
      [AS_IF([test "$with_prte_extra_lib" = "yes" || test "$with_prte_extra_lib" = "no"],
             [AC_MSG_RESULT([ERROR])
              AC_MSG_WARN([Invalid value for --with-extra-prte-lib:])
              AC_MSG_WARN([    $with_prte_extra_lib])
              AC_MSG_WARN([Must be path name of the library to add])
              AC_MSG_ERROR([Cannot continue])],
             [AC_MSG_RESULT([$with_prte_extra_lib])
              PRTE_EXTRA_LIB=$with_prte_extra_lib])],
      [AC_MSG_RESULT([no])
       PRTE_EXTRA_LIB=])
AC_SUBST(PRTE_EXTRA_LIB)

# Add any extra libtool lib?
AC_ARG_WITH([prte-extra-ltlib],
            AS_HELP_STRING([--with-prte-extra-ltlib=LIB],
                           [Link any embedded components/tools that require it to the provided libtool lib (used in embedded mode)]))
AC_MSG_CHECKING([for extra ltlib])
AS_IF([test ! -z "$with_prte_extra_ltlib"],
      [AS_IF([test "$with_prte_extra_ltlib" = "yes" || test "$with_prte_extra_ltlib" = "no"],
             [AC_MSG_RESULT([ERROR])
              AC_MSG_WARN([Invalid value for --with-prte-extra-ltlib:])
              AC_MSG_WARN([    $with_prte_extra_ltlib])
              AC_MSG_WARN([Must be path name of the library to add])
              AC_MSG_ERROR([Cannot continue])],
             [AC_MSG_RESULT([$with_prte_extra_ltlib])
              PRTE_EXTRA_LTLIB=$with_prte_extra_ltlib])],
      [AC_MSG_RESULT([no])
       PRTE_EXTRA_LTLIB=])
AC_SUBST(PRTE_EXTRA_LTLIB)

# Add any extra LDFLAGS for the extra libs?
AC_ARG_WITH([prte-extra-lib-ldflags],
            AS_HELP_STRING([--with-prte-extra-lib-ldflags=flags],
                           [Where to find the extra libs]))
AC_MSG_CHECKING([for extra lib LDFLAGS])
AS_IF([test ! -z "$with_prte_extra_lib_ldflags"],
      [AS_IF([test "$with_prte_extra_lib_ldflags" = "yes" || test "$with_prte_extra_lib_ldflags" = "no"],
             [AC_MSG_RESULT([ERROR])
              AC_MSG_WARN([Invalid value for --with-prte-extra-lib-ldflags:])
              AC_MSG_WARN([    $with_prte_extra_lib_ldflags])
              AC_MSG_WARN([Must be path name pointing to the libs to add])
              AC_MSG_ERROR([Cannot continue])],
             [AC_MSG_RESULT([$with_prte_extra_lib_ldflags])
              PRTE_EXTRA_LIB_LDFLAGS=$with_prte_extra_lib_ldflags])],
      [AC_MSG_RESULT([no])
       PRTE_EXTRA_LIB_LDFLAGS=])
AC_SUBST(PRTE_EXTRA_LIB_LDFLAGS)

#
# Package/brand string
#
AC_MSG_CHECKING([if want package/brand string])
AC_ARG_WITH([package-string],
     [AS_HELP_STRING([--with-package-string=STRING],
                     [Use a branding string throughout Open MPI])])
if test "$with_package_string" = "" || test "$with_package_string" = "no"; then
    with_package_string="Open MPI $PRTE_CONFIGURE_USER@$PRTE_CONFIGURE_HOST Distribution"
fi
AC_DEFINE_UNQUOTED([PRTE_PACKAGE_STRING], ["$with_package_string"],
     [package/branding string for Open MPI])
AC_MSG_RESULT([$with_package_string])

#
# Ident string
#
AC_MSG_CHECKING([if want ident string])
AC_ARG_WITH([ident-string],
     [AS_HELP_STRING([--with-ident-string=STRING],
                     [Embed an ident string into Open MPI object files])])
if test "$with_ident_string" = "" || test "$with_ident_string" = "no"; then
    with_ident_string="%VERSION%"
fi
# This is complicated, because $PRTE_VERSION may have spaces in it.
# So put the whole sed expr in single quotes -- i.e., directly
# substitute %VERSION% for (not expanded) $PRTE_VERSION.
with_ident_string="`echo $with_ident_string | sed -e 's/%VERSION%/$PRTE_VERSION/'`"

# Now eval an echo of that so that the "$PRTE_VERSION" token is
# replaced with its value.  Enclose the whole thing in "" so that it
# ends up as 1 token.
with_ident_string="`eval echo $with_ident_string`"

AC_DEFINE_UNQUOTED([PRTE_IDENT_STRING], ["$with_ident_string"],
     [ident string for Open MPI])
AC_MSG_RESULT([$with_ident_string])


# some systems don't want/like getpwuid
AC_MSG_CHECKING([if want getpwuid support])
AC_ARG_ENABLE([getpwuid],
    [AS_HELP_STRING([--disable-getpwuid],
        [Disable getpwuid support (default: enabled)])])
if test "$enable_getpwuid" = "no"; then
    AC_MSG_RESULT([no])
    prte_want_getpwuid=0
else
    AC_MSG_RESULT([yes])
    prte_want_getpwuid=1
fi
AC_DEFINE_UNQUOTED([PRTE_ENABLE_GETPWUID], [$prte_want_getpwuid],
                   [Disable getpwuid support (default: enabled)])

dnl Check for FT
AC_MSG_CHECKING([if want fault tolerance support])
AC_ARG_ENABLE([prte-ft],
    [AS_HELP_STRING([--enable-prte-ft],
        [ENable PRRTE fault tolerance support (default: disabled)])])
if test "$enable_prte_ft" = "yes"; then
    AC_MSG_RESULT([yes])
    prte_enable_ft=1
    PRTE_SUMMARY_ADD([[Options]],[[Fault tolerance]], [prte_ft], [yes])
else
    AC_MSG_RESULT([no])
    prte_enable_ft=0
    PRTE_SUMMARY_ADD([[Options]],[[Fault tolerance]], [prte_ft], [no])
fi
AC_DEFINE_UNQUOTED([PRTE_ENABLE_FT], [$prte_enable_ft],
                   [Enable PRRTE fault tolerance support (default: disabled)])


])dnl
