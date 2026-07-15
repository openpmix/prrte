# AGENTS.md — `plm/ssh` (the default / reference launcher)

Component guide for `src/mca/plm/ssh/`. Read the
[framework guide](../AGENTS.md) first for the module contract, the
state-machine handlers, the daemon callback/wireup, and the base helpers
referenced throughout.

---

## Role and priority

`ssh` is the **default, always-available fallback launcher**, priority
**10** (MCA `plm_ssh_priority`, the lowest of all `plm` components). It
starts `prted` daemons by forking a remote-shell agent — `ssh` or `rsh`
by default, or `qrsh`/`llspawn`/`pbs_tmrsh` when it detects the
corresponding batch environment. It is the reference implementation: the
only component that does a **tree-based fan-out** (`remote_spawn`), and
the one to read to understand the whole framework. It targets any
environment without a native launcher (bare clusters, workstations), and
is the catch-all when no higher-priority RM component selects.

The `rsh` name is a registered alias for this component (set up in the
base's `mca_plm_base_register`), so `--prtemca plm rsh` still works.

Files:

| File | Contents |
|------|----------|
| `plm_ssh_component.c` | Registration (all MCA params), `query` (agent/RM detection, priority), `prte_plm_ssh_search` (locate an agent in PATH). |
| `plm_ssh_module.c` | The whole launcher: `ssh_init`, `ssh_launch` (spawn), `launch_daemons`, `remote_spawn` (tree fan-out), `setup_launch` (build the remote cmd), `process_launch_list` (throttled fork/exec), `ssh_wait_daemon` (SIGCHLD), shell detection. |
| `plm_ssh.h` | `prte_mca_plm_ssh_component_t` struct + `prte_plm_ssh_search` prototype. |
| `help-plm-ssh.txt` | User-facing error text (agent-not-found, deadlock-params, cmd-line-too-long, …). |

---

## When/why selected (`ssh_component_query`)

The query decides *which agent* to use and offers the module at priority
10 if it finds one:

1. If the user explicitly set `plm_ssh_agent` (source != default), skip
   RM detection and just look up that agent.
2. Otherwise probe the batch environment, in order:
   - **Grid Engine / SGE** (`SGE_ROOT` + `ARC` + `PE_HOSTFILE` +
     `JOB_ID`, unless `plm_ssh_disable_qrsh`) → agent `qrsh`, sets
     `using_qrsh`.
   - **LoadLeveler** (`LOADL_STEP_ID`, unless `disable_llspawn`) → agent
     `llspawn`, sets `using_llspawn`.
   - **PBS** (`PBS_ENVIRONMENT` + `PBS_JOBID`, unless `disable_tmrsh`) →
     agent `pbs_tmrsh` (absolute path `PRTE_PBSTRMSH_PATH`), sets
     `using_tmrsh`.
3. Fall back to looking up the `plm_ssh_agent` list (default
   `"ssh : rsh"` — a colon-delimited preference list) in PATH via
   `ssh_launch_agent_lookup` → `prte_plm_ssh_search`.

If the user named an agent that cannot be found, it is a hard error
(`agent-not-found`, activates `NEVER_LAUNCHED`); if only the default
couldn't be found, the component simply declines (returns a module of
NULL). When an `ssh` agent is chosen it auto-adds `-X` (if `--xterm`) or
`-x` (disable X11 forwarding, unless debugging).

`daemon_nodes_assigned_at_launch` is **`true`** for ssh: because we
`ssh` to a specific host, each daemon vpid's node is known at launch.

---

## Spawn → launch flow

`ssh_launch` (the `spawn` vtable entry) is trivial: it just activates
`PRTE_JOB_STATE_INIT` (or `MAP` for a restart) and returns. All the work
happens later in **`launch_daemons`**, the handler `ssh_init` registered
on `PRTE_JOB_STATE_LAUNCH_DAEMONS`:

1. `prte_plm_base_setup_virtual_machine(jdata)` — compute the daemon
   map. If `DO_NOT_LAUNCH` or `num_new_daemons == 0`, fast-forward to
   `DAEMONS_LAUNCHED`→`DAEMONS_REPORTED` and return.
2. Guard against the `--debug-daemons` **deadlock**: if sessions are
   left attached AND `num_concurrent < num_new_daemons`, the ssh tunnels
   never close and the launch would deadlock — bail with
   `deadlock-params`.
