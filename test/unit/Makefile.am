# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

SUBDIRS = rmaps runtime errmgr ess event filem grpcomm hwloc include odls iof plm pmix prted rml ras schizo state tools util

# Source scans rather than programs, so they are distributed as-is and
# need no configure substitution - see the header of each script.
# check_static_init.pl is kept identical to the copy in PMIx
# (test/unit/check_static_init.pl); fix one and copy it across rather
# than letting the two drift.
dist_check_SCRIPTS = check_static_init.pl check_query_authority.py

TESTS = check_static_init.pl check_query_authority.py
