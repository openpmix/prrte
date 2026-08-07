# AGENTS.md — `src/util`

Orientation for AI agents and human contributors working in `src/util/`.
This is a map, not the rulebook: the authoritative project guidance lives in
the top-level [`AGENTS.md`](../../AGENTS.md) and under [`docs/`](../../docs/).
When this file and those disagree, **the docs win** — and please fix this
file.

---

## What lives here

`src/util` is PRRTE's own utility layer: the helpers that are specific to
PRRTE's data model and therefore cannot live in PMIx. It is **not** a general
toolbox. PRRTE gets its argv handling, string helpers, path manipulation,
lists, hash tables, output streams, `show_help`, environment handling and
command-line parsing from **PMIx** (`src/util/pmix_*.h` in the PMIx install).
If what you need is generic, it belongs in PMIx — see
[Generic CLI code lives in PMIx](../../AGENTS.md) and do not add a local
copy here.

Everything in this directory is compiled into `libprrteutil.la`, which is
linked into `libprrte`. There are no MCA components here.

| Area | Files | What it is |
|------|-------|------------|
| **Attributes** | `attr.[ch]` | The typed key/value store hung off `prte_job_t`, `prte_app_context_t`, `prte_node_t` and `prte_proc_t`. Get/set/append/prepend/fetch/remove, the load/unload marshalling, the key→name renderer, and the flag pretty-printers. |
| **Names** | `name_fns.[ch]` | Rendering and parsing of `pmix_proc_t`/`pmix_nspace_t`, and `prte_util_compare_name_fields()`. |
| **Node specifications** | [`hostfile/`](hostfile/AGENTS.md), [`dash_host/`](dash_host/AGENTS.md) | The two ways a user names machines. Each has its own AGENTS.md. |
| **Nidmap** | `nidmap.[ch]` | The compressed node-name/daemon-vpid map the HNP ships to every daemon. |
| **Errors and states** | `error.[ch]`, `error_strings.[ch]` | `prte_strerror()`, `PRTE_ERROR_LOG()`, and the four state→name renderers. |
| **Process info** | `proc_info.[ch]` | The `prte_process_info` global: hostname and its aliases, uid/gid, session-dir paths, proc type. |
| **Session directories** | `session_dir.[ch]` | Construction and teardown of the `$TMPDIR/<prefix>.<pid>/<jobid>/<rank>` tree. |
| **Tool option values** | `prte_cmd_line.[ch]` | Value interpreters more than one tool needs (`--pid`, `--app`, the daemon umask). See [`src/tools/AGENTS.md`](../tools/AGENTS.md). |
| **Bootstrap** | `prte_bootstrap.[ch]` | Reading `prte.conf` for a launcher-less DVM. |
| **Process plumbing** | `daemon_init.c`, `sys_limits.[ch]`, `stacktrace.[ch]`, `ethtool.[ch]` | Daemonizing, `setrlimit`, the crash handler, and the Linux interface-speed ioctl. |
| **Help delivery** | `prte_show_help.[ch]` | `prte_show_help()` — a drop-in for `pmix_show_help()` that works on a **daemon**. See below. |
| **Generated** | `prte_show_help_content.c`, `prte-convert-help.py` | Every `help-*.txt` in the tree, compiled in. **Never edit the generated file.** |

---

## GOLDEN RULE: this directory is on everyone's include path — assume nothing

A function here is called from tools that never ran `prte_init()`, from
parsers that run before the globals exist, and from the PMIx progress thread
by way of `PRTE_NAME_PRINT` in a verbose message. So:

- `prte_job_data`, `prte_node_pool`, `prte_sessions` and
  `prte_node_topologies` are **NULL** until `prte_init()`. Anything reachable
  from a tool or a parser must tolerate that. (`prte_check_host_is_local()`
  is called by both hostfile and dash-host parsing, which is why it checks
  `prte_process_info.nodename` for NULL.)
- Do not modify an argument, even temporarily. Several functions here take a
  `const pmix_nspace_t` and used to punch a `'\0'` into it to split it,
  restoring it afterwards. That argument is normally a live `jdata->nspace`:
  the truncation is visible to any other thread that reads the name in that
  window, and it is undefined behavior outright for a string literal. Copy
  out the part you want (`"%.*s"` works well) instead.
- `PRTE_NAME_PRINT` and friends hand back a pointer into a **per-thread ring
  of 16 buffers**. That is what makes `pmix_output(0, "%s -> %s", NAME(a),
  NAME(b))` correct. It also means the pointer is only good until the same
  thread has printed 16 more names — never store one.

