# AGENTS.md — The `schizo` Framework (Personalities)

Orientation for AI agents and human contributors working in
`src/mca/schizo/`. This is a map, not the rulebook: the authoritative
project guidance lives in the top-level [`AGENTS.md`](../../../AGENTS.md)
and under [`docs/`](../../../docs/). When this file and those disagree,
**the docs win** — and please fix this file.

---

## What this framework does

`schizo` is the **personality** framework. A personality is the set of
command-line and environment conventions a particular launcher exposes to
users. PRRTE's own tools (`prte`, `prun`, `prterun`, `prted`, `pterm`,
`prte_info`) speak the native **prte** personality; when PRRTE is asked
to stand in for Open MPI's `mpirun`/`mpiexec`, it speaks the **ompi**
personality instead. The job of a schizo component is to **parse a
tool's `argv` and environment in that launcher's dialect and translate
the result into PRRTE's internal model** — MCA params pushed into the
environment, `pmix_cli_result_t` option instances, and per-job/per-app
attributes.

schizo runs **very early**, before almost anything else, and in two
distinct places:

- **In the user-facing tools** (`src/tools/*`, and the app-parse path in
  `src/prted/`). Right after the framework opens, the tool calls
  `prte_schizo_base_select()`, then `prte_schizo_base_detect_proxy()` to
  choose a personality, then drives that module's `parse_cli`,
  `check_sanity`, `parse_env`, and `job_info` hooks to turn the command
  line into a spawn request.
- **In the daemons / HNP**, where a much thinner slice is used:
  `setup_fork` injects personality-specific environment into each local
  process just before `fork`/`exec` (called from `odls/base`, right after
  that framework's own `process_envars()` has applied the generic envar
  directives), and the `set_default_mapping`/`ranking`/`binding`/`rto`
  hooks let a personality override PRRTE's default placement policy from
  inside the rmaps and state machinery.

Unlike a launch or mapping decision, schizo produces no runtime state of
its own — it is a **translation layer** sitting at the boundary between
"what the user typed" and "what PRRTE's data structures expect."

---

## Directory layout

```
schizo/
  schizo.h                    # module/component vtable (the personality contract)
  base/
    base.h                    # framework-global struct + all base API prototypes
    schizo_base_frame.c       # open/close/register; MCA params; sanity checker; output/display parsers
    schizo_base_select.c      # multi-select: keeps ALL components, priority-sorted
    schizo_base_stubs.c       # detect_proxy stub, argv normalize, --mca/--prtemca/--pmixmca parsers, setup_fork
    help-schizo-base.txt      # base error/help text (deprecations, missing values, conflicts)
    help-schizo-display.txt   # --display directive help
    help-schizo-output.txt    # --output directive help
    help-schizo-rtos.txt      # --runtime-options directive help
  prte/                       # NATIVE personality (component pri 5): prte/prun/prterun/prted/pterm/prte_info
  ompi/                       # Open MPI personality (component pri 50): mpirun/mpiexec emulation
```

Read `schizo.h` first — it defines the module vtable every personality
fills in. Then read `schizo_base_stubs.c` (the proxy-detection stub and
the shared MCA-argv parsers) and `schizo_base_frame.c` (the sanity
checker and the option→attribute converters).

---

## The module contract

Every personality fills in one `prte_schizo_base_module_t`
(`schizo.h`). The struct is "module version 1.3.0"; every field is a
function pointer except `name`, and a personality may leave any hook
`NULL` — callers **always** null-check before invoking. There is no
`define_cli`/`wrap_args`/`setup_child` hook in this struct; the CLI
tables are static arrays *inside* each component's `parse_cli`, and
deprecated-option rewriting is a static helper (`convert_deprecated_cli`)
each component calls from its own `parse_cli`.

