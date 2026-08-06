#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
"""slurm-alloc -- hold a real SLURM allocation across many `docker exec` calls.

The problem this solves is entirely a property of the harness, not of SLURM.
A person testing PRRTE under SLURM writes::

    salloc -N 3 --ntasks-per-node=2 bash      # ... and works in that shell

Every command they then run inherits the allocation's environment, which is
what ``ras/slurm`` reads (``SLURM_JOBID``, ``SLURM_NODELIST``,
``SLURM_TASKS_PER_NODE``) and what ``plm/slurm`` needs before ``srun`` will
join the allocation rather than making a new one.  A test suite driving the
swarm from outside has no such shell: every case is its own ``docker exec``,
and an allocation created in one of them is gone by the next.

So this script does the same thing in two halves.  ``new`` runs a real
``salloc`` whose command is a shell that **dumps its own environment to a
file** and then sleeps, holding the allocation; ``env`` prints that captured
environment back as ``export`` lines for a later shell to eval.

The environment is *captured, never reconstructed*.  That distinction is the
whole point of this harness: a reconstruction would encode this author's
belief about which variables SLURM sets, and the interesting failures are
exactly where that belief is wrong.  (It already was: see AGENTS.md section
13 on ``SLURM_JOBID``.)  What ``env`` prints is what SLURM itself put in the
environment of a process running inside the allocation.

Subcommands::

    slurm-alloc new [--tag NAME] [--nodes N] [--nodelist LIST]
                    [--tasks-per-node T] [--exclusive] [--time MINS]
    slurm-alloc env [--tag NAME]        # export lines to eval
    slurm-alloc jobid [--tag NAME]      # the SLURM job id
    slurm-alloc nodes [--tag NAME]      # allocated nodes, comma separated
    slurm-alloc free [--tag NAME|--all] # scancel it
    slurm-alloc pool                    # nodes with nothing allocated on them
    slurm-alloc jobs                    # every job SLURM currently knows
    slurm-alloc wait <jobid> [secs]     # block until the job is RUNNING

State (the captured environments) lives in $SLURM_HARNESS_STATE, default
/tmp/slurm-harness.
"""

import argparse
import os
import shlex
import subprocess
import sys
import time

STATE = os.environ.get("SLURM_HARNESS_STATE", "/tmp/slurm-harness")

# What `env` will print back.  The filter is a prefix test rather than a list
# of names for the same reason the environment is captured rather than
# reconstructed: a variable SLURM adds in a later release should reach PRRTE
# without anyone here having to know about it.  SLURM_CONF is excluded because
# it points at a file, not at an allocation, and the containers all read the
# same baked one anyway.
ENV_PREFIXES = ("SLURM_", "SLURMD_")
ENV_EXCLUDE = ("SLURM_CONF",)


def die(msg, rc=1):
    sys.stderr.write("slurm-alloc: %s\n" % msg)
    sys.exit(rc)


def state_path(tag, suffix):
    return os.path.join(STATE, "%s.%s" % (tag, suffix))


def run(argv, **kw):
    """Run a command and return (rc, stdout+stderr)."""
    try:
        p = subprocess.run(argv, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, **kw)
    except OSError as e:
        return 127, str(e)
    return p.returncode, p.stdout.decode("utf-8", "replace")


# --------------------------------------------------------------------------
# new
# --------------------------------------------------------------------------

def cmd_new(args):
    os.makedirs(STATE, exist_ok=True)
    envf = state_path(args.tag, "env")
    jobf = state_path(args.tag, "jobid")
    logf = state_path(args.tag, "log")
    for f in (envf, jobf, logf):
        try:
            os.unlink(f)
        except OSError:
            pass

    salloc = ["salloc", "--no-bell", "--job-name=prte-%s" % args.tag]
    if args.nodes:
        salloc.append("--nodes=%d" % args.nodes)
    if args.nodelist:
        salloc.append("--nodelist=%s" % args.nodelist)
    if args.tasks_per_node:
        salloc.append("--ntasks-per-node=%d" % args.tasks_per_node)
    if args.exclusive:
        salloc.append("--exclusive")
    if args.time:
        salloc.append("--time=%d" % args.time)

    # The held command.  It writes the environment SLURM gave it, then the job
    # id (LAST, so a reader that sees the job id knows the environment file is
    # already complete), and then sleeps forever holding the allocation.
    #
    # `exec sleep infinity` rather than a shell loop: the sleep replaces the
    # shell, so the process tree under salloc is one process and a scancel
    # takes the whole thing down with nothing left behind.
    held = ("env > %s; echo $SLURM_JOB_ID > %s; exec sleep infinity"
            % (shlex.quote(envf), shlex.quote(jobf)))

    with open(logf, "wb") as log:
        subprocess.Popen(salloc + ["bash", "-c", held],
                         stdout=log, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)

    # Wait for the allocation to be granted.  A request SLURM cannot satisfy
    # sits PENDING forever rather than failing, so this has to time out and
    # say what salloc was told -- an allocation that never arrives otherwise
    # surfaces as an unrelated failure several cases later.
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if os.path.exists(jobf):
            with open(jobf) as fp:
                jid = fp.read().strip()
            if jid:
                print(jid)
                return 0
        time.sleep(0.25)

    try:
        with open(logf) as fp:
            why = " ".join(fp.read().split())
    except OSError:
        why = "(no output from salloc)"
    die("allocation not granted in %ds: %s" % (args.timeout, why))


# --------------------------------------------------------------------------
# reading an allocation back
# --------------------------------------------------------------------------