---

## Attributes

`prte_attribute_t` is a `pmix_value_t` plus a key and a `local` flag, on a
`pmix_list_t`. The API is in [`attr.h`](attr.h); the type marshalling is
`prte_attr_load()`/`prte_attr_unload()`.

Things that are easy to get wrong:

- **`local` is really "do not pack me".** `PRTE_ATTR_GLOBAL` means the
  attribute travels with the job when it is packed. A spawn request is
  *always* packed, so the mapper sees an unpacked **copy** of the job — any
  attribute the mapper must read has to be `PRTE_ATTR_GLOBAL`. This has bitten
  the mapper before.
- **A `PMIX_BOOL` attribute means "true" by its presence.** `prte_set_attribute`
  with `NULL` data records true; setting it to `false` *removes the entry*.
  Test with `prte_get_attribute(list, key, NULL, PMIX_BOOL)`.
- **`prte_get_attribute` refuses a type mismatch** rather than reinterpreting
  the bytes, and logs it. Pass the type the setter used.
- **Unload allocates for the pointer types** (`PMIX_STRING`, `PMIX_BYTE_OBJECT`,
  `PMIX_PROC`, `PMIX_PROC_NSPACE`, `PMIX_ENVAR`, `PMIX_DATA_ARRAY`) and
  `memcpy`s into caller storage for the scalars — so for a scalar you must pass
  the address of a pointer that already points at your variable:
  `int val; int *p = &val; prte_get_attribute(l, k, (void**)&p, PMIX_INT);`
- **A reload must fully release what it replaces.** Every branch of
  `prte_attr_load` that frees a pointer has to NULL it before conditionally
  reassigning, or a value the new data does not supply leaves a pointer to
  freed storage for the destructor to free again.
- **Adding a key means adding its name** to `prte_attr_key_to_str()`. A key
  with no entry renders as `UNKNOWN-KEY: <n>`, which is what
  `--display-map devel` and every attribute diagnostic will print.
  `test/unit/util` requires the names to be **unique**, which is how a
  copy/pasted name gets caught.

Attribute keys live in [`attr.h`](attr.h), numbered from
`PRTE_ATTR_KEY_BASE` within per-object bands (`PRTE_APP_*`, `PRTE_NODE_*`,
`PRTE_JOB_*`, `PRTE_PROC_*`, `PRTE_RML_*`). Append at the end of a band; do
not renumber, and do not reuse a skipped value.

---

## Names, and how they compare

A PRRTE namespace is `"<dvm-identifier>@<local-jobid>"`, so **every job in a
DVM has a namespace of the same length as its siblings' and differs only in
the trailing number.** Any comparison of namespaces therefore has to look at
their contents. `prte_util_compare_name_fields()` returns
`PRTE_EQUAL` / `PRTE_VALUE1_GREATER` / `PRTE_VALUE2_GREATER`, and callers
depend on both halves of that contract:

- `errmgr/prted` and `iof/prted` ask only "equal?" — a false equal routes one
  job's stdin, or one job's failure, to another job's process.
- `rml/oob/tcp` needs the **ordering** to break a simultaneous-connect tie. If
  both ends compute `PRTE_EQUAL` they both step aside and the connection is
  never made.

`PRTE_NS_CMP_WILD` makes an empty namespace or `PMIX_RANK_WILDCARD` match
anything; without it a wildcard is compared as the literal value it is.

---

## Errors and states

Two rules, both enforced by `test/unit/util`:

1. **Every error code needs a `prte_strerror()` entry.** `PRTE_ERROR_LOG()`
   prints nothing else, so a missing entry surfaces to the user as
   `PRTE ERROR: Unknown error in file ... at line ...`. The test sweeps the
   whole numeric range of [`constants.h`](../include/constants.h) rather
   than a hand-kept list, skipping only the reserved hole between the two
   documentation groups.
2. **Every job/proc/node/app state needs a name**, and no two states may
   share a name *or* a value. The state families in
   [`plm_types.h`](../mca/plm/plm_types.h) have intentional holes, so the
   test carries an explicit table — extend it when you add a state.

`PRTE_ERR_SILENT` is the "already reported, say nothing more" code;
`PRTE_ERROR_LOG()` suppresses it. `PRTE_ERR_TAKE_NEXT_OPTION` is a
control-flow signal, not a failure.

---

## Session directories

