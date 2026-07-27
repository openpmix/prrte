#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
"""fake-slurm -- a stand-in SLURM control plane for the dockerswarm harness.

``ras/slurm`` is the only ras component with a full elastic *modify* surface,
and every bit of it talks to the scheduler by shelling out:

    sbatch  --wrap=... --parsable --exclusive --nodes=N [propagated attrs]
    scontrol show job <id> --json
    scontrol update job <id> ReqNodeList=<survivors>
    scancel <id>

None of that can run on a developer's laptop, so the whole extend/release/
cancel path -- including the ~1000-line ``scontrol --json`` parser, the
``validate_hostname`` allowlist and ``prte_ras_slurm_drain_cmd_output`` -- had
no automated coverage anywhere.  This script supplies the missing scheduler:
it is installed into the swarm as ``sbatch``/``scontrol``/``scancel`` (it
dispatches on argv[0]), keeps its state in a directory, and emits JSON in the
shape ``ras_slurm_jansson.c`` expects.

It is deliberately *not* a SLURM emulator.  It implements exactly the four
command forms PRRTE issues, and it hands out real container hostnames, so a
grow driven through it launches real daemons on real nodes.

Housekeeping subcommands (run the script by its own name):

    fake-slurm init --jobid 1000 --base node1 --pool node2,node3,...
    fake-slurm env                     # export lines for the SLURM_* envars
    fake-slurm set <key> <value>       # pending_secs|scancel_fail|bad_json|...
    fake-slurm jobs                    # job ids, one per line
    fake-slurm nodes <jobid>           # that job's nodes, comma separated
    fake-slurm args <jobid>            # the sbatch argv that created it
    fake-slurm pool                    # unallocated nodes
    fake-slurm audit                   # every command this stub was given

State lives in $FAKE_SLURM_STATE (default /tmp/fake-slurm).
"""

import json
import os
import shutil
import sys
import time

STATE = os.environ.get("FAKE_SLURM_STATE", "/tmp/fake-slurm")
JOBDIR = os.path.join(STATE, "jobs")
CONFIG = os.path.join(STATE, "config.json")
POOL = os.path.join(STATE, "pool")
NEXTID = os.path.join(STATE, "nextid")
AUDIT = os.path.join(STATE, "audit.log")

DEFAULT_CONFIG = {
    "jobid": "1000",
    "base_nodes": [],
    "tasks_per_node": 2,
    # Per-node shape reported for an allocated node.  Two ALLOCATED cores and
    # one UNALLOCATED one, against a much larger cpus.count, so the slot count
    # PRRTE derives (allocated_cores * threads_per_core, capped by cpus.count)
    # is 2 -- a number that can only come from counting core *statuses*.
    "cpus_per_node": 8,
    "allocated_cores": 2,
    "idle_cores": 1,
    # attributes of the "parent" job, i.e. the ones the extend path propagates
    "account": "prrte-test",
    "partition": "debug",
    "qos": "normal",
    "cwd": "/root",
    "memory_per_cpu": None,          # unset: the sbatch arg must be omitted
    "memory_per_node": 1024,
    "time_limit": 60,
    "threads_per_core": 1,
    # fault injection
    "pending_secs": 0,               # how long a new job sits in PENDING
    "scancel_fail": 0,               # scancel exits non-zero, noisily
    "bad_json": 0,                   # scontrol show job emits unparsable JSON
}


# --------------------------------------------------------------------------
# state helpers
# --------------------------------------------------------------------------

def die(msg, rc=1):
    """Report the way the real tools do: a line of text, a non-zero exit."""
    sys.stdout.write(msg + "\n")
    sys.exit(rc)


def load_config():
    try:
        with open(CONFIG) as fp:
            cfg = json.load(fp)
    except (OSError, ValueError):
        die("fake-slurm: no state in %s -- run 'fake-slurm init' first" % STATE)
    merged = dict(DEFAULT_CONFIG)
    merged.update(cfg)
    return merged


def save_config(cfg):
    with open(CONFIG, "w") as fp:
        json.dump(cfg, fp, indent=1)


def job_path(jid):
    return os.path.join(JOBDIR, "%s.json" % jid)


def load_job(jid):
    try:
        with open(job_path(jid)) as fp:
            return json.load(fp)
    except (OSError, ValueError):
        return None


def save_job(job):
    with open(job_path(job["job_id"]), "w") as fp:
        json.dump(job, fp, indent=1)


def all_jobs():
    try:
        names = sorted(os.listdir(JOBDIR))
    except OSError:
        return []
    return [n[:-5] for n in names if n.endswith(".json")]