3. Pull the prefix(es) from the daemon job object (`PRTE_JOB_PREFIX` /
   `PRTE_JOB_PMIX_PREFIX`).
4. Pick a sample non-local node and call **`setup_launch`** to build the
   template remote command (see below).
5. Iterate `map->nodes`. **If tree-spawning** (`!no_tree_spawn`), only
   enqueue nodes whose daemon is one of *my* routing children
   (`prte_rml_base.children`); otherwise enqueue all. Skip nodes that
   already have a daemon (`PRTE_NODE_FLAG_DAEMON_LAUNCHED`). For each
   node: substitute the node name (with optional `user@host` and `-p
   <port>` from node attributes) and the real vpid into the template
   argv, wrap it in a `prte_plm_ssh_caddy_t`, and append to
   `launch_list`.
6. Set the job to `DAEMONS_LAUNCHED`, force `no_tree_spawn = true` (so
   any *secondary* launch is flat), and fire `launch_event` to drain the
   list.

### `setup_launch` — building the remote command

This is the intricate part. It assembles the argv passed to the agent:

`<agent> <agent-args> <host-placeholder> [shell prelude] "<remote cmd>"`

- Copies the agent argv (`ssh_agent_argv`) and any `plm_ssh_args`;
  records `node_name_index1` (the host slot, substituted per node).
- `setup_shell` determines the remote shell (via `assume_same_shell`, or
  `ssh_probe` which runs `echo $SHELL` over the agent) and, for `sh`/
  `ksh`, prepends a `.profile`-sourcing prelude.
- Builds the prefix/`LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` exports —
  **shell-specific**: `PRTE_PREFIX=...; export ...` form for
  sh/ksh/zsh/bash vs. `setenv ...` with `$?LD_LIBRARY_PATH` guards for
  [t]csh (csh evaluation order is fiddly — see the inline comment).
- Splits the daemon command via `prte_plm_base_setup_prted_cmd` (default
  `prted`), prepends the prefix `bindir` when it's the stock `prted`, and
  handles a wrapper (`valgrind ... prted`).
- Joins the whole shell script with `;`, appends `--daemonize` (non-tree,
  non-debug), then the standard daemon options via
  `prte_plm_base_prted_append_basic_args(..., "env", &proc_vpid_index)`.
- Forces `--prtemca plm ssh` on the remote daemon, and — **when
  tree-spawning** — adds `--tree-spawn --prtemca prte_parent_uri
  <my-uri>` so each child learns its parent's contact info.
- `prte_plm_base_wrap_args` quotes multi-word mca args; if the resulting
  command exceeds `_SC_ARG_MAX`, error with `cmd-line-too-long`.

### `process_launch_list` — throttled fork/exec

Drains `launch_list` up to `num_concurrent` (default **128**) at a time.
For each caddy it registers a `prte_wait_cb` (SIGCHLD → `ssh_wait_daemon`)
on the daemon proc, then `fork()`s. The child calls **`ssh_child`**:
redirect stdin from `/dev/null`, close inherited fds, reset signal
handlers to default and unblock signals, then `execve` the agent. Both
sides `setpgid` the child into its own process group so a `Ctrl-C` to
the HNP doesn't `SIGINT` the ssh processes (which would litter the
session dir and lose orted diagnostics). The parent records the ssh
fork's pid and marks the daemon `RUNNING`.

Launch metering is via `launch_event` + `num_in_progress`:
`ssh_wait_daemon` decrements `num_in_progress` as each ssh exits and
re-fires the event to admit more — this is the concurrency throttle.

---

## The tree-spawn / qtree fan-out (the reference feature)

By default (`no_tree_spawn == false`) ssh does **not** have the HNP `ssh`
to every node. Instead it launches only its **direct routing children**
(`prte_rml_base.children`, from the RML routing tree computed in
`setup_vm`). Each launched daemon is told `--tree-spawn` and its parent's
URI. On the daemon side, once it wires up, it calls the module's
**`remote_spawn`** vtable entry (the *only* component that implements
it), which:

1. Returns immediately if the daemon has no children
   (`prte_rml_base.n_children == 0`) — it's a leaf.
