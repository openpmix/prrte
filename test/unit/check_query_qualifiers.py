#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A query qualifier's value is the requesting client's, in type and in
# presence, and _query() must screen it before it reads the union.
#
# PMIx forwards a query's qualifier array to the host without inspecting
# what any entry holds, so reading a fixed member of pmix_value_t is
# reading whatever eight bytes the caller chose to put there.  Every
# string qualifier pmix_server_queries.c acts on is handed on to strlen(),
# strcmp(), PMIx_Check_nspace() or a session lookup, so a mistyped one is
# a fault that any process or tool attached to a daemon can produce with a
# single PMIx_Query_info.
#
# There is a second, quieter half.  A PMIX_STRING carrying no string
# survives the wire as a NULL - the packer writes a zero length and the
# unpacker hands back NULL - so accepting it leaves the arm's variable
# holding exactly the value that means "this qualifier was not given",
# and "not given" has a meaning for all of them.  It is a different
# answer, not an error: a NULL PMIX_HOSTNAME makes the server-URI arm
# report about this node, a NULL PMIX_ALLOC_ID makes the allocation arms
# report about the whole DVM.  PMIX_NSPACE is the one qualifier for which
# no string legitimately means "any", and it says so by passing nullok at
# its call.
#
# Both halves live in qual_string().  The compiler cannot require that a
# newly added string qualifier goes through it - reading the union
# directly compiles perfectly well - so it is required here.
#
# Deliberately not a list of qualifier keys: which keys the file handles
# changes, and a list of them would go stale silently.  What is checked is
# the *shape* - that the union is reached only through the screen, and
# that every call to the screen states which of the two policies it wants.

import os
import re
import sys

TARGET = os.path.join("src", "prted", "pmix", "pmix_server_queries.c")
SCREEN = "qual_string"
# the sole qualifier for which an absent string is a legitimate answer
NULLOK_KEY = "PMIX_NSPACE"
# how far back to look for the type test that makes a direct read safe
WINDOW = 240


def find_top():
    top = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    if not os.path.isdir(os.path.join(top, "src")) and "srcdir" in os.environ:
        top = os.path.join(os.environ["srcdir"], "..", "..")
    return top


def strip_comments(text):
    """Blank out comments, preserving offsets and line breaks."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def lineno(text, pos):
    return text.count("\n", 0, pos) + 1


def main():
    path = os.path.join(find_top(), TARGET)
    try:
        with open(path) as f:
            raw = f.read()
    except OSError as e:
        print("check_query_qualifiers: cannot read %s: %s" % (path, e))
        return 1

    text = strip_comments(raw)
    errors = []

    # 1. The union of a qualifier is reached only through the screen.  A
    #    direct read is allowed only where the same expression has just
    #    constrained the type - the verbose trace does exactly that.
    direct = re.compile(r"qualifiers\s*\[[^\]]*\]\s*\.\s*value\s*\.\s*data\s*\.")
    typed = re.compile(r"value\s*\.\s*type")
    for hit in direct.finditer(text):
        before = text[max(0, hit.start() - WINDOW):hit.start()]
        if typed.search(before):
            continue
        errors.append("%s:%d: reads a qualifier's value union directly; "
                      "take the string through %s(), which screens both a "
                      "wrong type and an absent one"
                      % (TARGET, lineno(text, hit.start()), SCREEN))

    # 2. Every call to the screen states which policy it wants, and only
    #    the one qualifier that has a meaning for an absent string may ask
    #    for it.
    call = re.compile(r"\b%s\s*\((.*?)\)\s*\)" % SCREEN, re.S)
    branch = re.compile(r"PMIX_CHECK_KEY\s*\([^)]*,\s*([A-Z_0-9]+)\s*\)")
    calls = 0
    nullok_calls = 0
    for hit in call.finditer(text):
        # the definition takes a named parameter, not a literal
        args = hit.group(1)
        if "bool" in args:
            continue
        calls += 1
        last = args.rsplit(",", 1)[-1].strip()
        if last not in ("true", "false"):
            errors.append("%s:%d: calls %s() without stating whether an "
                          "absent string is acceptable; pass true or false"
                          % (TARGET, lineno(text, hit.start()), SCREEN))
            continue
        if "false" == last:
            continue
        nullok_calls += 1
        # which qualifier is this branch handling?
        keys = branch.findall(text[:hit.start()])
        if not keys or keys[-1] != NULLOK_KEY:
            errors.append("%s:%d: accepts an absent string for %s; only %s "
                          "has a meaning for one - every other qualifier "
                          "read as unset is a different answer, not an error"
                          % (TARGET, lineno(text, hit.start()),
                             keys[-1] if keys else "an unknown qualifier",
                             NULLOK_KEY))

    if 0 == calls:
        errors.append("%s: no %s() call sites found - has the screen been "
                      "renamed or removed?" % (TARGET, SCREEN))
    elif 1 < nullok_calls:
        errors.append("%s: %d call sites accept an absent string; only the "
                      "%s qualifier may" % (TARGET, nullok_calls, NULLOK_KEY))

    if errors:
        for e in errors:
            print(e)
        print("\n%d violation(s).  See the comment above %s() in %s."
              % (len(errors), SCREEN, TARGET))
        return 1

    print("check_query_qualifiers: %s screens every qualifier it reads "
          "(%d call sites)" % (TARGET, calls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
