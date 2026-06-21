#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
"""Offline mapping test harness for PRRTE's rmaps framework.

This driver exercises ``prterun --rtos donotlaunch --display map`` over a
matrix of ``--map-by`` / ``--rank-by`` / ``--bind-to`` directives, crossed
with every hwloc topology XML found in ``test/topologies/``, parses each
printed map, and verifies it against invariants derived from the topology
(no launching, no DVM).  See ``SPEC.rst`` and ``IMPL-PLAN.rst`` in this
directory for the full design.

Exit status: 0 = all pass, 1 = a failure, 77 = prerequisites missing (the
Automake "skip" convention).
"""

import argparse
import fnmatch
import glob
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field

SKIP = 77

# ---------------------------------------------------------------------------
# Directive vocabulary and the displayed-policy calibration (observed from
# prterun's own output against this build).  See SPEC "supported directive
# vocabulary".
# ---------------------------------------------------------------------------

# topology-level mappers used by the blanket matrix
MAP_BY = ["slot", "node", "package", "numa", "l3cache", "l2cache",
          "l1cache", "core", "hwthread"]
RANK_BY = ["slot", "node", "fill", "span"]
BIND_TO = ["none", "hwthread", "core", "l1cache", "l2cache", "l3cache",
           "numa", "package"]

# object-level mappers (everything except slot/node)
OBJECT_MAPS = {"package", "numa", "l3cache", "l2cache", "l1cache",
               "core", "hwthread"}

# Binding "fineness" rank (smaller == finer/deeper).  hwthread and core share
# rank 0 on a 1-PU/core topology.  PRRTE rejects binding to an object that
# lies *above* (coarser than) the mapped object, so for an object-level map a
# bind is accepted only when its rank is <= the map's rank.  map-by slot/node
# accept any bind.  (Rule derived empirically from prterun; see SPEC.)
FINENESS = {"hwthread": 0, "core": 0, "l1cache": 1, "l2cache": 2,
            "l3cache": 3, "numa": 4, "package": 5}


def bind_compatible(map_by, bind_to):
    if bind_to == "none" or bind_to is None:
        return True
    if map_by in ("slot", "node") or map_by is None:
        return True
    if map_by not in FINENESS:
        return True  # non-object map (ppr/seq/...): leave to prterun
    return FINENESS[bind_to] <= FINENESS[map_by]

# map directive -> hwloc object level used for placement/spans
MAP_LEVEL = {"package": "Package", "numa": "NUMANode", "l3cache": "L3Cache",
             "l2cache": "L2Cache", "l1cache": "L1Cache", "core": "Core",
             "hwthread": "PU"}
BIND_LEVEL = {"hwthread": "PU", "core": "Core", "l1cache": "L1Cache",
              "l2cache": "L2Cache", "l3cache": "L3Cache", "numa": "NUMANode",
              "package": "Package"}

# Displayed "Mapping policy" base string per directive (core==hwthread on a
# 1-PU/core topology -- the display collapses them to BYHWTHREAD).
MAP_DISPLAY = {"slot": "BYSLOT", "node": "BYNODE", "package": "BYPACKAGE",
               "numa": "BYNUMA", "l3cache": "BYL3CACHE", "l2cache": "BYL2CACHE",
               "l1cache": "BYL1CACHE", "core": "BYHWTHREAD",
               "hwthread": "BYHWTHREAD"}

# Displayed "Binding policy" string per directive.  --bind-to core reports as
# HWTHREAD on a 1-PU/core topology, so both are accepted for core/hwthread.
BIND_DISPLAY = {"none": {"NONE"}, "hwthread": {"HWTHREAD", "CORE"},
                "core": {"HWTHREAD", "CORE"}, "l1cache": {"L1CACHE"},
                "l2cache": {"L2CACHE"}, "l3cache": {"L3CACHE"},
                "numa": {"NUMA"}, "package": {"PACKAGE"}}

JOBMAP_MARKER = "JOB MAP"


# ===========================================================================
# Topology model (Phase 3)
# ===========================================================================

# hwloc object levels we model
TOPO_LEVELS = ["Machine", "Package", "NUMANode", "L3Cache", "L2Cache",
               "L1Cache", "Core", "PU"]