| Field | Signature (args) | Meaning / return |
|-------|------------------|------------------|
| `name` | `char *` | Personality name (`"prte"`, `"ompi"`); also the fallback `--personality` value. |
| `init` | `(void)` | One-time module setup. Both current components leave it `NULL`. |
| `parse_cli` | `(char **argv, pmix_cli_result_t *results, bool silent)` | Parse this tool's command line into `results`. Selects a per-tool option table, runs `pmix_cmd_line_parse`, converts deprecated options. Returns `PRTE_SUCCESS`, `PRTE_OPERATION_SUCCEEDED` (stock output like `--version` already emitted), `PRTE_ERR_SILENT`, or an error. |
| `parse_env` | `(char **srcenv, char ***dstenv, pmix_cli_result_t *cli)` | Extract personality envars that must be forwarded into the app environment (`dstenv`). prte's is a near no-op; ompi's is the heavy OMPI-MCA translator. |
| `detect_proxy` | `(char *cmdpath)` | Return a 0–100 confidence that *this* personality owns the invocation. 100 = definitive. Highest confidence wins (see below). |
| `allow_run_as_root` | `(pmix_cli_result_t *results)` | Decide whether running as root is permitted; on refusal calls `prte_schizo_base_root_error_msg()` and exits. Personality-specific because the override envars differ (`PRTE_*` vs `OMPI_*`). |
| `set_default_mapping` | `(prte_job_t *, prte_rmaps_options_t *)` | Override rmaps' default `--map-by`. Both current components leave it `NULL` (base default is used). |
| `set_default_ranking` | `(prte_job_t *, prte_rmaps_options_t *)` | Override default `--rank-by`. Only **ompi** sets it (dense-pack for PPR). |
| `set_default_binding` | `(prte_job_t *, prte_rmaps_options_t *)` | Override default `--bind-to`. Both `NULL` today. |
| `set_default_rto` | `(prte_job_t *, prte_rmaps_options_t *)` | Set default runtime options for a job. Both delegate to `prte_state_base_set_runtime_options`. |
| `setup_app` | `(prte_pmix_app_t *app)` | Rewrite an app before launch (e.g. relative→absolute path, prepend `java`). Both currently `NULL`. |
| `setup_fork` | `(prte_job_t *, prte_app_context_t *)` | Inject personality-specific environment into `app->env` just before fork. Both use `prte_schizo_base_setup_fork` (shared base impl), which sets `PRTE_LAUNCHED` and applies the PMIx prefix. The generic envar directives are **not** applied here — `odls`' `process_envars()` owns those (see below). |
| `job_info` | `(pmix_cli_result_t *, void *jobinfo)` | Add personality-specific job info to a spawn request. Both currently no-ops. |
| `check_sanity` | `(pmix_cli_result_t *cmd_line)` | Validate directives/qualifiers and flag conflicts. Both use `prte_schizo_base_sanity`. |
| `finalize` | `(void)` | Cleanup. Both `NULL`. |

The MCA version macro is `PRTE_MCA_SCHIZO_BASE_VERSION_1_0_0`. The
`prte_schizo_base_component_t` is a bare `pmix_mca_base_component_t`;
each component wraps it in its own struct (`..._prte_component_t`,
`..._ompi_component_t`) that adds `priority`, `warn_deprecations`, and a
one-shot `warned` flag.

Which hooks are actually invoked, and from where:

| Hook | Called from |
|------|-------------|
| `detect_proxy` | tools + `ess` (`prte_schizo_base_detect_proxy` stub) |
| `parse_cli` | every tool's `main`, `prted/prte_app_parse.c` |
| `check_sanity` | `prted/prte_app_parse.c` |
| `parse_env` | `prted/prte_app_parse.c` |
| `job_info` | `prted/prun_common.c` |
| `setup_app` | `prted/prte_app_parse.c` (null-checked) |
| `allow_run_as_root` | `prun`, `prted` |
| `setup_fork` | `odls/base/odls_base_default_fns.c` |
| `set_default_mapping`/`ranking`/`binding` | `rmaps/base/rmaps_base_map_job.c` (null-checked) |
| `set_default_rto` | `prted/pmix/pmix_server_dyn.c` |

---

## Selection is "keep all", not "pick one"

