# AGENTS.md — `schizo/prte` (the native PRRTE personality)

Component guide for `src/mca/schizo/prte/`. Read the
[framework guide](../AGENTS.md) first for the module vtable, the
"keep all / detect_proxy" selection model, and the shared base helpers
referenced throughout.

---

## Role and selection

`prte` is the **native, catch-all personality**. It defines the option
sets for all six PRRTE-native tools and is the personality PRRTE falls
back to whenever nothing else is explicitly requested.

- Component static priority: **5** (below `ompi`'s 50), registered in
  `schizo_prte_component.c`.
- `detect_proxy` (in `schizo_prte.c`) bids:
  - **100** if the environment (`PRTE_MCA_schizo_proxy` or
    `PRTE_MCA_personality`) names `prte` exactly;
  - **0** if a `--personality` list was given that does *not* contain
    `prte`, or the env names a different personality;
  - otherwise its **static priority (5)** — i.e. it always makes a
    low bid, so with no personality hint the invocation lands here.

This "always bids at least 5, never 0 by default" behavior is what makes
`prte` the default. `ompi` bids 0 unless explicitly selected, so an
un-hinted `prun`/`prte`/`prterun` is a prte-personality invocation.

---

## Files

| File | Contents |
|------|----------|
| `schizo_prte.c` | The whole module: the six per-tool option tables, `parse_cli`, `convert_deprecated_cli`, `detect_proxy`, `parse_env`, `allow_run_as_root`, `job_info`, `set_default_rto`, and the `prte_schizo_prte_module` vtable. |
| `schizo_prte.h` | `prte_schizo_prte_component_t` (adds `priority`, `warn_deprecations`, `warned`) and the module/component externs. |
| `schizo_prte_component.c` | Registration (`warn_deprecations` MCA param, default **true**), `component_query` returning the module at priority 5. |
| `configure.m4` | Always builds; adds `PRTE` to the "Personalities" summary. |
| `help-prte.txt`, `help-prterun.txt`, `help-prun.txt`, `help-prted.txt`, `help-pterm.txt`, `help-prte-info.txt` | Per-tool usage/help text (one file per tool, matched to the option table). |

The module vtable wires up: `parse_cli`, `parse_env`,
`setup_fork = prte_schizo_base_setup_fork`, `detect_proxy`,
`allow_run_as_root`, `job_info`, `set_default_rto`, and
`check_sanity = prte_schizo_base_sanity`. Everything else
(`init`, `set_default_mapping/ranking/binding`, `setup_app`, `finalize`)
is left `NULL`, so the rmaps/state base defaults apply.

---

## The option sets — one table per tool

There is no `define_cli` hook. Instead `schizo_prte.c` carries six
static `struct option[]` tables plus their short-option strings, and
`parse_cli` picks the right pair by matching `prte_tool_actual`:

| `prte_tool_actual` | Table | Shorts | Help file |
|--------------------|-------|--------|-----------|
| `prte` | `prteoptions` | `prteshorts` (`h::vVx:H:`) | `help-prte.txt` |
| `prterun` | `prterunoptions` | `prterunshorts` | `help-prterun.txt` |
| `prted` | `prtedoptions` | `prtedshorts` | `help-prted.txt` |
| `prun` | `prunoptions` | `prunshorts` | `help-prun.txt` |
| `pterm` | `ptermoptions` | `ptermshorts` | `help-pterm.txt` |
| `prte_info` | `pinfooptions` | `pinfoshorts` | `help-prte-info.txt` |

Each table entry is a `PMIX_OPTION_DEFINE(KEY, ARG)` or
`PMIX_OPTION_SHORT_DEFINE(KEY, ARG, 'c')` macro, where `KEY` is a
canonical `PRTE_CLI_*` constant from `src/util/prte_cmd_line.h`
(`PRTE_CLI_NP`, `PRTE_CLI_HOST`, `PRTE_CLI_MAPBY`, `PRTE_CLI_RTOS`,
`PRTE_CLI_DISPLAY`, …). The tables differ by role: `prte` is DVM-startup
heavy (`--daemonize`, `--system-server`, `--set-sid`, `--report-uri`);
`prun`/`prterun` add the launch/placement/IO options (`-n`, `-N`,
`--map-by`, `--bind-to`, `-x`, `--output`, `--pset`, env-manipulation
options); `prted` and `pterm` are minimal.

Because the tables use the **un-hyphenated** canonical keys
(`mapby`, `rankby`, `bindto`, `rtos`), the tool must first call
`prte_schizo_base_normalize_argv()` (done in the tool `main`, not here)
to rewrite the deprecated hyphenated spellings — otherwise those options
won't match.

---

## `parse_cli`

1. Selects `myoptions`/`shorts`/`helpfile`/`sdprefix` from
   `prte_tool_actual`.
2. Sets the tool banner (`pmix_tool_msg`/`_org`/`_version`) and the
   session-dir prefix.
3. Calls `pmix_cmd_line_parse(argv, shorts, myoptions, …, results,
   helpfile)`. A `PMIX_OPERATION_SUCCEEDED` return (e.g. `--version`
   already printed) is mapped to `PRTE_OPERATION_SUCCEEDED`.
4. Calls `convert_deprecated_cli` to rewrite old options.
5. Walks the parsed `results` and eagerly pushes any `--prtemca` /
   `--pmixmca` values into the environment via
   `prte_schizo_base_expose(value, "PRTE_MCA_"/"PMIX_MCA_")` so later
   framework opens see them.

## `convert_deprecated_cli`

The deprecation engine. Gated by `warn_deprecations` (default **true**
for prte) and the one-shot `warned` flag, it walks `results->instances`
and folds legacy spellings into modern directives/qualifiers using
`prte_schizo_base_add_directive` / `_add_qualifier`, then removes the old
instance with `PMIX_CLI_REMOVE_DEPRECATED`. Examples:

- `--n` → `--np` (silent, no warning).
- `--nolocal` → `--map-by :NOLOCAL` (qualifier).
- and the rest of the classic ORTE-era options mapped onto the current
  `--map-by`/`--rank-by`/`--bind-to`/`--output`/`--display` directive
  vocabulary.

Because prte defaults `warn_deprecations = true`, native-tool users get a
warning for each converted option — the opposite of `ompi` (see below).

## Other hooks

- **`parse_env`** — effectively a no-op (logs and returns
  `PRTE_SUCCESS`). The native personality forwards no special envars; app
  environment handling happens through attributes in `setup_fork`.
- **`allow_run_as_root`** — honors `--allow-run-as-root`, or the pair
  `PRTE_ALLOW_RUN_AS_ROOT` + `PRTE_ALLOW_RUN_AS_ROOT_CONFIRM` both set to
  `1`; otherwise calls `prte_schizo_base_root_error_msg()` (which
  exits).
- **`job_info`** — no-op.
- **`set_default_rto`** — delegates to
  `prte_state_base_set_runtime_options(jdata, NULL)`.
- **`setup_fork`** and **`check_sanity`** — the shared base
  implementations (`prte_schizo_base_setup_fork`,
  `prte_schizo_base_sanity`).

---

## How it differs from `ompi`

- **Default warning behavior is opposite:** prte warns on deprecated
  options by default; ompi is silent by default.
- **`parse_env` is trivial here.** ompi's `parse_env` is a large
  OMPI-MCA translator; prte's does nothing.
- **No default-placement overrides.** prte leaves
  `set_default_ranking`/`mapping`/`binding` `NULL`; ompi overrides
  `set_default_ranking` (dense-pack for PPR).
- **Six explicit option tables** keyed on the tool name, versus ompi's
  single `mpirun`-style table.
- **detect_proxy defaults to a low nonzero bid**, so prte is the
  fallback personality; ompi defaults to 0.

---

## Gotchas when editing

- **Add an option to the *right* table(s).** An option only visible to
  `prun` users goes in `prunoptions` (and probably `prterunoptions`),
  not `prteoptions`. Forgetting `prterun` is a common miss because
  `prterun` = `prte` + `prun` in one shot.
- **Use canonical un-hyphenated `PRTE_CLI_*` keys**, and if you add a
  deprecated spelling, wire its conversion in `convert_deprecated_cli`
  rather than adding a second table entry.
- **Mirror in `ompi` when it's a shared concept.** Placement, output,
  and runtime-option changes usually need the ompi table/handler updated
  too, or the two personalities drift.
- **Edit the matching `help-*.txt`** for any user-visible option change,
  then follow the GOLDEN RULE (`rm src/util/prte_show_help_content.*`
  and rebuild) or the binary serves stale help.
- **`warned` is one-shot per process.** Deprecation warnings fire once;
  don't rely on repeated warnings in a single tool invocation.
