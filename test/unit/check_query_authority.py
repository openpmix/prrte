#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A daemon must not answer a query out of state it does not have.
#
# A prted holds the identity half of the DVM's node table and nothing
# else: the nidmap ships node names, aliases, daemon vpids and pool slots
# (src/util/nidmap.c), and every writer of prte_node_t::slots, slots_max,
# slots_inuse and state runs only on the DVM master - ras/*, the hostfile
# and dash_host parsers, plm_base_setup_virtual_machine().  prte_sessions
# on a prted likewise holds the default session and nothing more.  A
# branch of _query() that reads either on a daemon gets a
# default-constructed zero, which is indistinguishable from a real answer
# and is returned to the client as one.
#
# pmix_server_queries.c therefore reaches that state only through the
# accessors in src/runtime/prte_globals.c, which return
# PRTE_ERR_NOT_AUTHORITATIVE off the master so the key can be relayed to
# it.  This checks that it still does.  The rule cannot be enforced by
# the compiler - the fields are ordinary members of a struct the file
# legitimately uses for node *identity* - so it is enforced here.
#
# Deliberately not a list of query keys.  Which keys need the master is
# not knowable in advance and goes stale the moment someone adds one;
# which *reads* cannot be satisfied locally is exactly the set below.

import os
import re
import sys

# fields of prte_node_t that the nidmap does not ship
CAPACITY = ("slots", "slots_max", "slots_inuse")
# the session table, which a daemon holds only the default entry of
SESSIONS = "prte_sessions"
# the sanctioned ways in
ACCESSORS = ("prte_get_allocated_nodes",
             "prte_get_allocation_session",
             "prte_get_allocation_sessions")

TARGET = os.path.join("src", "prted", "pmix", "pmix_server_queries.c")


def find_top():
    top = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    if not os.path.isdir(os.path.join(top, "src")) and "srcdir" in os.environ:
        top = os.path.join(os.environ["srcdir"], "..", "..")
    return top


def main():
    path = os.path.join(find_top(), TARGET)
    try:
        with open(path) as f:
            lines = f.readlines()
    except OSError as e:
        print("check_query_authority: cannot read %s: %s" % (path, e))
        return 1

    errors = []

    # The session table has no legitimate direct use here at all.
    for n, line in enumerate(lines, 1):
        if line.lstrip().startswith(("*", "/*", "//")):
            continue
        if re.search(r"\b%s\b" % SESSIONS, line):
            errors.append("%s:%d: names %s directly; use %s()"
                          % (TARGET, n, SESSIONS, ACCESSORS[2]))

    # Capacity fields may be read, but only downstream of an accessor in
    # the same query branch.  Branches are delimited by the key tests of
    # the if/else-if chain in _query(), so split on those and require any
    # chunk that reads capacity to have settled authority first.
    branch = re.compile(r"(?:\}\s*else\s+)?if\s*\(\s*PMIx_Check_key\s*\(")
    capacity = re.compile(r"->\s*(%s)\b" % "|".join(CAPACITY))

    chunks = []
    start = 0
    for n, line in enumerate(lines):
        if branch.search(line):
            chunks.append((start, lines[start:n]))
            start = n
    chunks.append((start, lines[start:]))

    for offset, chunk in chunks:
        text = "".join(chunk)
        hit = capacity.search(text)
        if hit is None:
            continue
        if any(a in text for a in ACCESSORS):
            continue
        n = offset + text[:hit.start()].count("\n") + 1
        errors.append("%s:%d: reads ->%s without first establishing "
                      "authority; call one of %s and defer on "
                      "PRTE_ERR_NOT_AUTHORITATIVE"
                      % (TARGET, n, hit.group(1), ", ".join(ACCESSORS)))

    if errors:
        for e in errors:
            print(e)
        print("\n%d violation(s).  See the block comment above "
              "query_complete() in %s." % (len(errors), TARGET))
        return 1

    print("check_query_authority: %s reaches master-only state only "
          "through the accessors" % TARGET)
    return 0


if __name__ == "__main__":
    sys.exit(main())