One tree per process, named for the **tool's own** session prefix and pid:
`$TMPDIR/<sessdir_prefix>.<pid>/` for the top level, then `<local-jobid>/`,
then `<rank>/`. Each tool has its own prefix (`prte`, `prterun`, `prted`), and
each holds a PMIx server rendezvous file — which is why a stale directory from
a dead DVM makes the *next* `prun` report "multiple possible servers ...
connection handles have been read from files named `pmix.*`" instead of
finding the live DVM. Anything that leaves a session directory behind is a
bug, and `contrib/dockerswarm/run-tests.sh` clears all three prefixes between
cases for exactly this reason.

`prte_job_session_dir_finalize()` keeps non-empty `output-*` files (that is
what `--output file=...` wrote) and removes everything else. The
`prte_process_info.rm_session_dirs` flag means the resource manager will clean
up and PRRTE must not.

---

## The nidmap

`prte_util_nidmap_create()` packs, and `prte_util_decode_nidmap()` unpacks,
four lockstep lists — node name, aliases, daemon vpid, and the node's slot in
the sender's `prte_node_pool` — plus the *span* of the daemon vpid space. It
is sent to every daemon whenever the DVM's membership changes, so a daemon
decodes it **many times**, and each decode has to leave the daemon's view
equal to the master's:

- **The pool slot travels on the wire because it is the node's identity.**
  It is the `PMIX_NODEID` a daemon hands its local clients, and the subscript
  the `PMIX_SERVER_URI` query resolves a nodeid through. It is *not* the
  position in the packed list: the sender skips nodes that have no daemon, so
  after a shrink the packed sequence is compacted while the pool is not, and
  a receiver that used the packed position renamed one node's entry into
  another's and gave two machines one nodeid. It is not the daemon's vpid
  either — a shrink retires a vpid permanently but leaves the node's slot.