def parse_cpuset(s):
    """Turn an hwloc cpuset string into an integer bitmask.

    The format is comma-separated 32-bit hex words, most-significant first.
    An empty field is a zero word (e.g. ``0x00000001,,,,,0x0`` is bit 160),
    so empties still shift the accumulator -- they must not be skipped.
    """
    if not s:
        return 0
    val = 0
    for word in s.split(","):
        word = word.strip()
        val = (val << 32) | ((int(word, 16) & 0xFFFFFFFF) if word else 0)
    return val


@dataclass
class Obj:
    level: str
    os_index: int
    logical: int
    cpuset: int
    core_logical: frozenset = field(default_factory=frozenset)


class TopoModel:
    """Per-topology model derived entirely from the XML -- no hardcoded
    dimensions."""

    def __init__(self, name, by_level):
        self.name = name
        self.by_level = by_level
        ncore = len(by_level["Core"])
        npu = len(by_level["PU"])
        self.pus_per_core = (npu // ncore) if ncore else 1
        self.hwthread_eq_core = (self.pus_per_core == 1)
        self.all_cores = frozenset(o.logical for o in by_level["Core"])

    @classmethod
    def from_xml(cls, path):
        root = ET.parse(path).getroot()
        by_level = {lvl: [] for lvl in TOPO_LEVELS}
        counters = {lvl: 0 for lvl in TOPO_LEVELS}
        for el in root.iter("object"):
            lvl = el.get("type")
            if lvl not in by_level:
                continue
            oi = el.get("os_index")
            o = Obj(level=lvl,
                    os_index=int(oi) if oi is not None else -1,
                    logical=counters[lvl],
                    cpuset=parse_cpuset(el.get("cpuset")))
            by_level[lvl].append(o)
            counters[lvl] += 1
        # assign each object the set of logical core ids contained in its cpuset
        cores = [(c.cpuset, c.logical) for c in by_level["Core"]]
        for lvl, objs in by_level.items():
            for o in objs:
                if lvl == "Core":
                    o.core_logical = frozenset([o.logical])
                else:
                    o.core_logical = frozenset(
                        cl for (cb, cl) in cores
                        if cb and (cb & o.cpuset) == cb)
        return cls(os.path.splitext(os.path.basename(path))[0], by_level)

    def objects_at(self, level):
        return self.by_level.get(level, [])

    def count(self, level):
        return len(self.by_level.get(level, []))

    def spans_at(self, level):
        """Set of frozensets of logical core ids, one per object at level."""
        return {o.core_logical for o in self.objects_at(level)}


# ===========================================================================
# Map output parsing (Phase 2)
# ===========================================================================

HEADER_RE = re.compile(
    r"Mapping policy:\s*(\S+)\s+Ranking policy:\s*(\S+)\s+Binding policy:\s*(\S+)")
CPUTYPE_RE = re.compile(r"Cpu Type:\s*(\S+)")
NODE_RE = re.compile(
    r"Data for node:\s*(\S+)\s+Num slots:\s*(\d+)\s+Max slots:\s*(\d+)\s+Num procs:\s*(\d+)")
PROC_RE = re.compile(
    r"App:\s*(\d+)\s+Process rank:\s*(\d+)\s+Bound:\s*(.+?)\s*$")
BOUND_RE = re.compile(r"(\w+)\[(\d+)\]\[(\w+):L(\d+)(?:-(\d+))?\]")
JOBID_RE = re.compile(r"prterun-\S+@\d+")


class ParseError(Exception):
    pass


@dataclass
class BoundSpec:
    obj: str
    os_index: int
    child_level: str
    lo: int
    hi: int

    def core_set(self):
        return frozenset(range(self.lo, self.hi + 1))


@dataclass
class Proc:
    app: int
    rank: int
    bound: object  # BoundSpec or None


@dataclass
class NodeMap:
    name: str
    slots: int
    maxslots: int
    numprocs: int
    procs: list = field(default_factory=list)


@dataclass
class ParsedMap:
    map_base: str
    map_mods: list
    rank_policy: str
    bind_policy: str
    cpu_type: str
    nodes: list


def _parse_bound(text):
    text = text.strip()
    if text == "N/A":
        return None
    m = BOUND_RE.search(text)
    if not m:
        raise ParseError("unrecognized Bound field: %r" % text)
    lo = int(m.group(4))
    hi = int(m.group(5)) if m.group(5) is not None else lo
    return BoundSpec(m.group(1), int(m.group(2)), m.group(3), lo, hi)


def parse_map(raw):
    if raw.count(JOBMAP_MARKER) != 1:
        raise ParseError("expected exactly one JOB MAP block")
    hm = HEADER_RE.search(raw)
    if not hm:
        raise ParseError("no policy header found")
    map_field = hm.group(1)
    parts = map_field.split(":")
    map_base, map_mods = parts[0], parts[1:]
    cm = CPUTYPE_RE.search(raw)
    cpu_type = cm.group(1) if cm else ""
    nodes = []
    cur = None
    for line in raw.splitlines():
        nm = NODE_RE.search(line)
        if nm:
            cur = NodeMap(nm.group(1), int(nm.group(2)), int(nm.group(3)),
                          int(nm.group(4)))
            nodes.append(cur)
            continue
        pm = PROC_RE.search(line)
        if pm and cur is not None:
            cur.procs.append(Proc(int(pm.group(1)), int(pm.group(2)),
                                  _parse_bound(pm.group(3))))
    return ParsedMap(map_base, map_mods, hm.group(2), hm.group(3), cpu_type,
                     nodes)


def normalize(raw):
    """Strip non-deterministic noise so two runs of the same case compare
    equal (used for golden snapshots)."""
    out = []
    for line in raw.splitlines():
        if "compiled against libxml" in line:
            continue
        if line.startswith("Warning: This map has been generated"):
            break  # drop the trailing DONOTLAUNCH paragraph + footer
        line = JOBID_RE.sub("prterun-JOBID@N", line)
        line = re.sub(r"Total slots allocated \d+",
                      "Total slots allocated N", line)
        out.append(line.rstrip())
    return "\n".join(out).rstrip() + "\n"


# ===========================================================================
# Invocation and classification (Phase 1)
# ===========================================================================

@dataclass
class RunResult:
    argv: list
    rc: int
    out: str
    mapped: bool
    banner: str = None


def locate_prterun(top_builddir):
    env = os.environ.get("PRTE_PRTERUN")
    if env and os.path.exists(env):
        return env
    from shutil import which
    found = which("prterun")
    if found:
        return found
    if top_builddir:
        prte = os.path.join(top_builddir, "src", "tools", "prte", "prte")
        if os.path.exists(prte):
            link = os.path.join(os.path.dirname(prte), "prterun")
            if not os.path.exists(link):
                try:
                    os.symlink("prte", link)
                except OSError:
                    return prte
            return link
    return None


def build_argv(prterun, topo_path, case):
    argv = [prterun, "--rtos", "donotlaunch", "--display", "map",
            "--prtemca", "hwloc_use_topo_file", topo_path,
            "-H", case.hostspec]
    if case.map_by is not None:
        argv += ["--map-by", case.map_by]
    if case.rank_by is not None:
        argv += ["--rank-by", case.rank_by]
    if case.bind_to is not None:
        argv += ["--bind-to", case.bind_to]
    argv += list(case.extra_args)
    if case.apps:
        first = True
        for (n, exe) in case.apps:
            if not first:
                argv += [":"]
            argv += ["-n", str(n), exe]
            first = False
    else:
        argv += ["-n", str(case.n), "hostname"]
    return argv


def classify(out):
    mapped = out.count(JOBMAP_MARKER) == 1
    banner = None
    if not mapped:
        m = re.search(r"Topic:\s*(\S+)", out)
        if m:
            banner = m.group(1)
        else:
            m = re.search(r"(Valid directives:.*|.*not supported.*)", out)
            if m:
                banner = m.group(1).strip()
            else:
                stripped = [l for l in out.splitlines() if l.strip()]
                banner = stripped[-1].strip() if stripped else ""
    return mapped, banner


def run_case(prterun, topo_path, case, timeout=60):
    argv = build_argv(prterun, topo_path, case)
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout,
                          universal_newlines=True)
    mapped, banner = classify(proc.stdout)
    return RunResult(argv, proc.returncode, proc.stdout, mapped, banner)