2. Calls the same `setup_launch` to build a template command (finding the
   prefix from `PRTE_PREFIX`/`PMIX_PREFIX` in its environment, or the
   built-in default).
3. For each of *its* routing children (`prte_rml_base.children`),
   resolves the child's hostname, substitutes host + vpid, and enqueues a
   caddy — then fires the same `launch_event`/`process_launch_list`
   machinery to `ssh` to its children.

The result is a logarithmic-depth **fan-out tree**: the HNP spawns its
children, they spawn theirs, and so on, so launch time scales with tree
depth rather than node count — essential at extreme scale. Every daemon
still reports directly back to the HNP via
`PRTE_RML_TAG_PRTED_CALLBACK`; only the *spawning* is tree-structured.

**Secondary launches never tree-spawn.** After the initial launch (and in
`remote_spawn`), `no_tree_spawn` is forced `true`, so a dynamic
`add-host` launch is a flat, direct ssh from the HNP.

---

## Failure reporting (`ssh_wait_daemon`)

Registered as the SIGCHLD callback on each ssh fork. If the fork exits
abnormally (non-zero / non-`WIFEXITED`) and we're not already
terminating:

- **On a daemon** (tree-spawn child): pack the failed daemon's rank +
  exit code and send to the HNP on `PRTE_RML_TAG_REPORT_REMOTE_LAUNCH`
  (caught by `prte_plm_base_daemon_failed`); mark the daemon
  `FAILED_TO_START`.
- **On the HNP**: print the long "daemon failed to report back"
  diagnostic (unable to find prted, missing libraries, firewall blocking
  the TCP callback, …), update exit status, mark the daemon
  `FAILED_TO_START`, drop it from routing, and activate the
  proc-failure state so the DVM exits.

This message is the #1 symptom of a launch stuck at `DAEMONS_LAUNCHED`.

---

## Key structs and MCA params

`prte_mca_plm_ssh_component_t` (in `plm_ssh.h`) — the notable fields and
their MCA vars (`plm_ssh_*`):

| Field / param | Meaning |
|---------------|---------|
| `num_concurrent` (128) | Max simultaneous ssh forks. |
| `priority` (10) | Selection priority. |
| `no_tree_spawn` (false) | Disable the fan-out; flat launch from HNP. |
| `agent` (`"ssh : rsh"`) | Preference-ordered agent list. |
| `agent_argv` / `agent_path` | Resolved agent + its args. |
| `ssh_args` | Extra args appended to the agent. |
| `assume_same_shell` (true) | Skip the remote `echo $SHELL` probe. |
| `pass_libpath` | Prepend a path to the remote `LD_LIBRARY_PATH`. |
| `chdir` | `cd` on the remote node before exec'ing prted. |
| `force_ssh`, `disable_qrsh`/`disable_llspawn`/`disable_tmrsh`, `daemonize_qrsh`/`daemonize_llspawn` | RM-detection and daemonize toggles. |
| `using_qrsh`/`using_llspawn`/`using_tmrsh` | Set by `query` when the batch env is detected. |

`prte_plm_ssh_caddy_t` — the per-daemon launch item on `launch_list`:
carries `argc`/`argv` (the fully-substituted command) and the `daemon`
proc (retained; released when it starts).

---

## Things to watch when editing

- **`num_concurrent` vs. `--debug-daemons` is a real deadlock**, guarded
  explicitly in `launch_daemons`. Any change to session-attach behavior
  must keep that check.
- **Tree-spawn only launches routing children.** The `children` loop in
  `launch_daemons` and `remote_spawn` depends on the RML routing tree
  being current — `setup_vm` recomputes it, so don't reorder that.
- **Force `no_tree_spawn = true` after the primary launch.** Secondary/
  dynamic launches must be flat; the two force-assignments in
  `launch_daemons` and `remote_spawn` are load-bearing.
- **Process-group isolation (`setpgid`) is not optional** — without it a
  shell `Ctrl-C` kills the ssh processes mid-launch and litters session
  dirs.
- **Shell-specific env syntax.** The bash/sh/ksh/zsh vs. [t]csh split in
  `setup_launch` is subtle (csh evaluation order); test both when
  touching prefix/library-path emission.
- **Command-length limit.** Long environments blow past `_SC_ARG_MAX`;
  the fix the help text suggests is `plm_ssh_pass_environ_mca_params 0`.