Like `rmaps`, schizo is a **multi-select** framework.
`prte_schizo_base_select()` (`schizo_base_select.c`) queries every
component, keeps *every* one that returns a module, and stores them
**priority-sorted (highest first)** in
`prte_schizo_base.active_modules`. Static component priorities today:

```
ompi 50  >  prte 5
```

But the static priority is **not** how a personality is chosen. That
happens per-invocation in `prte_schizo_base_detect_proxy()`
(`schizo_base_stubs.c`), which walks `active_modules`, calls each
module's `detect_proxy(cmdpath)`, and returns the module with the
highest returned confidence. Each component's `detect_proxy` decides its
own confidence from three signals, in override order:

1. **Explicit `--personality` list** (passed in as `cmdpath`): if it
   names the component, that component bids (prte bids its static
   priority `5`; ompi bids `translate_params()` = `100`); otherwise it
   bids `0`.
2. **Environment** (`PRTE_MCA_schizo_proxy` or `PRTE_MCA_personality`):
   an exact match bids `100`, a mismatch bids `0`.
3. **Default**: prte falls back to its static priority (`5`) — it is the
   catch-all; ompi bids `0` (it never claims an invocation it wasn't
   explicitly asked for).

**A bid of `0` means "not me", so the bar is a strictly positive bid,
not merely the highest one** — and when nothing clears that bar,
`detect_proxy` falls back to the **default** personality (whatever a
query with no hint at all returns), not to whichever component happens
to sort first. Seeded at `-1`, an all-zero field was won by the
highest-static-priority component, so a personality nobody claims read
the command line in the **ompi** dialect: a different option table and
a different MCA translation than the user asked for. Landing on the
catch-all instead is the fix; the falling back itself was never the
problem.

That distinction is load-bearing, not cosmetic. **Open MPI starts a
singleton's DVM with `--prtemca schizo prte`** — so only the native
component is even loaded — **and then spawns with
`PMIX_PERSONALITY="ompi5"`.** Nothing claims that name in that DVM.
Refusing it fails every `MPI_Comm_spawn` from a singleton, which is
exactly what a first attempt at this did. Do not "tighten" the fallback
into an error.

`detect_proxy` still returns `NULL` when there is no default either
(no modules at all), so callers that resolve a personality out of a
**spawn request** rather than a command line (`pmix_server_dyn.c`,
`pmix_server_session.c`) null-check and reject the request: every later
use of `jdata->schizo` is an unchecked dereference, and on a persistent
DVM that is the whole DVM, not one job.

