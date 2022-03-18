dnl -*- autoconf -*-
dnl
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2016-2020 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2016      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
dnl                         All Rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PRTE_SUMMARY_ADD(section, topic, unused, result)
#
# queue a summary line in the given section of the form:
#   topic: result
#
# section:topic lines are only added once; first to add wins.
# The key for uniqification is a shell-variable-ified representation
# of section followed by an underscore followed by a
# shell-variable-ified representation of line.
#
# There are no restrictions on the contents of section and topic; they
# can be variable references (although the use case for this is
# dubious) and they can contain most ASCII characters (escape
# characters excluded).  Note that some care must be taken with the
# unique check and this liberal rule, as the unique check is after the
# string has been run through AS_TR_SH.  Basically, any character that
# is not legal in a shell variable name will be turned into an
# underscore.  So the strings "Section_foo" and "Section-foo" would be
# the same as far as the unique check is concerned.
#
# The input strings are evaluated during PRTE_SUMMARY_ADD, not during
# PRTE_SUMMARY_PRINT.  This seems to meet the principle of least
# astonishment.  A common pattern is to clal
# PRTE_SUMMARY_ADD([Resource Type], [Component Name], [], [$results])
# and then unset $results to avoid namespace pollution.  This will
# work properly with the current behavior, but would result in odd
# errors if we delayed evaulation.
#
# As a historical note, the third argument has never been used in
# PRTE_SUMMARY_ADD and its meaning is unclear.  Preferred behavior is
# to leave it empty.
#
# As a final historical note, the initial version of SUMMARY_ADD was
# added with implementation of the callers having all of the section
# and topic headers double quoted.  This was never necessary, and
# certainly is not with the current implementation.  While harmless,
# it is not great practice to over-quote, so we recommend against
# doing so.
# -----------------------------------------------------------
AC_DEFUN([PRTE_SUMMARY_ADD],[
    PRTE_VAR_SCOPE_PUSH([prte_summary_line prte_summary_newline prte_summary_key])

    # The end quote on the next line is intentional!
    prte_summary_newline="
"
    prte_summary_line="$2: $4"
    prte_summary_key="AS_TR_SH([$1])_AS_TR_SH([$2])"

    # Use the section name variable as an indicator for whether or not
    # the section has already been created.
    AS_IF([AS_VAR_TEST_SET([prte_summary_section_]AS_TR_SH([$1])[_name])],
          [],
          [AS_VAR_SET([prte_summary_section_]AS_TR_SH([$1])[_name], ["$1"])
           PRTE_APPEND([prte_summary_sections], [AS_TR_SH([$1])])])

    # Use the summary key as indicator if the section:topic has already
    # been added to the results for the given section.
    AS_IF([AS_VAR_TEST_SET([${prte_summary_key}])],
          [],
          [AS_VAR_SET([${prte_summary_key}], [1])
           dnl this is a bit overcomplicated, but we are basically implementing
           dnl a poor mans AS_VAR_APPEND with the ability to specify a separator,
           dnl because we have a newline separator in the string.
           AS_IF([AS_VAR_TEST_SET([prte_summary_section_]AS_TR_SH([$1])[_value])],
                 [AS_VAR_APPEND([prte_summary_section_]AS_TR_SH([$1])[_value],
                                ["${prte_summary_newline}${prte_summary_line}"])],
                 [AS_VAR_SET([prte_summary_section_]AS_TR_SH([$1])[_value],
                             ["${prte_summary_line}"])])])

    PRTE_VAR_SCOPE_POP
])

AC_DEFUN([PRTE_SUMMARY_PRINT],[
    PRTE_VAR_SCOPE_PUSH([prte_summary_section prte_summary_section_name])
    cat <<EOF >&2

PRTE configuration:
-----------------------
Version: $PRTE_MAJOR_VERSION.$PRTE_MINOR_VERSION.$PRTE_RELEASE_VERSION$PRTE_GREEK_VERSION
EOF

    if test $WANT_DEBUG = 0 ; then
        echo "Debug build: no" >&2
    else
        echo "Debug build: yes" >&2
    fi

    if test ! -z $with_prte_platform ; then
        echo "Platform file: $with_prte_platform" >&2
    else
        echo "Platform file: (none)" >&2
    fi

    echo >&2

    for prte_summary_section in ${prte_summary_sections} ; do
        AS_VAR_COPY([prte_summary_section_name], [prte_summary_section_${prte_summary_section}_name])
        AS_VAR_COPY([prte_summary_section_value], [prte_summary_section_${prte_summary_section}_value])
        echo "${prte_summary_section_name}" >&2
        echo "-----------------------" >&2
        echo "${prte_summary_section_value}" | sort -f >&2
        echo " " >&2
    done

    if test $WANT_DEBUG = 1 ; then
        cat <<EOF >&2
*****************************************************************************
 THIS IS A DEBUG BUILD!  DO NOT USE THIS BUILD FOR PERFORMANCE MEASUREMENTS!
*****************************************************************************

EOF
    fi

    PRTE_VAR_SCOPE_POP
])