def read_env(tag):
    try:
        with open(state_path(tag, "env")) as fp:
            lines = fp.read().splitlines()
    except OSError:
        die("no allocation tagged '%s' -- run 'slurm-alloc new' first" % tag)
    out = {}
    for line in lines:
        if "=" not in line:
            continue                      # a multi-line value's continuation
        key, value = line.split("=", 1)
        out[key] = value
    return out


def cmd_env(args):
    env = read_env(args.tag)
    for key in sorted(env):
        if key in ENV_EXCLUDE or not key.startswith(ENV_PREFIXES):
            continue
        # Quoted, always: SLURM_TASKS_PER_NODE carries the compressed "2(x4)"
        # form, which is a syntax error unquoted.
        print("export %s=%s" % (key, shlex.quote(env[key])))
    return 0


def cmd_jobid(args):
    env = read_env(args.tag)
    jid = env.get("SLURM_JOB_ID") or env.get("SLURM_JOBID")
    if not jid:
        die("the captured environment for '%s' has no job id" % args.tag)
    print(jid)
    return 0


def cmd_nodes(args):
    env = read_env(args.tag)
    nodelist = env.get("SLURM_JOB_NODELIST") or env.get("SLURM_NODELIST")
    if not nodelist:
        die("the captured environment for '%s' has no node list" % args.tag)
    # SLURM hands out the compressed form ("node[1-3,7]"); expand it with
    # SLURM's own tool rather than a regex here, because getting that wrong
    # would make the harness disagree with the scheduler about what it holds.
    rc, out = run(["scontrol", "show", "hostnames", nodelist])
    if 0 != rc:
        die("scontrol show hostnames %s failed: %s" % (nodelist, out.strip()))
    print(",".join(out.split()))
    return 0


# --------------------------------------------------------------------------
# releasing, and looking at the cluster
# --------------------------------------------------------------------------

def cmd_free(args):
    if args.all:
        # Every job, not just the ones this script created.  A PRRTE *extend*
        # submits its own expander job through sbatch, and that job is what
        # holds the grown nodes -- so a teardown that spared it would leave
        # the pool short for every later case, which is the harness failure
        # that is hardest to read.  This is a dedicated single-purpose
        # cluster; there is nothing in its queue that is not the harness.
        rc, out = run(["squeue", "-h", "-o", "%i"])
        if 0 != rc:
            die("squeue failed: %s" % out.strip())
        for jid in out.split():
            run(["scancel", jid])
        # and drop every captured environment, so a later `env` cannot hand
        # back an allocation that no longer exists
        if os.path.isdir(STATE):
            for name in os.listdir(STATE):
                os.unlink(os.path.join(STATE, name))
        return 0

    try:
        env = read_env(args.tag)
    except SystemExit:
        return 0                          # nothing to free is not an error
    jid = env.get("SLURM_JOB_ID") or env.get("SLURM_JOBID")
    if jid:
        run(["scancel", jid])
    for suffix in ("env", "jobid", "log"):
        try:
            os.unlink(state_path(args.tag, suffix))
        except OSError:
            pass
    return 0


def cmd_pool(args):
    """Nodes with nothing allocated on them.

    %t is the abbreviated node state; a node is free for a new allocation when
    it is idle.  Reported one per line, in the order sinfo gives them.
    """
    rc, out = run(["sinfo", "-h", "-N", "-o", "%N %t"])
    if 0 != rc:
        die("sinfo failed: %s" % out.strip())
    for line in out.splitlines():
        parts = line.split()
        if 2 == len(parts) and parts[1] in ("idle", "idle~"):
            print(parts[0])
    return 0


def cmd_jobs(args):
    rc, out = run(["squeue", "-h", "-o", "%i %j %T %N"])
    if 0 != rc:
        die("squeue failed: %s" % out.strip())
    sys.stdout.write(out)
    return 0


def cmd_wait(args):
    deadline = time.time() + args.secs
    while time.time() < deadline:
        rc, out = run(["squeue", "-h", "-j", args.jobid, "-o", "%T"])
        if 0 == rc and out.strip() == "RUNNING":
            return 0
        time.sleep(0.5)
    die("job %s was not RUNNING within %ds" % (args.jobid, args.secs))


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(prog="slurm-alloc", add_help=True,
                                 description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("new")
    p.add_argument("--tag", default="dvm")
    p.add_argument("--nodes", type=int)
    p.add_argument("--nodelist")
    p.add_argument("--tasks-per-node", type=int)
    p.add_argument("--exclusive", action="store_true")
    p.add_argument("--time", type=int)
    p.add_argument("--timeout", type=int, default=60)
    p.set_defaults(fn=cmd_new)

    for name, fn in (("env", cmd_env), ("jobid", cmd_jobid), ("nodes", cmd_nodes)):
        p = sub.add_parser(name)
        p.add_argument("--tag", default="dvm")
        p.set_defaults(fn=fn)

    p = sub.add_parser("free")
    p.add_argument("--tag", default="dvm")
    p.add_argument("--all", action="store_true")
    p.set_defaults(fn=cmd_free)

    for name, fn in (("pool", cmd_pool), ("jobs", cmd_jobs)):
        p = sub.add_parser(name)
        p.set_defaults(fn=fn)

    p = sub.add_parser("wait")
    p.add_argument("jobid")
    p.add_argument("secs", nargs="?", type=int, default=30)
    p.set_defaults(fn=cmd_wait)

    args = ap.parse_args()
    if not getattr(args, "fn", None):
        ap.print_help()
        return 2
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
