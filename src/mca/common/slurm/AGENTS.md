# AGENTS.md — `common/slurm` (what Slurm is, asked once)

Component guide for `src/mca/common/slurm/`. Read the
[framework guide](../AGENTS.md) first for what a `common` library is and
how it is built.

---

## Why this exists

Three components care whether Slurm is present and what version it is —
[`ras/slurm`](../../ras/slurm/AGENTS.md),
[`plm/slurm`](../../plm/slurm/AGENTS.md) and
[`ess/slurm`](../../ess/slurm/AGENTS.md) — and none of them owns the answer.
Before this library, each asked for itself:

- `plm/slurm` ran `popen("srun --version")` inside its `query` and cached
  major/minor plus two booleans on its own component struct;
- `ras/slurm` had no version check at all and simply failed later, inside
  its JSON parser, with a message that named neither Slurm nor a version;
- all three read `getenv("SLURM_JOBID")` independently.

That is one subprocess per asking component on every tool startup, three
copies of the version arithmetic to keep in step, and — the reason this
was actually written — three places that had to learn about the
`SLURM_JOBID` → `SLURM_JOB_ID` rename separately.

So the question is asked **once**, here, on first use, and the answer is
cached for the life of the process.

## What it answers

```c
const prte_common_slurm_version_t *prte_common_slurm_version(void);
const char                        *prte_common_slurm_jobid(void);
```

`prte_common_slurm_jobid()` returns the allocation's job id, reading
`SLURM_JOB_ID` (current) and then `SLURM_JOBID` (historical). Every Slurm
component's availability test goes through it. **This is the one that will
bite if it is bypassed**: a component that reads only the old spelling
under a Slurm that has stopped setting it does not fail — it reports "not
my environment", which is a legitimate answer everywhere it is asked. The
DVM then falls back to the ssh launcher and a one-node allocation and
looks like it is working.

`prte_common_slurm_version()` never returns NULL. It probes `srun`, then
`scontrol`/`sbatch`/`sinfo`, and yields:

| Field | Meaning |
|-------|---------|
| `available` | a Slurm command answered with a parseable version. When false, everything below is meaningless |
| `version`, `major`, `minor` | as Slurm reported it; `version` is kept verbatim for diagnostics |
| `ancient` | older than **17.11** — `plm/slurm` refuses to launch |
| `early` | older than **23.11** — `srun` has no `--external-launcher` |
| `extended` | **24.05** or newer — `scontrol show job --json` emits the `job_resources` shape `ras/slurm`'s parser reads |

**The three thresholds are three different questions.** Do not fold them
into one "minimum supported version": a component asking the wrong one
would either refuse a Slurm it can drive perfectly well or drive one it
cannot.

## Things to know before editing

- **The first word of the version line is not always `slurm`.** Debian and
  Ubuntu build Slurm under their own package name, so their clients answer
  `slurm-wlm 23.11.4`, and the version they report carries a packaging
  suffix (`21.08.8-2`). The parser therefore matches the package name as a
  *prefix* and takes the version from after the first space; it must not
  anchor on `"slurm "` or index a fixed number of characters into the line.
  This has broken twice. It is worth knowing what the breakage looks like,
  because it does not look like a parse error: the probe just reports
  `available == false`, which is indistinguishable from a machine with no
  Slurm at all, so `plm/slurm` declines to select and the DVM falls back to
  `ssh` and a one-node allocation *inside a live Slurm allocation* — and
  says nothing, because "not my environment" is a legitimate answer
  everywhere it is asked. The same rule applies to the shell-side probe in
  [`ras/slurm/configure.m4`](../../ras/slurm/configure.m4), which mis-reads
  the same line into the build's idea of the version.
- **`!available` is not "too old".** A Slurm client establishes a
  configuration source *before* it will print its version, so on a machine
  with the binaries but no reachable `slurm.conf` the command exits 1 having
  printed `Could not establish a configuration source` and no version at
  all. That is the ordinary state of a build node, and callers must treat
  it as "cannot tell", never as a refusal — `ras/slurm` deliberately serves
  the request in that case rather than turning a transient into a policy.
- **The probe runs a subprocess**, so call it from the progress thread
  only. Every current caller does: they are inside component selection or
  an allocation request.
- **The cache is a plain file-static.** There is no lock, and none is
  wanted; adding a caller from another thread means adding one.
- **This library is built into `libprrte`** in the default static build, so
  the three components resolve it there and share the single cache. The
  `Makefile.am` also handles the DSO shape (one shared library, still one
  copy) and sym links the noinst library to the installable name so the
  components' `LIBADD` need no conditional. If you add a fourth consumer,
  add the same `LIBADD` line — see any of the three.
- **A new common library needs a `VERSION` entry.**
  `libmca_prte_common_slurm_so_version` lives at the bottom of
  [`VERSION`](../../../../VERSION) and is `AC_SUBST`ed in `configure.ac`;
  without it `-version-info` gets an empty argument in a DSO build.

## Testing

[`test/unit/common/test_common_slurm.c`](../../../../test/unit/common/test_common_slurm.c)
covers the parser and the three thresholds without a scheduler: it writes
stub `srun`/`scontrol`/`sbatch`/`sinfo` scripts that print a chosen line,
puts them first on `PATH`, and runs each case in a forked child — the cache
is per process by design, so one process can hold exactly one answer. That
is also the only way to reach the parser at all, since it is static and its
input comes from a subprocess.

Everything above the parser is exercised by
[`contrib/slurmswarm`](../../../../contrib/slurmswarm/), which runs PRRTE
against a real Slurm whose version is pinned by the image and can be
changed with a build argument. That harness is also where a wrong answer
shows up as something other than a puzzle: `plm/slurm` failing to select
means the suite's `plm` phase reports the ssh launcher, and `extended`
being wrong means the elastic phase refuses with a message naming the
version it found.
