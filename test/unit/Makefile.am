# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

SUBDIRS = rmaps runtime errmgr ess event filem grpcomm hwloc include odls iof plm pmix prted rml ras schizo state tools util

# A source scan rather than a program, so it is distributed as-is and
# needs no configure substitution - see the header of the script. It is
# kept identical to the copy in PMIx (test/unit/check_static_init.pl);
# fix one and copy it across rather than letting the two drift.
dist_check_SCRIPTS = check_static_init.pl

TESTS = check_static_init.pl