def read_pool():
    try:
        with open(POOL) as fp:
            return [ln.strip() for ln in fp if ln.strip()]
    except OSError:
        return []


def write_pool(nodes):
    with open(POOL, "w") as fp:
        fp.write("".join(n + "\n" for n in nodes))


def next_jobid():
    try:
        with open(NEXTID) as fp:
            nid = int(fp.read().strip())
    except (OSError, ValueError):
        nid = 2001
    with open(NEXTID, "w") as fp:
        fp.write("%d\n" % (nid + 1))
    return str(nid)


def audit(argv):
    try:
        with open(AUDIT, "a") as fp:
            fp.write("%d %s\n" % (time.time(), " ".join(argv)))
    except OSError:
        pass


# --------------------------------------------------------------------------
# JSON rendering -- the shape ras_slurm_jansson.c parses
# --------------------------------------------------------------------------

def numobj(value):
    """A SLURM {set, infinite, number} triple.  None means "not set"."""
    if value is None:
        return {"set": False, "infinite": False, "number": 0}
    if value == "infinite":
        return {"set": True, "infinite": True, "number": 0}
    return {"set": True, "infinite": False, "number": int(value)}


def node_object(cfg, index, name):
    cores = []
    for k in range(int(cfg["allocated_cores"])):
        cores.append({"index": k, "status": ["ALLOCATED"]})
    for k in range(int(cfg["idle_cores"])):
        cores.append({"index": int(cfg["allocated_cores"]) + k,
                      "status": ["UNALLOCATED"]})
    return {
        "index": index,
        "name": name,
        "cpus": {"count": int(cfg["cpus_per_node"]), "used": 0},
        "memory": {"allocated": 0, "used": 0},
        "sockets": [{"index": 0, "cores": cores}],
    }


def render_job(cfg, job):
    """Build the document `scontrol show job <id> --json` prints."""
    if job.get("cancelled"):
        states = ["CANCELLED"]
    elif time.time() >= job.get("ready_at", 0):
        states = ["RUNNING"]
    else:
        states = ["PENDING"]

    alloc = [node_object(cfg, i, n) for i, n in enumerate(job["nodes"])]

    jrec = {
        "job_id": int(job["job_id"]),
        "name": job.get("name", "prte-expander"),
        "job_state": states,
        "account": job.get("account"),
        "partition": job.get("partition"),
        "qos": job.get("qos"),
        "current_working_directory": job.get("cwd"),
        "memory_per_cpu": numobj(job.get("memory_per_cpu")),
        "memory_per_node": numobj(job.get("memory_per_node")),
        "time_limit": numobj(job.get("time_limit")),
        "threads_per_core": numobj(job.get("threads_per_core")),
        "job_resources": {
            "nodes": {
                "count": len(alloc),
                "list": ",".join(job["nodes"]),
                "allocation": alloc,
            },
        },
    }
    # real scontrol wraps the array in meta/errors/warnings; carrying them
    # keeps the parser honest about ignoring what it was not asked for
    return {
        "meta": {"plugin": {"type": "openapi/slurmctld", "name": "fake-slurm"}},
        "errors": [],
        "warnings": [],
        "jobs": [jrec],
    }


# --------------------------------------------------------------------------
# sbatch
# --------------------------------------------------------------------------

def cmd_sbatch(args):
    cfg = load_config()
    audit(["sbatch"] + args)

    nodes_req = None
    job = {
        "account": None, "partition": None, "qos": None, "cwd": None,
        "memory_per_cpu": None, "memory_per_node": None,
        "time_limit": None, "threads_per_core": 1,
    }
    for arg in args:
        if arg.startswith("--nodes="):
            try:
                nodes_req = int(arg.split("=", 1)[1])
            except ValueError:
                die("sbatch: error: invalid node count '%s'" % arg)
        elif arg.startswith("--account="):
            job["account"] = arg.split("=", 1)[1]
        elif arg.startswith("--partition="):
            job["partition"] = arg.split("=", 1)[1]
        elif arg.startswith("--qos="):
            job["qos"] = arg.split("=", 1)[1]
        elif arg.startswith("--chdir="):
            job["cwd"] = arg.split("=", 1)[1]
        elif arg.startswith("--mem-per-cpu="):
            job["memory_per_cpu"] = arg.split("=", 1)[1]
        elif arg.startswith("--mem="):
            job["memory_per_node"] = arg.split("=", 1)[1]
        elif arg.startswith("--time="):
            job["time_limit"] = arg.split("=", 1)[1]
        elif arg.startswith("--threads-per-core="):
            job["threads_per_core"] = arg.split("=", 1)[1]

    if nodes_req is None:
        die("sbatch: error: --nodes was not requested")
    if nodes_req < 1:
        die("sbatch: error: refusing to allocate %d nodes" % nodes_req)

    pool = read_pool()
    if len(pool) < nodes_req:
        die("sbatch: error: Node count specification invalid -- "
            "only %d node(s) available" % len(pool))

    job["job_id"] = next_jobid()
    job["nodes"] = pool[:nodes_req]
    job["ready_at"] = time.time() + float(cfg["pending_secs"])
    job["cancelled"] = False
    write_pool(pool[nodes_req:])
    save_job(job)

    with open(os.path.join(JOBDIR, "%s.args" % job["job_id"]), "w") as fp:
        fp.write("".join(a + "\n" for a in args))

    # --parsable output is "<jobid>[;<cluster>]"; PRRTE keeps the leading
    # digits and stops at the separator, so print the cluster suffix too
    sys.stdout.write("%s;swarm\n" % job["job_id"])
    return 0