# ===========================================================================
# Case model and generation (Phase 4)
# ===========================================================================

# layout name -> (-H spec, ordered list of (node, slots))
LAYOUTS = {
    "single": ("node0:8", [("node0", 8)]),
    "even": ("node0:8,node1:8,node2:8",
             [("node0", 8), ("node1", 8), ("node2", 8)]),
    "uneven": ("node0:8,node1:4", [("node0", 8), ("node1", 4)]),
}


@dataclass
class Case:
    id: str
    group: str
    topo: TopoModel
    layout: str
    hostspec: str
    pool: list           # [(node, slots)]
    map_by: str = None
    rank_by: str = None
    bind_to: str = None
    n: int = 1
    apps: list = field(default_factory=list)
    extra_args: tuple = ()
    expect: str = "map"          # "map" | "reject"
    expect_banner: str = None    # substring expected on reject


def _expect_for(map_by, rank_by, bind_to):
    # rank-by fill/span require an object-level map (must-map-by-obj).  This
    # only arises for slot/node maps, where every bind is accepted, so it is
    # an unambiguous reject reason.
    if rank_by in ("fill", "span") and map_by not in OBJECT_MAPS:
        return ("reject", "must-map-by-obj")
    # binding above an object-level map is rejected.
    if not bind_compatible(map_by, bind_to):
        return ("reject", "lies above the mapping object")
    return ("map", None)


