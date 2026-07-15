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
  `setup_fork` injects personality-specific envars into each local
  process's environment just before `fork`/`exec` (called from
  `odls/base`), and the `set_default_mapping`/`ranking`/`binding`/`rto`
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
| `setup_fork` | `(prte_job_t *, prte_app_context_t *)` | Inject job/app envar attributes into `app->env` just before fork. Both use `prte_schizo_base_setup_fork` (shared base impl). |
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
**strictly highest** returned confidence (ties keep the first/higher
static-priority module, since the test is `pri < p`). Each component's
`detect_proxy` decides its own confidence from three signals, in
override order:

1. **Explicit `--personality` list** (passed in as `cmdpath`): if it
   names the component, that component bids (prte bids its static
   priority `5`; ompi bids `translate_params()` = `100`); otherwise it
   bids `0`.
2. **Environment** (`PRTE_MCA_schizo_proxy` or `PRTE_MCA_personality`):
   an exact match bids `100`, a mismatch bids `0`.
3. **Default**: prte falls back to its static priority (`5`) — it is the
   catch-all; ompi bids `0` (it never claims an invocation it wasn't
   explicitly asked for).

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
- **`prte_schizo_base_check_directives` / `_check_qualifiers`** — the
  reusable directive validators, including the special-cased
  `--map-by ppr:N:resource` pattern check.
- **`prte_schizo_base_parse_display` / `_parse_output`** — convert a
  parsed `--display` / `--output` option into `PMIX_INFO` list entries
  (`PMIX_DISPLAY_MAP`, `PMIX_IOF_TAG_OUTPUT`, `PMIX_IOF_OUTPUT_TO_FILE`,
  …) on the job-info object. These are where the human-facing directive
  strings become PMIx keys.
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
- **`prte_schizo_base_parse_prte` / `_parse_pmix`** — shared scanners
  that pull `--prtemca`/`--mca` (and `--pmixmca`/`--gpmixmca`/`--gmca`)
  triples out of an argv, map generic `--mca fw ...` to the right
  project (`pmix_pmdl_base_check_prte_param` / `_check_pmix_param`),
  handle framework renames (`if`→`prteif`/`pif`, `reachable`→
  `prtereachable`/`preachable`, `dl`→`prtedl`/`pdl`,
  `plm_rsh`→`plm_ssh`), and either push them into the environment
  (`target == NULL`) or append them to a target argv.
- **`prte_schizo_base_add_directive` / `_add_qualifier`** — the
  option→attribute plumbing used by deprecated-option conversion. They
  merge a directive/qualifier into an existing `pmix_cli_item_t` value
  (respecting the "may this option take multiple directives?" whitelist:
  `display`, `output`, `tune`, `rtos`), handle the leading-`:` qualifier
  form, and optionally emit the "deprecated-converted" warning.
- **`prte_schizo_base_setup_fork(jdata, app)`** — the shared `setup_fork`
  impl. Sets `PRTE_LAUNCHED=1`, then walks job-level then app-level
  attributes and applies every envar directive (`SET`/`ADD`/`UNSET`/
  `PREPEND`/`APPEND_ENVAR`, `PRTE_APP_PMIX_PREFIX`) into `app->env` **in
  order** (app-level overrides job-level), and applies a default
  `PMIX_PREFIX`/`LD_LIBRARY_PATH` if none was set on the app.
- **`prte_schizo_base_getline` / `_strip_quotes` / `_root_error_msg`** —
  small shared utilities.

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

## Where to go next

Each component directory has its own `AGENTS.md`:

- [`prte/AGENTS.md`](prte/AGENTS.md) — the native PRRTE personality;
  read this second.
- [`ompi/AGENTS.md`](ompi/AGENTS.md) — the Open MPI `mpirun` emulation.
