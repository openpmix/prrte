#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# An attribute key with only one end is a feature that does nothing.
#
# PRTE_JOB_NOTIFY_COMPLETION and PRTE_JOB_SILENT_TERMINATION were the two
# halves of one directive, and they never met: pmix_server_dyn.c recorded
# PMIX_NOTIFY_COMPLETION as the first, which nothing anywhere read, while
# state/dvm's dvm_notify() asked for the second, which nothing anywhere
# wrote.  Each half looked live in isolation.  A tool spawning with
# PMIX_NOTIFY_COMPLETION=false was told nothing and notified anyway, and the
# suppression logic written for it could not be reached from any input.
#
# Neither the compiler nor a test can see that: both halves compile, and a
# test would have to know the feature exists in order to ask for it.  What
# does see it is the observation that a key which is only ever written, or
# only ever read, cannot be doing anything.
#
# This also enforces the other half of the same lesson.  Booleans are
# three-state (src/util/attr.h): prte_get_attribute() answers "found / not
# found" through a bool and so cannot tell FALSE from NOT_SET, which is how
# PMIX_DO_NOT_LAUNCH=false came to mean "do not launch".  The generic
# accessors refuse PMIX_BOOL at runtime; this refuses it at build time,
# where it is cheaper to find.
#
# Exemptions are listed below with a reason apiece.  Adding one is a
# statement that the key really is write-only or read-only on purpose - not
# a way to quiet the check.

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ATTR_H = os.path.join(ROOT, "src", "util", "attr.h")

KEY_RE = re.compile(r"\b(PRTE_(?:JOB|APP|PROC|NODE)_[A-Z_0-9]+)\b")

# Walking the attribute list and comparing the key directly is a read too -
# it is how the envar directives are consumed (odls process_envars).  A bare
# "case PRTE_JOB_X:" deliberately does NOT count: prte_attr_key_to_str() has
# one for every key in the tree, and counting those would make every key look
# read and the check would find nothing ever again.
KEYCMP_RE = re.compile(
    r"->key\s*==\s*(PRTE_(?:JOB|APP|PROC|NODE)_[A-Z_0-9]+)"
    r"|(PRTE_(?:JOB|APP|PROC|NODE)_[A-Z_0-9]+)\s*==\s*\w+->key")

# Functions and macros that READ an attribute, and the argument position the
# key appears in (0-based).
READERS = {
    "prte_get_attribute": 1,
    "prte_get_bool_attribute": 1,
    "prte_fetch_attribute": 2,
    "PRTE_ATTR_IS_TRUE": 1,
}

# ...and that WRITE (or unset) one.  A wrapper that takes the key through
# and hands it to one of the above belongs here too, or its callers look
# like they never write the key.
WRITERS = {
    "prte_set_attribute": 1,
    "prte_set_bool_attribute": 1,
    "prte_remove_attribute": 1,
    "prte_add_attribute": 1,
    "prte_prepend_attribute": 1,
    "set_bool_option": 1,
}

# Keys that are legitimately one-ended.  Each needs a reason.
# test_util.c asserts that the generic accessors REFUSE a boolean, so it is
# the one file that must be able to call them that way.
BOOL_EXEMPT_FILES = {os.path.join("test", "unit", "util", "test_util.c")}

# Keys that are legitimately one-ended.  Empty, and meant to stay that way:
# an entry here is a feature whose other half is missing, so adding one is a
# statement that something is broken, not a way to quiet a failure.
EXEMPT = set()


def split_args(text):
    args, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    args.append(cur.strip())
    return args


def find_calls(body, fname):
    out = []
    for m in re.finditer(r"\b" + re.escape(fname) + r"\s*\(", body):
        i, depth = m.end(), 1
        while depth > 0 and i < len(body):
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
            i += 1
        out.append((m.start(), body[m.end():i - 1]))
    return out


def strip_comments(text):
    """Blank out comments and string literals so that prose describing a
    call - and there is plenty of it in this tree - is not read as one."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in '"\'':
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def sources():
    for base in ("src", "test"):
        for root, _dirs, files in os.walk(os.path.join(ROOT, base)):
            for f in files:
                if f.endswith((".c", ".h")):
                    yield os.path.join(root, f)


def main():
    if not os.path.isfile(ATTR_H):
        print("check_attr_pairing: cannot find %s" % ATTR_H)
        return 1

    declared = []
    for line in open(ATTR_H, errors="ignore"):
        m = re.match(r"#define\s+(PRTE_(?:JOB|APP|PROC|NODE)_[A-Z_0-9]+)\s+\(", line)
        if m:
            declared.append(m.group(1))

    read, written, boolviol = set(), set(), []

    for path in sources():
        body = strip_comments(open(path, errors="ignore").read())
        rel = os.path.relpath(path, ROOT)

        for fname, pos in list(READERS.items()) + list(WRITERS.items()):
            for start, inner in find_calls(body, fname):
                args = split_args(inner)
                if pos >= len(args):
                    continue
                for key in KEY_RE.findall(args[pos]):
                    (read if fname in READERS else written).add(key)

        for m in KEYCMP_RE.finditer(body):
            read.add(m.group(1) or m.group(2))

        # booleans must not go through the generic accessors
        for fname in ("prte_get_attribute", "prte_set_attribute"):
            if rel in BOOL_EXEMPT_FILES:
                continue
            for start, inner in find_calls(body, fname):
                args = split_args(inner)
                if args and args[-1] == "PMIX_BOOL":
                    line = body[:start].count("\n") + 1
                    boolviol.append("%s:%d: %s() with PMIX_BOOL - booleans are "
                                    "three-state; use prte_get_bool_attribute()/"
                                    "prte_set_bool_attribute()/PRTE_ATTR_IS_TRUE()"
                                    % (rel, line, fname))

    errors = list(boolviol)
    for key in declared:
        if key in EXEMPT:
            continue
        if key in read and key not in written:
            errors.append("%s is READ but never WRITTEN - the half that would "
                          "set it is missing, so it can never be true" % key)
        elif key in written and key not in read:
            errors.append("%s is WRITTEN but never READ - setting it does "
                          "nothing" % key)

    if errors:
        for e in errors:
            print(e)
        print("\n%d violation(s).  See the block comment at the top of %s."
              % (len(errors), os.path.relpath(__file__, ROOT)))
        return 1

    print("check_attr_pairing: %d attribute keys, every one both read and "
          "written; no boolean through the generic accessors" % len(declared))
    return 0


if __name__ == "__main__":
    sys.exit(main())
