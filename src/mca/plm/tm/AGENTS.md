# AGENTS.md — `plm/tm` (daemons as PBS tasks, through a TM helper)

Component guide for `src/mca/plm/tm/`. Read the
[framework guide](../AGENTS.md) first.

---

## Why it exists, and why it does not call TM itself

Inside a PBS job only the TM API starts a process as a **task of the job**,
and only a task is accounted for: PBS charges the job the CPU time of its
tasks and of everything they start, and kills them when the job ends or is
deleted. A daemon started with `ssh` runs under `sshd` instead - on a site
that bills by `cput`, a multi-node job launched that way is charged for its
first node.

PRRTE must not load the TM library: some builds of it (Altair's PBS Pro)
carry private copies of libevent, which conflict with the one PMIx uses in
the same process. So every TM call is made by **`prte_tm_spawn`**, a small
program in this directory, linked against the TM library and libc only, and
installed in `$(libexecdir)/prte` - not `bindir`, since nobody should run it
by hand. The component links nothing of TM's; configure's flags are the
helper's (`plm_tm_helper_*`), and the helper gets a run path to the TM
library (`plm_tm_helper_RPATH`, libtool `-R`) because OpenPBS keeps it in
`/opt/pbs/lib`, where the run-time linker does not look.

Nothing else may reach the helper's link. Configure's `LIBS` carries PMIx,
libevent and hwloc, and automake appends it to every link in a Makefile,
so this directory's `Makefile.am` empties it (the component gets what it
needs from `libprrte.la`). And `PRTE_CHECK_TM` tries a bare `-lpbs` before
`-lpbs -lcrypto -lz`: a shared libpbs brings its own, and PBS Pro's
`libpbs` links its system's `libcrypto`, so naming `-lcrypto` would add
whichever one the build's search path finds first (a Spack OpenSSL 3, at
one site) beside it. A linker that drops unused libraries by default
(Ubuntu's) hides both mistakes; RHEL's does not. What libtool adds by
itself is libpbs's own: given `-lpbs`, it reads `libpbs.la` where there is
one and links the `dependency_libs` it names.

`pbs_tmrsh`, TM as a command, cannot stand in for it: the mom runs its
arguments with no shell, gives the task `/dev/null` for stdin and the job's
environment rather than the caller's, so a daemon started through it has no
private way to receive its DVM key. Nothing in PRRTE uses it.

## One helper per launch

`launch_daemons()` builds the daemon's argv (the base's, ess `env`, never
`--daemonize`), the environment, and forks **one** helper for all of that
launch's daemons (`start_helper()`); a later launch (an elastic grow) gets
another. Each run is a `tm_launch_t` on the `launches` list until it is
reaped.

| Channel | Carries |
|---------|---------|
| argv | `prte_tm_spawn <vpid-index> <daemon argv...>` - nothing private. |
| environment | What each daemon gets: our `prte_launch_environ` less every `PMIX_*`/`PRTE_*` variable (they are on the command line, as plm/slurm strips them for srun), plus the prefix's `PATH`/`LD_LIBRARY_PATH`, `PRTE_PREFIX`/`PMIX_PREFIX` and the umask. It also carries the job's `PBS_*` variables, which `tm_init()` reads. |
| stdin | `PRTE_DVM_KEY=<hex>`, a blank line, then `<vpid> <host>` per daemon. The helper adds the key to the environment it hands `tm_spawn()` - **only** there, not to its own environment - and the prted reads it with `prte_dvm_key_from_env()`, as under srun. |
| stdout | One line per daemon event: `<vpid> exited <status>`, `<vpid> spawn-failed <tm-error>`, `<vpid> no-such-host 0`. |
| stderr | Ours: the helper's diagnostics go straight to the user. |

The mom executes `argv[0]` with `execvp()` under the environment it was
handed, so `prted` is found along our `PATH` (with `--prefix`, the prefix's
`bin` first) - the same assumption `plm/ssh` makes of a remote shell.

The key travels over PBS's own channels - to the local mom, and from the
mother superior's mom to each sister's - as srun's environment travels over
Slurm's.

### Which TM node a host is

`tm_nodeinfo()` returns the job's nodes as ids, with no names. Node `i` is
line `i` of `$PBS_NODEFILE` - the mapping `pbs_tmrsh` itself relies on - so
the helper takes the first line naming the host; failing that, the first
line whose first label is the host, for a name the HNP shortened from the
file's FQDN. The HNP sends `node->rawname` when it has one, which is the
file's own spelling.

### Writing the input without SIGPIPE

`prte` aborts on SIGPIPE. The input can exceed a pipe's buffer (a line per
daemon), so it is written non-blocking from a write event, and the HNP
**keeps its own copy of the pipe's read end** (`in_rfd`) until the write is
done: a write therefore never meets a closed pipe however soon the helper
dies, and the helper's reaping (`helper_exited()`) is what ends a write it
will never read. The buffer holds the key, and is wiped when it is let go.

## Failure reporting

