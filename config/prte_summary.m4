dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2016-2020 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2016      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl
AC_DEFUN([PRTE_SUMMARY_ADD],[
    PRTE_VAR_SCOPE_PUSH([prte_summary_section prte_summary_line prte_summary_section_current])

    dnl need to replace spaces in the section name with somethis else. _ seems like a reasonable
    dnl choice. if this changes remember to change PRTE_PRINT_SUMMARY as well.
    prte_summary_section=$(echo $1 | tr ' ' '_')
    prte_summary_line="$2: $4"
    prte_summary_section_current=$(eval echo \$prte_summary_values_$prte_summary_section)

    if test -z "$prte_summary_section_current" ; then
        if test -z "$prte_summary_sections" ; then
            prte_summary_sections=$prte_summary_section
        else
            prte_summary_sections="$prte_summary_sections $prte_summary_section"
        fi
        eval prte_summary_values_$prte_summary_section=\"$prte_summary_line\"
    else
        eval prte_summary_values_$prte_summary_section=\"$prte_summary_section_current,$prte_summary_line\"
    fi

    PRTE_VAR_SCOPE_POP
])

AC_DEFUN([PRTE_SUMMARY_PRINT],[
    PRTE_VAR_SCOPE_PUSH([prte_summary_section prte_summary_section_name])
    cat <<EOF

PRTE configuration:
-----------------------
Version: $PRTE_MAJOR_VERSION.$PRTE_MINOR_VERSION.$PRTE_RELEASE_VERSION$PRTE_GREEK_VERSION
EOF

    if test $WANT_DEBUG = 0 ; then
        echo "Debug build: no"
    else
        echo "Debug build: yes"
    fi

    if test ! -z $with_prte_platform ; then
        echo "Platform file: $with_prte_platform"
    else
        echo "Platform file: (none)"
    fi

    echo

    for prte_summary_section in $(echo $prte_summary_sections) ; do
        prte_summary_section_name=$(echo $prte_summary_section | tr '_' ' ')
        echo "$prte_summary_section_name"
        echo "-----------------------"
        echo "$(eval echo \$prte_summary_values_$prte_summary_section)" | tr ',' $'\n' | sort -f
        echo " "
    done

    if test $WANT_DEBUG = 1 ; then
        cat <<EOF
*****************************************************************************
 THIS IS A DEBUG BUILD!  DO NOT USE THIS BUILD FOR PERFORMANCE MEASUREMENTS!
*****************************************************************************

EOF
    fi

    PRTE_VAR_SCOPE_POP
])