- **Every decode rebinds, it does not just fill gaps.** A node's daemon
  changes (grow, shrink, re-grow), so `nd->daemon` and
  `daemons->procs[vpid]` are (re)established on every pass, releasing the
  reference the node held. Binding only newly-created entries made every
  decode after the first a no-op, which is how a daemon that predated a grow
  never learned the new daemon existed — and `odls` resolves the parent of
  *every* proc in a job, not just its own, so that daemon launched nothing
  and the master, which had no such gap, waited forever (#2616).
- **A node the sender did not name has lost its daemon** and its backpointer
  is cleared, mirroring what the master did to its own pool on the shrink.

`prte_util_pack_job_catchup()` / `prte_util_decode_job_catchup()` ride in the
same message, immediately after the nidmap. They carry **the jobs already
running in the DVM** — a daemon that has just joined never saw their launch
messages and so cannot resolve their namespaces. They live beside the nidmap
because they answer the same question, *what does the DVM currently consist
of*, and because that message is sent on exactly the event that changes the
answer. Three things about them:

- **The job being launched is excluded.** A daemon that already holds a
  namespace *drops* the copy in the launch message, so a catch-up entry for
  a job that has not been mapped yet would leave every daemon holding a
  procless version of it for good.
- **Only the rank travels, not the parent vpid.** `prte_job_pack` already
  carries each proc's parent daemon, which is all the receiver needs to put
  the proc back on its node; the launch message used to pack that vpid a
  second time alongside.
- **The decode registers each new namespace with the local PMIx server and
  does not wait.** Nothing later in the message depends on it, and the
  launch message that might care cannot have been built yet — the master
  sends this at `VM_READY` and the launch message several states later.

This replaced a block at the head of the launch message, which tied that
message's size to the number of jobs resident in the DVM and still left a
daemon added by a bare elastic grow knowing nothing, since a grow launches no
job and therefore sends no launch message.

---

## `prte_process_info`

A single global, filled in by `prte_setup_hostname()` and `prte_proc_info()`.
The part that matters to everything else is the **hostname and its aliases**:
`nodename` is the canonical short name, and `aliases` carries the FQDN, any
prefix-stripped form, `localhost` and `127.0.0.1`. `prte_check_host_is_local()`
is the only correct way to ask "is this name me?", and both node parsers call
it on every name they read so that a node the user named by an alias resolves
to the same `prte_node_t` the daemon reports itself as.

`prte_keep_fqdn_hostnames` and `prte_strip_prefix` are MCA parameters read
here, and the stripping has to happen *here* so the names exchanged through
the modex match the names found locally.

---

## Testing

- **`test/unit/util/test_util.c`** (`make check`) covers attributes, names,
  error/state strings, the dash-host parser, the hostfile parser and the
  system-limit parser. Almost everything in this directory is a pure function
  of its arguments, so if you add one here, it should be testable there — and
  if it is not, say why in the file's header comment.
- **`contrib/dockerswarm/run-tests.sh`** (`test_util` phase) covers what a
  unit test cannot: the *multi-node* meaning of the two parsers. With one node
  in the pool a slot-count or node-selection defect looks correct, which is
  precisely how several of them survived. Anything that changes which
  machines a specification names belongs there too.
- The **offline mapper harness** (`make -C test/offline check-offline`) drives
  `--map-by`/`--rank-by`/`--bind-to` over synthetic topologies. It consumes
  what `dash_host`/`hostfile` produce, so run it after touching either.

Not unit-testable, and deliberately left to the live smoke test: `session_dir`
(creates directories under the real `$TMPDIR`), `stacktrace` (installs signal
handlers), `daemon_init` (forks), and `nidmap` (needs a populated DVM).

`nidmap` in particular needs a DVM that has **changed size**, and a job that
**spans a daemon which predates the change** — a one-proc job lands on the
master, whose copy of the map is authoritative and never decoded. That is the
elastic grow/shrink/grow case in `contrib/dockerswarm/run-tests.sh`.

---

## `prte_show_help()` — because `pmix_show_help()` does not work on a daemon

`pmix_show_help()` renders correctly everywhere and **delivers nowhere on
a `prted`.** It hands the rendered text to PMIx's `plog` framework, and
`plog/stdfd` writes its own `stderr` only when the caller is a PMIx
*client or tool*. A daemon is a PMIx *server*, so it takes the other
branch, which passes the text to `PMIx_server_IOF_deliver()` tagged with
the daemon's own identity — and nothing has an IOF sink for a daemon's
own output. The message is built and thrown away. The head node looks
fine only because there the daemon's PMIx server is the one holding the
tool connection (and under `prterun` it *is* the tool).

`prte_show_help()` has the same signature and the same rendering, and:

- on the **HNP**, on a **tool**, or in an **application**, delivers
  locally, exactly as `pmix_show_help()` would;
- on any **other daemon**, renders locally and ships the text to the HNP
  over `PRTE_RML_TAG_SHOW_HELP`, where `prte_show_help_recv()` delivers
  it. Aggregation and duplicate suppression then happen **once**, on the
  HNP, keyed by the same filename/topic — which is what you want anyway
  when 500 nodes hit the same error.
- falls back to local delivery if there is no HNP to send to yet (early
  startup, or teardown), so a message is never simply lost.

**The whole tree is converted** — every one of the ~420 call sites that
used to say `pmix_show_help(` now says `prte_show_help(`, so a plain
`grep` for the PMIx spelling in a `.c` file should come back empty. A new
`pmix_show_help()` is therefore a message that will be invisible off the
head node, and nothing will tell you: no warning, no error, just silence
on 999 nodes out of 1000. Use `prte_show_help()` everywhere. It is
identical outside a daemon, so there is no case where the PMIx spelling
is the better choice.

(`pmix_show_help_string()` and `pmix_show_help_norender()` are different
functions with different signatures and are untouched; `prte_show_help()`
is built on top of them.)

`prte-convert-help.py` recognizes both spellings when it scans for help
citations, so converting a call site does not remove it from the
help-file cross-check.

---

## `show_help` content is generated

`prte_show_help_content.c` is built by `prte-convert-help.py` from **every**
`help-*.txt` in the tree — including `help-prte-util.txt` here and the ones
in `dash_host/` and `hostfile/`. The Make rule depends only on the converter
script, so an ordinary `make` will **not** pick up a change to help text:

```sh
rm src/util/prte_show_help_content.*
make
```

This is a top-level golden rule; it is repeated here because three of the
tree's `help-*.txt` files live under this directory.

---

## Do not grow this directory

Two things belong somewhere else:

- **Generic utilities belong in PMIx.** A local copy is two maintenance paths.
  If PRRTE needs a helper PMIx does not have yet, add it to PMIx and require
  the PMIx that has it (`PRTE_CHECK_PMIX_CAP`), rather than shimming it here.
- **Dead code is a liability.** This directory accumulated an unused CRC
  library, an unused URI encoder (with a heap overflow in it), an unused
  number-to-string pair, an unused bit-ops header, an unused malloc-debug
  wrapper, and a `parse_options.c` that duplicated PMIx's and was not even in
  `Makefile.am`. If nothing calls it, delete it.