def matrix_cases(topo, layouts, ns):
    for layout in layouts:
        hostspec, pool = LAYOUTS[layout]
        for n in ns:
            for m in MAP_BY:
                for r in RANK_BY:
                    for b in BIND_TO:
                        expect, banner = _expect_for(m, r, b)
                        cid = "matrix.%s.%s.m-%s.r-%s.b-%s.n%d" % (
                            topo.name, layout, m, r, b, n)
                        yield Case(cid, "matrix", topo, layout, hostspec, pool,
                                   map_by=m, rank_by=r, bind_to=b, n=n,
                                   expect=expect, expect_banner=banner)


def negative_cases(topo):
    hostspec, pool = LAYOUTS["even"]
    bad = [
        ("badbind", dict(bind_to="bogus"), "Valid directives"),
        ("badmap", dict(map_by="bogus"), "Valid directives"),
        ("badrank", dict(rank_by="bogus"), "Valid directives"),
        ("rankcore", dict(rank_by="core"), "Valid directives"),
        ("dfltmod", dict(map_by="core:nooversubscribe"), "not supported"),
    ]
    for name, kw, banner in bad:
        cid = "negative.%s.%s" % (topo.name, name)
        yield Case(cid, "negative", topo, "even", hostspec, pool, n=4,
                   expect="reject", expect_banner=banner, **kw)


def group_cases(topo):
    # oversubscribe: more procs than slots (the default policy already permits
    # oversubscription -- the OVERSUBSCRIBE modifier cannot be a default).
    hostspec, pool = LAYOUTS["uneven"]   # 12 slots
    total = sum(s for _, s in pool)
    yield Case("group.%s.oversub" % topo.name, "oversubscribe", topo,
               "uneven", hostspec, pool, map_by="core", rank_by="slot",
               bind_to="core", n=total + 4, expect="map")

    # ppr: 2 procs per package on a single node (npkg packages -> 2*npkg procs)
    hostspec, pool = LAYOUTS["single"]
    npkg = topo.count("Package")
    yield Case("group.%s.ppr" % topo.name, "ppr", topo, "single", hostspec,
               pool, map_by="ppr:2:package", rank_by="slot", bind_to="package",
               n=2 * npkg, expect="map")

    # multi-app: two app contexts
    hostspec, pool = LAYOUTS["even"]
    yield Case("group.%s.multiapp" % topo.name, "multiapp", topo, "even",
               hostspec, pool, map_by="core", rank_by="slot", bind_to="core",
               apps=[(2, "hostname"), (3, "hostname")], expect="map")


def generate_cases(topos, layouts, ns, full):
    cases = []
    for topo in topos:
        cases.extend(matrix_cases(topo, layouts, ns))
        cases.extend(negative_cases(topo))
        cases.extend(group_cases(topo))
    return cases


# ===========================================================================
# Invariants (Phase 5)
# ===========================================================================

def _flat_procs(pmap):
    out = []
    for node in pmap.nodes:
        for p in node.procs:
            out.append((node, p))
    return out


