dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2016-2018 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2016      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl
AC_DEFUN([PRRTE_SUMMARY_ADD],[
    PRRTE_VAR_SCOPE_PUSH([prrte_summary_section prrte_summary_line prrte_summary_section_current])

    dnl need to replace spaces in the section name with somethis else. _ seems like a reasonable
    dnl choice. if this changes remember to change PRRTE_PRINT_SUMMARY as well.
    prrte_summary_section=$(echo $1 | tr ' ' '_')
    prrte_summary_line="$2: $4"
    prrte_summary_section_current=$(eval echo \$prrte_summary_values_$prrte_summary_section)

    if test -z "$prrte_summary_section_current" ; then
        if test -z "$prrte_summary_sections" ; then
            prrte_summary_sections=$prrte_summary_section
        else
            prrte_summary_sections="$prrte_summary_sections $prrte_summary_section"
        fi
        eval prrte_summary_values_$prrte_summary_section=\"$prrte_summary_line\"
    else
        eval prrte_summary_values_$prrte_summary_section=\"$prrte_summary_section_current,$prrte_summary_line\"
    fi

    PRRTE_VAR_SCOPE_POP
])

AC_DEFUN([PRRTE_SUMMARY_PRINT],[
    PRRTE_VAR_SCOPE_PUSH([prrte_summary_section prrte_summary_section_name])
    cat <<EOF

PRRTE configuration:
-----------------------
Version: $PRRTE_MAJOR_VERSION.$PRRTE_MINOR_VERSION.$PRRTE_RELEASE_VERSION$PRRTE_GREEK_VERSION
EOF

    if test $WANT_DEBUG = 0 ; then
        echo "Debug build: no"
    else
        echo "Debug build: yes"
    fi

    if test ! -z $with_platform ; then
        echo "Platform file: $with_platform"
    else
        echo "Platform file: (none)"
    fi

    echo

    for prrte_summary_section in $(echo $prrte_summary_sections) ; do
        prrte_summary_section_name=$(echo $prrte_summary_section | tr '_' ' ')
        echo "$prrte_summary_section_name"
        echo "-----------------------"
        echo "$(eval echo \$prrte_summary_values_$prrte_summary_section)" | tr ',' $'\n' | sort -f
        echo " "
    done

    if test $WANT_DEBUG = 1 ; then
        cat <<EOF
*****************************************************************************
 THIS IS A DEBUG BUILD!  DO NOT USE THIS BUILD FOR PERFORMANCE MEASUREMENTS!
*****************************************************************************

EOF
    fi

    PRRTE_VAR_SCOPE_POP
])
