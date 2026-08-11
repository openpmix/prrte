#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A recording, optionally misbehaving, wrapper around the real salloc,
# scontrol and scancel.  Dispatches on argv[0], the way the tools it wraps are
# reached: build.sh installs it as
#
#     /opt/prte/slurmshim/bin/{salloc,scontrol,scancel}
#
# deliberately NOT into the install bin/ that every node has on its PATH -- a
# case has to opt in by putting that directory first, so nothing else in the
# suite can be perturbed by it.
#
# WHY THIS EXISTS, GIVEN THAT THIS HARNESS HAS A REAL SCHEDULER.
#
# Two kinds of assertion cannot be made against slurmctld alone, and both used
# to live in contrib/dockerswarm against a whole fake scheduler:
#
#   * WHAT PRRTE ASKED FOR.  The scheduler records the job it created, not the
#     command line that created it.  Several behaviors are only visible in the
#     argv -- an attribute that must be OMITTED when its source is unset, a
#     propagate_* MCA parameter that must drop exactly its own argument, the
#     absence of --wrap.  A job record cannot show the absence of an argument.
#   * WHAT PRRTE DOES WHEN THE SCHEDULER MISBEHAVES.  A live slurmctld will
#     not emit unparsable JSON or fail a scancel on demand, and those are
#     precisely the paths that exist to handle it doing so.
#
# So this wraps rather than replaces: every command is passed through to the
# real one unless a fault is explicitly armed.  What the scheduler does stays
# real; only the record, and the two armed faults, are ours.
#
#   slurm-shim reset                 # clear the state (argv records, faults)
#   slurm-shim argv                  # the argv of the most recent salloc
#   slurm-shim audit                 # every wrapped command, in order
#   slurm-shim set <key> <value>     # arm/disarm a fault
#
# Faults:
#   bad_json 1     `scontrol show job ... --json` prints garbage, exits 0
#   scancel_fail 1 `scancel` fails with far more output than PRRTE's capture
#                  buffer holds, so the truncation path is taken

import os
import sys

STATE = os.environ.get("SLURM_SHIM_STATE", "/tmp/slurm-shim")
WRAPPED = ("salloc", "scontrol", "scancel")


def state_path(*parts):
    return os.path.join(STATE, *parts)


def ensure_state():
    os.makedirs(STATE, exist_ok=True)


def flag(key):
    """A fault's current setting, '0' when it has never been set."""
    try:
        with open(state_path("flag." + key)) as f:
            return f.read().strip()
    except OSError:
        return "0"


def record(name, argv):
    """Append to the audit log, and for salloc keep the argv on its own.

    The argv record is per-invocation and 'argv' reports the most recent one.
    Keying it by job id is not possible from here: this process EXECs the real
    salloc (see below), so it never sees the job id salloc goes on to print --
    and every case that asserts on an argv issues exactly one extend, so the
    most recent record is the one it means.
    """
    ensure_state()
    with open(state_path("audit.log"), "a") as f:
        f.write(" ".join(argv) + "\n")
    if "salloc" == name:
        with open(state_path("argv.last"), "w") as f:
            f.write("\n".join(argv) + "\n")


def real_command(name):
    """The wrapped command itself, found by skipping our own directory.

    Searching PATH normally would find this script again: the whole point is
    that our directory is in front.
    """
    ours = os.path.dirname(os.path.abspath(sys.argv[0]))
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if not d or os.path.abspath(d) == ours:
            continue
        cand = os.path.join(d, name)
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def passthrough(name, argv):
    """Become the real command.

    exec rather than fork: PRRTE forks salloc itself, reads the job id off its
    output, and reaps it later by pid.  A wrapper that stayed in the middle
    would have to reproduce all of that faithfully; becoming the command means
    there is nothing to reproduce.
    """
    real = real_command(name)
    if real is None:
        sys.stderr.write("slurm-shim: no real %s on PATH\n" % name)
        sys.exit(127)
    os.execv(real, [real] + argv)


def main():
    name = os.path.basename(sys.argv[0])
    argv = sys.argv[1:]

    if name not in WRAPPED:
        # invoked as "slurm-shim <subcommand>" -- the control interface
        return control(argv)

    record(name, [name] + argv)

    if "scontrol" == name and "--json" in argv and "1" == flag("bad_json"):
        # Exit 0 with unparsable output on purpose: a non-zero status would be
        # caught by the caller's status check and never reach the parser,
        # which is the code under test.
        sys.stdout.write("{ this is not json, and never was\n")
        return 0

    if "scancel" == name and "1" == flag("scancel_fail"):
        # Far more than the 256 bytes PRRTE captures, so the '...' truncation
        # path is taken.  Numbered lines so a test can tell how much survived.
        for i in range(1, 21):
            sys.stdout.write("scancel: error: Kill job error on job id %s "
                             "(line %d)\n" % (argv[-1] if argv else "?", i))
        return 1

    passthrough(name, argv)
    return 0                                    # not reached


def control(argv):
    if not argv:
        sys.stderr.write(__doc__ or "usage: slurm-shim <reset|argv|audit|set>\n")
        return 2
    cmd = argv[0]
    if "reset" == cmd:
        ensure_state()
        for f in os.listdir(STATE):
            os.remove(state_path(f))
        return 0
    if "argv" == cmd:
        try:
            with open(state_path("argv.last")) as f:
                sys.stdout.write(f.read())
        except OSError:
            return 1
        return 0
    if "audit" == cmd:
        try:
            with open(state_path("audit.log")) as f:
                sys.stdout.write(f.read())
        except OSError:
            return 1
        return 0
    if "set" == cmd and 3 == len(argv):
        ensure_state()
        with open(state_path("flag." + argv[1]), "w") as f:
            f.write(argv[2])
        return 0
    sys.stderr.write("slurm-shim: unknown command %s\n" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main())