def _node_counts(pmap):
    return [(node.name, len(node.procs)) for node in pmap.nodes]


def _expected_block_fill(pool, n):
    """byslot/object placement: fill each node's slots in pool order."""
    counts = {}
    rem = n
    for name, slots in pool:
        take = min(slots, rem)
        if take > 0:
            counts[name] = take
            rem -= take
    return counts  # nodes that get 0 are omitted


def _expected_rr_nodes(pool, n):
    """bynode placement: round-robin across nodes in order."""
    counts = {name: 0 for name, _ in pool}
    i = 0
    for _ in range(n):
        counts[pool[i % len(pool)][0]] += 1
        i += 1
    return {k: v for k, v in counts.items() if v > 0}


def _expected_rank_by_node(node_order, counts_by_name, n):
    """Round-robin rank assignment across nodes honoring per-node capacity."""
    rem = dict(counts_by_name)
    sets = {name: set() for name in node_order}
    order = list(node_order)
    r = 0
    idx = 0
    placed = 0
    guard = 0
    while placed < n and guard < n * (len(order) + 1) + 1:
        name = order[idx % len(order)]
        idx += 1
        guard += 1
        if rem.get(name, 0) > 0:
            sets[name].add(r)
            rem[name] -= 1
            r += 1
            placed += 1
    return sets


def check_universal(case, pmap):
    v = []
    n = case.n if not case.apps else sum(a for a, _ in case.apps)
    flat = _flat_procs(pmap)

    # U1 policy strings
    if case.map_by and ":" not in case.map_by and case.map_by in MAP_DISPLAY:
        exp = MAP_DISPLAY[case.map_by]
        if pmap.map_base != exp:
            v.append(("U1", "map policy %s != expected %s"
                      % (pmap.map_base, exp)))
    if case.rank_by and case.rank_by in ("slot", "node", "fill", "span"):
        if pmap.rank_policy != case.rank_by.upper():
            v.append(("U1", "rank policy %s != %s"
                      % (pmap.rank_policy, case.rank_by.upper())))
    if case.bind_to and case.bind_to in BIND_DISPLAY:
        if pmap.bind_policy not in BIND_DISPLAY[case.bind_to]:
            v.append(("U1", "bind policy %s not in %s"
                      % (pmap.bind_policy, BIND_DISPLAY[case.bind_to])))

    # U2 total procs
    total = len(flat)
    if total != n:
        v.append(("U2", "placed %d procs, expected %d" % (total, n)))

    # U3 ranks are a permutation of 0..n-1
    ranks = sorted(p.rank for _, p in flat)
    if ranks != list(range(n)):
        v.append(("U3", "ranks %s != 0..%d" % (ranks, n - 1)))

    # U4 per-node Num procs matches listed
    for node in pmap.nodes:
        if node.numprocs != len(node.procs):
            v.append(("U4", "%s Num procs=%d but %d listed"
                      % (node.name, node.numprocs, len(node.procs))))

    # U5 app indices
    if case.apps:
        valid = set(range(len(case.apps)))
    else:
        valid = {0}
    for _, p in flat:
        if p.app not in valid:
            v.append(("U5", "rank %d has App %d not in %s"
                      % (p.rank, p.app, valid)))
            break

    # U7 bound references existing cores
    for _, p in flat:
        if p.bound is not None:
            cs = p.bound.core_set()
            if not cs <= case.topo.all_cores:
                v.append(("U7", "rank %d bound to cores %s outside topology"
                          % (p.rank, sorted(cs))))
                break
    return v


def check_mapping(case, pmap):
    v = []
    if case.apps:
        return v  # multi-app placement shape is pinned by golden snapshots
    if not case.map_by or ":" in case.map_by or case.map_by not in MAP_BY:
        return v
    n = case.n
    total_slots = sum(s for _, s in case.pool)
    actual = dict(_node_counts(pmap))
    if case.map_by == "node":
        # bynode: round-robin across nodes
        exp = _expected_rr_nodes(case.pool, n)
        if actual != exp:
            v.append(("M-node", "node counts %s != round-robin %s"
                      % (actual, exp)))
    elif case.map_by == "slot":
        # byslot: block-fill nodes in pool order (only checkable up to slots;
        # past total slots the oversubscribe spread is left to golden)
        if n <= total_slots:
            exp = _expected_block_fill(case.pool, n)
            if actual != exp:
                v.append(("M-slot",
                          "node counts %s != block-fill %s" % (actual, exp)))
    else:
        # object-level maps (non-span) are node-local: all procs land on the
        # first node, round-robin across that node's objects, oversubscribing
        # rather than spilling to the next node.  (--map-by node / :SPAN are
        # the directives that spread across nodes.)
        first = case.pool[0][0]
        if actual != {first: n}:
            v.append(("M-%s" % case.map_by,
                      "node counts %s != node-local {%s: %d}"
                      % (actual, first, n)))
    return v


