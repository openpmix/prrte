#!/usr/bin/env python3
#
# Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Every proc added to a job's proc table goes through prte_job_add_proc().
#
# Once the mapper has placed a job, its table holds every proc the job will
# have, and the master marks it so (prte_job_t::procs_fixed).
# prte_job_add_proc() refuses to add to a marked job.  Code that relies on
# a job's membership being fixed - the connection registry addresses a job
# connected in full by its wildcard rank - is then sound for as long as
# nothing writes the table directly.  The table is an ordinary pointer
# array the compiler cannot protect, so this checks it.
#
# A write that stores NULL removes a proc and is allowed: a rank that is
# gone matches nothing.  Arrays named procs that belong to other structs
# are listed in OTHER_PROCS; add one there only if it cannot hold a job's
# procs.

import os
import re
import sys

HELPER_FILE = os.path.join("src", "runtime", "prte_globals.c")
HELPER = "prte_job_add_proc"

# proc arrays that are not a job's table
OTHER_PROCS = (
    re.compile(r"node->procs$"),        # prte_node_t: procs placed on a node
    re.compile(r"nptr->procs$"),
    re.compile(r"^&app->procs$"),       # prep_singleton's local app record
    re.compile(r"^&kill_batch->procs$"),  # odls kill batch
)

WRITE = re.compile(
    r"\bpmix_pointer_array_(set_item|add)\s*\(\s*"
    r"(?P<array>&?[A-Za-z_][A-Za-z0-9_>.\-]*procs)\s*,"
    r"(?P<rest>[^;]*?)\)\s*[;)]",
    re.S)


def find_top():
    top = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    if not os.path.isdir(os.path.join(top, "src")) and "srcdir" in os.environ:
        top = os.path.join(os.environ["srcdir"], "..", "..")
    return top


def strip_comments(text):
    # keep newlines so line numbers survive
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


def in_helper(text, pos):
    start = text.rfind("\nint " + HELPER + "(", 0, pos)
    if start < 0:
        return False
    return text.find("\n}", start) > pos


def main():
    top = find_top()
    src = os.path.join(top, "src")
    errors = []
    found_helper = False

    for root, _, files in os.walk(src):
        for name in files:
            if not name.endswith(".c"):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, top)
            try:
                with open(path) as f:
                    text = strip_comments(f.read())
            except OSError as e:
                print("check_job_procs: cannot read %s: %s" % (path, e))
                return 1
            for m in WRITE.finditer(text):
                array = re.sub(r"\s+", "", m.group("array"))
                value = m.group("rest").split(",")[-1].strip()
                if "NULL" == value:
                    continue
                if any(p.search(array) for p in OTHER_PROCS):
                    continue
                if rel == HELPER_FILE and in_helper(text, m.start()):
                    found_helper = True
                    continue
                n = text.count("\n", 0, m.start()) + 1
                errors.append("%s:%d: writes %s directly; use %s()"
                              % (rel, n, array, HELPER))

    if not found_helper:
        errors.append("%s: %s() no longer writes the table this check "
                      "expects" % (HELPER_FILE, HELPER))

    if errors:
        for e in errors:
            print(e)
        print("\n%d violation(s).  See the header of %s."
              % (len(errors), os.path.basename(__file__)))
        return 1

    print("check_job_procs: every proc added to a job goes through %s()"
          % HELPER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