# --------------------------------------------------------------------------
# scontrol
# --------------------------------------------------------------------------

def scontrol_show_job(cfg, jid):
    job = load_job(jid)
    if job is None:
        sys.stderr.write("slurm_load_jobs error: Invalid job id specified\n")
        return 1
    if int(cfg["bad_json"]):
        # a truncated response: the command succeeds, the payload does not parse
        sys.stdout.write('{"jobs": [ {"job_id": %s, ' % jid)
        return 0
    json.dump(render_job(cfg, job), sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


def scontrol_update_job(cfg, jid, settings):
    job = load_job(jid)
    if job is None:
        sys.stdout.write("slurm_update error: Invalid job id specified\n")
        return 1

    survivors = None
    for setting in settings:
        if setting.startswith("ReqNodeList="):
            value = setting.split("=", 1)[1]
            survivors = [n for n in value.split(",") if n]
    if survivors is None:
        sys.stdout.write("slurm_update error: nothing to update\n")
        return 1

    unknown = [n for n in survivors if n not in job["nodes"]]
    if unknown:
        sys.stdout.write("slurm_update error: node(s) %s are not in job %s\n"
                         % (",".join(unknown), jid))
        return 1
    if len(survivors) >= len(job["nodes"]):
        sys.stdout.write("slurm_update error: job %s cannot grow this way\n" % jid)
        return 1

    released = [n for n in job["nodes"] if n not in survivors]
    job["nodes"] = survivors
    save_job(job)
    write_pool(read_pool() + released)

    # A real shrink leaves these behind in the caller's cwd for the user to
    # run; PRRTE deletes them, so create them to give that cleanup something
    # to do.
    for suffix in ("sh", "csh"):
        try:
            with open("slurm_job_%s_resize.%s" % (jid, suffix), "w") as fp:
                fp.write("# fake-slurm resize helper for job %s\n" % jid)
        except OSError:
            pass
    return 0


def cmd_scontrol(args):
    cfg = load_config()
    audit(["scontrol"] + args)

    if len(args) >= 3 and args[0] == "show" and args[1] == "job":
        return scontrol_show_job(cfg, args[2])
    if len(args) >= 3 and args[0] == "update" and args[1] == "job":
        return scontrol_update_job(cfg, args[2], args[3:])
    sys.stdout.write("scontrol: error: unsupported command '%s'\n" % " ".join(args))
    return 1


# --------------------------------------------------------------------------
# scancel
# --------------------------------------------------------------------------

def cmd_scancel(args):
    cfg = load_config()
    audit(["scancel"] + args)

    if not args:
        die("scancel: error: No job identification provided")
    jid = args[0]

    if int(cfg["scancel_fail"]):
        # Deliberately long and multi-line: PRRTE captures this through
        # prte_ras_slurm_drain_cmd_output() into a 256-byte buffer, so the
        # message it reports must come back truncated and "..."-terminated.
        for i in range(12):
            sys.stdout.write("scancel: error: Kill job error on job id %s: "
                             "Transport endpoint is not connected (line %d)\n"
                             % (jid, i))
        return 1

    job = load_job(jid)
    if job is None:
        sys.stdout.write("scancel: error: Invalid job id %s\n" % jid)
        return 1

    write_pool(read_pool() + job["nodes"])
    os.unlink(job_path(jid))
    return 0


# --------------------------------------------------------------------------
# housekeeping subcommands
# --------------------------------------------------------------------------

def cmd_init(args):
    cfg = dict(DEFAULT_CONFIG)
    pool = []
    i = 0
    while i < len(args):
        if args[i] == "--jobid":
            cfg["jobid"] = args[i + 1]
        elif args[i] == "--base":
            cfg["base_nodes"] = [n for n in args[i + 1].split(",") if n]
        elif args[i] == "--pool":
            pool = [n for n in args[i + 1].split(",") if n]
        elif args[i] == "--tasks":
            cfg["tasks_per_node"] = int(args[i + 1])
        else:
            die("fake-slurm init: unknown option '%s'" % args[i])
        i += 2

    if not cfg["base_nodes"]:
        die("fake-slurm init: --base is required")

    shutil.rmtree(STATE, ignore_errors=True)
    os.makedirs(JOBDIR)
    write_pool(pool)
    save_config(cfg)

    # the job the DVM itself is running in: extend reads its attributes and
    # propagates them onto the sbatch command line
    save_job({
        "job_id": cfg["jobid"],
        "name": "prte-dvm",
        "nodes": list(cfg["base_nodes"]),
        "ready_at": 0,
        "cancelled": False,
        "account": cfg["account"],
        "partition": cfg["partition"],
        "qos": cfg["qos"],
        "cwd": cfg["cwd"],
        "memory_per_cpu": cfg["memory_per_cpu"],
        "memory_per_node": cfg["memory_per_node"],
        "time_limit": cfg["time_limit"],
        "threads_per_core": cfg["threads_per_core"],
    })
    return 0


def cmd_env(args):
    """Shell to eval.  Every value is quoted: SLURM_TASKS_PER_NODE carries
    the compressed "2(x4)" form, which is a syntax error unquoted."""
    cfg = load_config()
    nodes = cfg["base_nodes"]
    def line(key, value):
        print("export %s='%s'" % (key, value))
    line("SLURM_JOBID", cfg["jobid"])
    line("SLURM_JOB_ID", cfg["jobid"])
    line("SLURM_NODELIST", ",".join(nodes))
    line("SLURM_TASKS_PER_NODE",
         "%d(x%d)" % (int(cfg["tasks_per_node"]), len(nodes)))
    line("SLURM_JOB_CPUS_PER_NODE",
         "%d(x%d)" % (int(cfg["cpus_per_node"]), len(nodes)))
    line("SLURMD_NODENAME", args[0] if args else nodes[0])
    return 0


def cmd_set(args):
    if len(args) < 2:
        die("fake-slurm set: need <key> <value>")
    cfg = load_config()
    if args[0] not in DEFAULT_CONFIG:
        die("fake-slurm set: unknown key '%s'" % args[0])
    value = args[1]
    if isinstance(DEFAULT_CONFIG[args[0]], int) and value.lstrip("-").isdigit():
        value = int(value)
    cfg[args[0]] = value
    save_config(cfg)
    return 0


def cmd_jobs(args):
    for jid in all_jobs():
        print(jid)
    return 0


def cmd_nodes(args):
    if not args:
        die("fake-slurm nodes: need a job id")
    job = load_job(args[0])
    if job is None:
        die("fake-slurm nodes: no such job %s" % args[0])
    print(",".join(job["nodes"]))
    return 0


def cmd_args(args):
    if not args:
        die("fake-slurm args: need a job id")
    try:
        with open(os.path.join(JOBDIR, "%s.args" % args[0])) as fp:
            sys.stdout.write(fp.read())
    except OSError:
        die("fake-slurm args: no sbatch record for job %s" % args[0])
    return 0


def cmd_pool(args):
    for node in read_pool():
        print(node)
    return 0


def cmd_audit(args):
    try:
        with open(AUDIT) as fp:
            sys.stdout.write(fp.read())
    except OSError:
        pass
    return 0


HOUSEKEEPING = {
    "init": cmd_init, "env": cmd_env, "set": cmd_set, "jobs": cmd_jobs,
    "nodes": cmd_nodes, "args": cmd_args, "pool": cmd_pool, "audit": cmd_audit,
}


def main():
    name = os.path.basename(sys.argv[0])
    if name == "sbatch":
        return cmd_sbatch(sys.argv[1:])
    if name == "scontrol":
        return cmd_scontrol(sys.argv[1:])
    if name == "scancel":
        return cmd_scancel(sys.argv[1:])

    if len(sys.argv) < 2 or sys.argv[1] not in HOUSEKEEPING:
        sys.stdout.write(__doc__)
        return 2
    return HOUSEKEEPING[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