def check_ranking(case, pmap):
    v = []
    if case.apps:
        return v  # multi-app ranking shape is pinned by golden snapshots
    if not case.rank_by or (case.map_by and ":" in case.map_by):
        return v
    n = case.n
    flat = _flat_procs(pmap)
    if case.rank_by == "slot":
        order = [p.rank for _, p in flat]
        if order != list(range(n)):
            v.append(("R-slot", "ranks in placement order %s != 0..%d"
                      % (order, n - 1)))
    elif case.rank_by == "node":
        node_order = [node.name for node in pmap.nodes]
        counts = {node.name: len(node.procs) for node in pmap.nodes}
        exp = _expected_rank_by_node(node_order, counts, n)
        actual = {node.name: set(p.rank for p in node.procs)
                  for node in pmap.nodes}
        if actual != exp:
            v.append(("R-node", "rank sets %s != round-robin %s"
                      % (actual, exp)))
    # fill/span cross-node shape is pinned by golden snapshots, not asserted
    return v


def check_binding(case, pmap):
    v = []
    if not case.bind_to:
        return v
    flat = _flat_procs(pmap)
    if case.bind_to == "none":
        for _, p in flat:
            if p.bound is not None:
                v.append(("B-none", "rank %d is bound but bind-to none"
                          % p.rank))
                break
        return v
    level = BIND_LEVEL[case.bind_to]
    spans = case.topo.spans_at(level)
    for _, p in flat:
        if p.bound is None:
            v.append(("B-%s" % case.bind_to, "rank %d unbound" % p.rank))
            break
        if p.bound.core_set() not in spans:
            v.append(("B-%s" % case.bind_to,
                      "rank %d bound to cores %s, not a %s span"
                      % (p.rank, sorted(p.bound.core_set()), level)))
            break
    return v


def check_case(case, pmap):
    violations = []
    violations += check_universal(case, pmap)
    violations += check_mapping(case, pmap)
    violations += check_ranking(case, pmap)
    violations += check_binding(case, pmap)
    return violations


# ===========================================================================
# Reporting and orchestration (Phase 6 / 7)
# ===========================================================================

def golden_path(golden_dir, case):
    return os.path.join(golden_dir, case.topo.name, case.id + ".map")