TM knows nothing of a failed `exec`: a daemon that cannot start is a task
that ends (OpenPBS reports 254). So the helper watches every task it
started with `tm_obit()` and lives as long as they do. In
`helper_line()`, every event becomes `daemon_failed()`, which settles each
daemon once and does nothing for one that has reported (`rml_uri` set) - a
reported daemon that dies is a lost connection, which errmgr hears of
already - or while the DVM is coming down. Otherwise it shows
`daemon-failed` with the reason and activates the proc `FAILED_TO_START`,
as `ssh_wait_daemon` does, so a bad launch fails at once instead of waiting
for daemons that will never call back.

The mom also writes a line of its own for a task it could not exec, to
the job's stderr: `<argv[0]>: No such file or directory`. OpenPBS's
`start_process()` prints `argv[0]` after it has freed the task's argv, so
the name comes out garbled, and PBS Pro prints it the same way. It is not
the helper's or the prted's.

When the helper itself ends, `helper_exited()` reads what it left in the
pipe, then fails every daemon it said nothing about. At finalize a helper
still running is sent SIGTERM; its tasks are PBS's and do not depend on it
(the `cluster` phase of `contrib/pbsswarm` checks that a TM task outlives
its spawner and is still charged to the job).

## Selection

`query` offers the module (priority 75) when `PBS_ENVIRONMENT` and
`PBS_JOBID` are set and the helper is executable at
`<libexecdir>/prte/prte_tm_spawn`. Without the helper it shows `no-helper`
and declines, and `ssh` launches - daemons PBS does not account for. The
location follows `prte_install_dirs`, so `PRTE_LIBEXECDIR` (or
`PRTE_PREFIX`) moves it; the suite uses that to test the fallback.

The prted never opens `plm` unless `PRTE_MCA_plm` is in its environment,
and that variable is among the `PRTE_*` the environment loses - so a daemon
never selects this component.

`daemon_nodes_assigned_at_launch` is true: the helper starts each vpid on
the node named for it.

## Configure

`--with-pbs` builds PBS support (`ras/pbs`), and so does `--with-tm`,
which implies it: this component is built only together with `ras/pbs`,
since it can start daemons only on the nodes of the PBS job and only
`ras/pbs` reads those. `--with-tm` with `--without-pbs` is an error. The
decision is `PRTE_SETUP_PBS` (`config/prte_check_pbs.m4`), made once
however many components ask through `PRTE_CHECK_PBS`, which
`AC_REQUIRE`s it - so `plm` and `ras` may be configured in either order.

The component needs TM too, found by `PRTE_CHECK_TM`
(`config/prte_check_tm.m4`), which runs only when PBS support is on:
`--with-tm=DIR`,
else the directory given to `--with-pbs`, else `pbs-config` and the default
paths. Not finding it is a configure **error** in two cases: an explicit
`--with-tm`, and `--with-pbs` with nothing said about TM - that user asked
for a PRRTE that works in a PBS job, and without this component it would
launch with ssh, which PBS does not account for, and nothing would say so.
`--without-tm` is how to build PBS support that way on purpose.

`--enable-testbuild-launchers` builds the component without TM: it links
nothing of TM's anyway, and the helper is compiled against
`testbuild_tm.h` (the declarations, with OpenPBS's types) and linked with
`testbuild_tm.c`, whose every call fails. A program must link, so these are
definitions, unlike the declaration-only stubs the other launchers use.
Keep their prototypes faithful to `tm.h`.

## Things to watch

- **Never pass `--daemonize`.** A daemon that forks out of its task is one
  PBS does not account for - the reason this component exists.
- **Nothing of TM's in PRRTE's processes.** If a TM call seems needed in
  the HNP, it goes in the helper and its result comes back on the pipe.
- **Nothing of PRRTE's in the helper.** It includes `prte_config.h` for
  its defines only, and links the TM library and libc. Do not call a
  PRRTE or PMIx function from it, and do not take `LIBS =` out of the
  `Makefile.am`.
- **The key goes to `tm_spawn()`'s environment only** - not on the
  helper's command line, not in its own environment (both are readable on
  the node), and it is wiped from the helper once the spawns are sent.
- **The helper's line protocol and `helper_line()` change together.** They
  are one build, like everything in a DVM.
- **No tree spawn.** `remote_spawn` is NULL; the mother superior's mom fans
  the spawns out.

## Testing

[`contrib/pbsswarm`](../../../../contrib/pbsswarm/) runs a real OpenPBS:
its `plm` phase checks that the helper needs nothing but libpbs, libc and
libpbs's own dependencies (the harness links with `--no-as-needed`, as RHEL does, so that check can
fail), that every remote daemon is a task under its node's
`pbs_mom`, that the key is on no daemon's command line and gone from its
environment and was never in the helper's, that a daemon that cannot start
fails the launch at once, and that without the helper `ssh` takes over;
`accounting` that PBS charges the job for every node; `qdel` that deleting
the job takes the daemons down. What is specific to Altair's PBS Pro - the
library conflict this design avoids - still needs a PBS Pro site.