So absent any personality hint, **prte always wins** as the default
personality, and ompi only takes over when the user (or the
`mpirun`/`mpiexec` symlink's environment) explicitly asks for it. The
`--personality` value itself is discovered up front by
`prte_schizo_base_normalize_argv()`, which the tool passes to
`detect_proxy`. Selection is not derived from `argv[0]` directly inside
schizo; the tool basename is resolved elsewhere (`prte_tool_basename` /
`prte_tool_actual`) and read by `parse_cli` to pick the per-tool option
table.

---

## What `base/` provides in detail

### `schizo_base_frame.c` — open/close/register, sanity, output/display

- **Framework globals & registration.** Defines `prte_schizo_base` and
  registers the framework-level MCA params: `prte_personality` (default
  personality, with a deprecated synonym `schizo_proxy`),
  `prte_display`, `prte_output`, `prte_rtos` (deprecated synonym
  `runtime_options`), and `schizo_base_test_proxy_launch`. The big help
  strings in the `--display`/`--output`/`--rtos` registrations are the
  canonical directive lists.
- **`prte_schizo_base_sanity(cmd_line)`** — the shared `check_sanity`
  implementation, called from the app-parse path. It is described in its
  own comment as a **developer** check (it emits show_help but is really
  about catching bad translations from user CLI to PRRTE internals). It:
  rejects duplicate single-value options (`--map-by`, `--rank-by`,
  `--bind-to`, `--display`, `--runtime-options`); expands synonyms
  (`machinefile`→`hostfile`, `wd`→`wdir`); validates `--map-by`/
  `--rank-by`/`--bind-to`/`--output`/`--display`/`--runtime-options`
  directives and qualifiers against fixed `mappers[]`/`rankers[]`/
  `binders[]`/… tables via `prte_schizo_base_check_directives`; enforces
  per-option value-count limits (`check_ndirs`); and flags the map-by
  PE / bind-to conflict.

  `check_ndirs` is worth understanding before touching it.
  `pmix_cmd_line_parse` appends **every occurrence** of an option to the
  *same* `pmix_cli_item_t`'s value array, and every consumer of the keys
  in `limits[]` (`--path`, `--wdir`, `--pset`, `--np`, `--keepalive`)
  reads `values[0]`. So a repeated option is not "last wins" or "first
  wins" by design — the later values are simply discarded. `check_ndirs`
  is what refuses it, and the check is `1 < count` (a count of *zero* is
  not "too many"; it means a flag-style option with no value at all).
  This is safe for MPMD because `prte_parse_locals()` splits the command
  line at each `:` and calls `parse_cli`/`check_sanity` on one app
  segment at a time, so a per-app `--np` is a single value in its own
  result set.
- **`prte_schizo_base_check_directives` / `_check_qualifiers`** — the
  reusable directive validators, including the special-cased
  `--map-by ppr:N:resource` pattern check.
- **`prte_schizo_base_parse_display` / `_parse_output`** — convert a
  parsed `--display` / `--output` option into `PMIX_INFO` list entries
  (`PMIX_DISPLAY_MAP`, `PMIX_IOF_TAG_OUTPUT`, `PMIX_IOF_OUTPUT_TO_FILE`,
  …) on the job-info object. These are where the human-facing directive
  strings become PMIx keys.

  Two shape rules govern both, and both have been got wrong before —
  each time silently, because an unrecognized directive here is simply
  not emitted and the job runs without it:

  1. **Iterate over every value.** `opt->values` is an array (each
     repetition of `--output`/`--display` appends one), so the loop body
     must split `values[n]`, not `values[0]`.
  2. **Directives are `,`-delimited; qualifiers are `:`-delimited.**
     After the `,` split, the text following the first `:` in a token is
     a `:`-joined run of qualifiers and must be split on `:` again.
     Splitting it on `,` yields one token that still contains the
     colons, and `PMIX_CHECK_CLI_OPTION` prefix-matches it against the
     first qualifier only — so `file=X:raw:nocopy` kept `raw` and lost
     `nocopy`, while `file=X:nocopy:raw` did the reverse. Order must not
     matter.

  Known gap: the `pattern` qualifier is inert end to end - it is absent
  from `prte_schizo_base_sanity`'s `outquals[]`, so the CLI rejects it
  before `parse_output` runs, and nothing in PRRTE consumes
  `PMIX_IOF_FILE_PATTERN` either.

- **`prte_schizo_base_expose(param, prefix)`** — split a `key=value`
  string and `setenv` it as `<prefix>key=value` (used by `parse_cli` to
  push `--prtemca`/`--pmixmca` values into the environment).

### `schizo_base_stubs.c` — proxy stub, argv normalization, MCA parsers, fork

- **`prte_schizo_base_detect_proxy(cmdpath)`** — the personality-election
  stub described above.
- **`prte_schizo_base_normalize_argv(argv)`** — rewrites the deprecated
  **hyphenated** long-option spellings in place (`--map-by`→`--mapby`,
  `--rank-by`→`--rankby`, `--bind-to`→`--bindto`,
  `--runtime-options`→`--rtos`) so the option tables (which use the
  canonical un-hyphenated keys) match, and **returns the `--personality`
  value** (a pointer into `argv`, not a copy). `--rank-by`/`--bind-to`
  are renamed unconditionally on every occurrence because MPMD lines may
  repeat them per app-context; detecting an illegal duplicate is the
  MPMD parser's job.

  This runs on the raw argv **before any option table exists**, so it
  has to reproduce what `getopt_long` will later accept: both
  `--opt value` **and** `--opt=value`. Handling only the first form
  leaves the `=` spelling to be rejected downstream as an unrecognized
  option (for the renamed options) or — worse — silently ignored (for
  `--personality`, which then falls back to the default personality and
  rejects every option only the requested personality defines). The
  renames are driven by the `normalized_opts[]` table; add both forms at
  once by adding a row, and note the `=` match is anchored (`argv[i][len]
  == '='`) so `--map-by-something` is not mistaken for `--map-by`.
- **`prte_schizo_base_parse_prte` / `_parse_pmix`** — shared scanners
  that pull `--prtemca`/`--mca` (and `--pmixmca`/`--gpmixmca`/`--gmca`)
  triples out of an argv, map generic `--mca fw ...` to the right
  project (`pmix_pmdl_base_check_prte_param` / `_check_pmix_param`),
  handle framework renames (`if`→`prteif`/`pif`, `reachable`→
  `prtereachable`/`preachable`, `plm_rsh`→`plm_ssh`, and — on the PMIx
  side only — `dl`→`pdl`), and either push them into the environment
  (`target == NULL`) or append them to a target argv.
- **`prte_schizo_base_add_directive` / `_add_qualifier`** — the
  option→attribute plumbing used by deprecated-option conversion. They
  merge a directive/qualifier into an existing `pmix_cli_item_t` value
  (respecting the "may this option take multiple directives?" whitelist:
  `display`, `output`, `tune`, `rtos`), handle the leading-`:` qualifier
  form, and optionally emit the "deprecated-converted" warning.
- **`prte_schizo_base_setup_fork(jdata, app)`** — the shared `setup_fork`
  impl. Sets `PRTE_LAUNCHED=1` and applies the **PMIx prefix**: the app's
  own `PRTE_APP_PMIX_PREFIX` (plus the matching `LD_LIBRARY_PATH` entry),
  or the DVM-wide default from the daemon job's `PRTE_JOB_PMIX_PREFIX` if
  the app named none.

  It does **not** apply the generic envar directives
  (`SET`/`ADD`/`UNSET`/`PREPEND`/`APPEND_ENVAR`). `odls`'
  `process_envars()` applies all of those to `app->env` in the statement
  immediately before it calls this hook, and doing it a second time is
  not a no-op: `PREPEND`/`APPEND` **edit** the existing value, so applying
  them twice yields `head:head:middle` and duplicates every entry a user
  prepends onto `PATH` or `LD_LIBRARY_PATH`. `process_envars()` owns them
  because it runs for every personality regardless of what that
  personality's `setup_fork` does. If you are adding envar handling, add
  it there, not here.

- **`prte_schizo_base_getline` / `_strip_quotes` / `_root_error_msg`** —
  small shared utilities. The first two are fed **user data** (MCA
  param/tune files, `--prtemca` values), so they must not assume it is
  well formed: `getline` only strips a trailing newline if there is one
  (a file whose last line has none would otherwise lose a character),
  and `strip_quotes` only inspects a last character if the string has
  one (`""` used to index `pout[-1]`, writing in front of the
  allocation).

### `schizo_base_select.c`

Just the multi-select loop described above (query all, keep all,
priority-insert, dump the final priority list at verbosity > 4).

---

## Conventions & gotchas specific to this framework

- **Two personalities, one behavior contract.** `prte` and `ompi` are
  deliberately parallel. When you add or change a CLI option, a
  deprecation, or an env translation, ask whether the *other*
  personality needs the mirror change — divergence here is a common
  source of "works with `prun`, breaks with `mpirun`" bugs.
- **Deprecated options are rewritten, not rejected.** Each component's
  `convert_deprecated_cli` folds old spellings into the modern
  directive/qualifier form via `prte_schizo_base_add_directive/
  _add_qualifier`, and warns exactly once (gated by the component's
  `warn_deprecations` MCA param and one-shot `warned` flag).
- **Every deprecated entry in an option table needs a conversion.** An
  option that a table defines but `convert_deprecated_cli` has no branch
  for parses perfectly and is then **silently discarded** — the user
  asks for something and gets nothing, with no error. That is how
  `--merge-stderr-to-stdout` and `--display-devel-allocation` came to be
  no-ops under the prte personality while `--merge-stderr-to-stdout`
  worked under ompi. When adding a deprecated spelling, add the table
  entry, the conversion, and the mirror in the *other* personality, in
  one change; when reviewing, diff the set of literal option strings in
  the tables against the set of `strcmp(option, "...")` branches in the
  converter. (Some converter branches have no table entry — those are
  merely dead, not harmful.)
- **A conversion's `report` argument is the component's `warn`, never a
  literal `true`.** Hard-coding it makes that one option scold users who
  have deliberately turned deprecation warnings off — which, for ompi,
  is the default.
- **Help text is embedded, not read at runtime.** These `help-*.txt`
  files feed the generated `show_help` content. Per the GOLDEN RULE in
  the top-level guide, after editing any `help-*.txt` you must
  `rm src/util/prte_show_help_content.*` and rebuild, or the binary
  serves stale text.
- **`check_sanity` is a developer guard, not user UX.** Its show_help
  output is aimed at contributors; real user-facing validation of a
  directive belongs in the owning framework (mostly `rmaps`).
- **Option keys are the un-hyphenated canonical spellings** (`PRTE_CLI_*`
  from `src/util/prte_cmd_line.h`). `normalize_argv` exists precisely so
  the tables never have to carry both spellings.
- Standard PRRTE rules apply: `prte_config.h` first, braces on every
  block, constant-on-left comparisons, `PMIX_NEW`/`PMIX_RELEASE` for
  objects, no new compiler warnings.

---

## Debugging

```sh
prterun --prtemca schizo_base_verbose 5 ...   # trace personality selection + parsing
```

At verbosity ≥ 5 the base prints every component it queries, whether it
returned a module, and — critically — the **final schizo priority
list**. `detect_proxy` logs at verbosity ≥ 2 which personality is
bidding on the invocation and with what confidence. The
`schizo_base_test_proxy_launch` MCA param exists to exercise the
proxy-launch path in testing.

---

## Testing

**Unit tests: [`test/unit/schizo/`](../../../test/unit/schizo/)**, wired
into `make check`. They need no DVM: they build `pmix_cli_result_t`
objects by hand (exactly as `pmix_cmd_line_parse` would — one instance
per key, every occurrence appended to that instance's value array) and
run the base helpers and each personality's `parse_cli` over them.

| File | Covers |
|------|--------|
| `test_normalize.c` | `normalize_argv` (both option spellings, personality capture), `strip_quotes`, `getline`, `expose` |
| `test_directives.c` | `add_directive`/`add_qualifier` merging rules, `check_directives`/`check_qualifiers` incl. the `ppr:N:resource` pattern |
| `test_sanity.c` | `prte_schizo_base_sanity`: duplicates, `check_ndirs`, synonyms, bad directives, the PE/bind-to conflict |
| `test_output.c` | `parse_output`/`parse_display` — asserts on the resulting `PMIX_INFO` array, which is the only way to see a directive that was silently dropped |
| `test_personality.c` | `detect_proxy` election (incl. an unknown personality returning NULL) and both personalities' deprecated-option conversions |

Reach a personality through the framework (`schizo_test_module()`), not
by naming its module symbol — the module belongs to its component, which
may be a DSO and is not visible outside `libprrte` in any case.

**Multi-node: `test_schizo()` in
[`contrib/dockerswarm/run-tests.sh`](../../../contrib/dockerswarm/).**
What the unit tests cannot show is that the *result* of a translation
survives the trip to a remote daemon — the envar directives are applied
by `setup_fork` on the prted that forks the process, `--output file=…`
is written per-daemon, and a job's personality is resolved again on
every daemon by `odls`. Those cases, plus "an unknown personality must
be refused without taking a persistent DVM down", live there.

---

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`prte/AGENTS.md`](prte/AGENTS.md) — the native PRRTE personality;
  read this second.
- [`ompi/AGENTS.md`](ompi/AGENTS.md) — the Open MPI `mpirun` emulation.