def is_curated_golden(c):
    """A small, human-reviewable subset pinned by golden snapshots."""
    if c.group in ("oversubscribe", "ppr", "multiapp"):
        return True
    if c.group == "matrix" and c.expect == "map":
        # one representative per map-by (even layout, rank slot, bind none)
        if (c.layout == "even" and c.n == 7 and c.rank_by == "slot"
                and c.bind_to == "none"):
            return True
        # ranking + binding variety on the core map
        if (c.layout == "even" and c.n == 7 and c.map_by == "core"
                and c.bind_to == "core"):
            return True
    return False


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--topo", action="append", default=[],
                    help="explicit topology XML (repeatable); overrides discovery")
    ap.add_argument("--topo-dir", default=None,
                    help="directory to discover *.xml topologies in")
    ap.add_argument("--filter", default=None,
                    help="glob over case ids to select a subset")
    ap.add_argument("--layouts", default=None,
                    help="comma list of layouts (default: even,uneven; full adds single)")
    ap.add_argument("--full", action="store_true",
                    help="run the full layout x N cross product")
    ap.add_argument("--list", action="store_true",
                    help="list case ids without running")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--golden", action="store_true",
                    help="compare normalized output to golden snapshots")
    ap.add_argument("--update-golden", action="store_true",
                    help="regenerate golden snapshots")
    ap.add_argument("--golden-dir", default=None)
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    top_srcdir = os.environ.get("top_srcdir") or os.path.dirname(here)
    top_builddir = os.environ.get("top_builddir")
    golden_dir = args.golden_dir or os.path.join(here, "golden")

    # discover topologies
    if args.topo:
        topo_paths = args.topo
    else:
        topo_dir = (args.topo_dir
                    or os.path.join(top_srcdir, "test", "topologies"))
        topo_paths = sorted(glob.glob(os.path.join(topo_dir, "*.xml")))
    if not topo_paths:
        print("SKIP: no topology XML files found", file=sys.stderr)
        return SKIP

    prterun = locate_prterun(top_builddir)
    if not prterun:
        print("SKIP: no prterun/prte executable found", file=sys.stderr)
        return SKIP

    topos = []
    for p in topo_paths:
        try:
            topos.append(TopoModel.from_xml(p))
        except Exception as e:  # noqa
            print("SKIP: cannot parse topology %s: %s" % (p, e),
                  file=sys.stderr)
            return SKIP
    topo_path_by_name = {t.name: p for t, p in zip(topos, topo_paths)}

    if args.layouts:
        layouts = args.layouts.split(",")
        ns = [7, 12]
    elif args.full:
        layouts = ["single", "even", "uneven"]
        ns = [7, 12]
    else:
        layouts = ["even", "uneven"]
        ns = [7]

    cases = generate_cases(topos, layouts, ns, args.full)
    if args.filter:
        cases = [c for c in cases if fnmatch.fnmatch(c.id, args.filter)]

    if args.list:
        for c in cases:
            print(c.id)
        print("# %d cases" % len(cases))
        return 0

    print("prterun: %s" % prterun)
    print("topologies: %s" % ", ".join(t.name for t in topos))
    print("cases: %d (layouts=%s, ns=%s)\n" % (len(cases), layouts, ns))

    n_pass = n_fail = n_skip = 0
    start = time.time()

    for c in cases:
        topo_path = topo_path_by_name[c.topo.name]
        try:
            res = run_case(prterun, topo_path, c)
        except Exception as e:  # noqa
            print("FAIL %s harness-error: %s" % (c.id, e))
            n_fail += 1
            continue

        if c.expect == "reject":
            if res.mapped:
                print("FAIL %s expected-reject but produced a map" % c.id)
                _dump(res)
                n_fail += 1
            elif c.expect_banner and (c.expect_banner not in res.out):
                print("FAIL %s rejected but banner %r not found"
                      % (c.id, c.expect_banner))
                _dump(res)
                n_fail += 1
            else:
                n_pass += 1
                if args.verbose:
                    print("PASS %s (rejected: %s)" % (c.id, res.banner))
            continue

        # expect a map
        if not res.mapped:
            print("FAIL %s expected a map but was rejected: %s"
                  % (c.id, res.banner))
            _dump(res)
            n_fail += 1
            continue

        try:
            pmap = parse_map(res.out)
        except ParseError as e:
            print("FAIL %s parse-error: %s" % (c.id, e))
            _dump(res)
            n_fail += 1
            continue

        violations = check_case(c, pmap)

        if (args.update_golden or args.golden) and is_curated_golden(c):
            gp = golden_path(golden_dir, c)
            if args.update_golden:
                os.makedirs(os.path.dirname(gp), exist_ok=True)
                with open(gp, "w") as f:
                    f.write(normalize(res.out))
            elif not os.path.exists(gp):
                violations.append(("golden", "no golden file %s" % gp))
            else:
                with open(gp) as f:
                    want = f.read()
                if normalize(res.out) != want:
                    violations.append(("golden", "output differs from golden"))

        if violations:
            for inv, msg in violations:
                print("FAIL %s %s: %s" % (c.id, inv, msg))
            _dump(res)
            n_fail += 1
        else:
            n_pass += 1
            if args.verbose:
                print("PASS %s" % c.id)

    elapsed = time.time() - start
    print("\n==== %d pass, %d fail, %d skip in %.1fs ===="
          % (n_pass, n_fail, n_skip, elapsed))
    return 1 if n_fail else 0


def _dump(res):
    print("    cmd: %s" % " ".join(res.argv))
    for line in normalize(res.out).splitlines():
        print("    | %s" % line)


if __name__ == "__main__":
    sys.exit(main())
