# AGENTS.md — `schizo/ompi` (the Open MPI personality)

Component guide for `src/mca/schizo/ompi/`. Read the
[framework guide](../AGENTS.md) first for the module vtable, the
"keep all / detect_proxy" selection model, and the shared base helpers
referenced throughout.

---

## Role and selection

`ompi` makes PRRTE impersonate Open MPI's `mpirun`/`mpiexec`: it accepts
the Open MPI option set and, crucially, **translates Open MPI's MCA
conventions** (`OMPI_MCA_*` envars, `~/.openmpi/mca-params.conf`,
`$OMPIHOME/etc/openmpi-mca-params.conf`, `--mca`/`-mca` with OMPI
framework prefixes) into the PRRTE and PMIx MCA parameters that actually
drive the runtime.

- Component static priority: **50** (above `prte`'s 5), registered in
  `schizo_ompi_component.c`.
- `detect_proxy` (in `schizo_ompi.c`) bids:
  - **100** if the environment (`PRTE_MCA_schizo_proxy` /
    `PRTE_MCA_personality`) names `ompi` exactly;
  - **`translate_params()` (= 100)** if a `--personality` list contains
    `ompi` — and translating the OMPI params is a *side effect* of the
    bid, so it happens exactly when ompi is chosen this way;
  - otherwise **0** — ompi never claims an invocation it wasn't
    explicitly asked for.

In practice the `mpirun`/`mpiexec` wrappers set `OMPI_TOOL_NAME`,
`OMPI_VERSION`, and the personality env, so ompi wins; a bare
`prterun --personality ompi …` selects it via the `--personality` path.

---

## Files

| File | Contents |
|------|----------|
| `schizo_ompi.c` | The whole module (~2100 lines): the single `ompioptions` table, `parse_cli` (with single-dash correction), `convert_deprecated_cli`, the OMPI-MCA env translator (`parse_env`, `translate_params`, `check_prte_overlap`, `check_pmix_overlap`, `check_generic`, the framework-prefix list), `detect_proxy`, `allow_run_as_root`, `set_default_ranking`, `job_info`, `set_default_rto`, and the `prte_schizo_ompi_module` vtable. |
| `schizo_ompi.h` | `prte_schizo_ompi_component_t` (adds `priority`, `warn_deprecations`, `warned`) and externs. |
| `schizo_ompi_component.c` | Registration (`warn_deprecations` MCA param, default **false**), `component_query` returning the module at priority 50. |
| `configure.m4` | Gated by `--enable-ompi-support` (default yes / `--disable-ompi-support` to drop it); adds `OMPI` to the "Personalities" summary. |
| `help-schizo-ompi.txt` | `mpirun`-style `version`/`usage`/error help text. |
| `schizo-ompi-cli.rstxt` | RST source documenting the ompi CLI (rendered into the docs). |

The module vtable wires up: `parse_cli`, `parse_env`,
`setup_fork = prte_schizo_base_setup_fork`, `setup_app = NULL`,
`detect_proxy`, `allow_run_as_root`, `set_default_ranking`, `job_info`,
`set_default_rto`, and `check_sanity = prte_schizo_base_sanity`.

---

## The option set

Unlike prte's six per-tool tables, ompi has one table, `ompioptions`
(shorts `ompishorts` = `h::vVpn:c:N:sH:x:`), covering the `mpirun`
surface: `-n`/`-c`/`-N`, `-H`/`--host`, `-x`, `--map-by`/`--rank-by`/
`--bind-to`, `--output`/`--display`, `-p` (`--parseable`/`--parsable`),
`--report-pid`/`--report-uri`, plus the many legacy options folded in by
`convert_deprecated_cli`. Options use the same canonical `PRTE_CLI_*`
keys as prte.

`parse_cli` does extra work no other personality needs:

1. **Single-dash correction.** Open MPI historically accepted multi-char
   options with a single dash (`-output`, `-report-uri`). ompi's
   `parse_cli` scans a copy of argv and rewrites `-foo` → `--foo`
   (skipping `--mca`/`-mca` triples, which `mcaoption()` detects), so the
   standard `pmix_cmd_line_parse` can handle them. When `warn_deprecations`
   is on it records each corrected token and, after parsing, emits a
   deprecation notice listing the offenders that appeared *before* the
   user executable (`results->tail`).
2. **Tool identity.** If `OMPI_VERSION` + `OMPI_TOOL_NAME` are set (the
   `mpirun` wrappers set them), it adopts the Open MPI banner
   (`pmix_tool_basename`, `pmix_tool_org = "Open MPI"`, the OMPI bug URL);
   otherwise it's `prterun --personality ompi` and keeps the PRRTE
   identity.
3. Then runs `pmix_cmd_line_parse` against `ompioptions`/
   `help-schizo-ompi.txt` and `convert_deprecated_cli`.

`convert_deprecated_cli` is the ompi analogue of prte's — the same
`add_directive`/`add_qualifier` machinery — but gated by ompi's
`warn_deprecations`, which defaults to **false** (Open MPI users are not
warned about the legacy spellings they've always used).

---

## The OMPI → PRRTE/PMIx MCA translator

This is the defining feature of the ompi personality and has no
counterpart in prte.

- **`translate_params()`** (invoked from `detect_proxy` when ompi is
  chosen) walks, in reverse-precedence order so earlier sources win:
  1. `OMPI_MCA_*` environment variables;
  2. `~/.openmpi/mca-params.conf`;
  3. `$OMPIHOME/etc/openmpi-mca-params.conf`.
  For each `name=value` it asks whether the param overlaps PRRTE
  (`check_prte_overlap`) and/or PMIx (`check_pmix_overlap`), or belongs
  to a PRRTE/PMIx framework (`pmix_pmdl_base_check_prte_param` /
  `_check_pmix_param`), and `setenv`s the corresponding `PRTE_MCA_*` /
  `PMIX_MCA_*` variable **without overwriting** anything already set (so
  the user's explicit PRRTE/PMIx envars keep precedence). It returns
  `100` — hence detecting the ompi personality also carries a definitive
  confidence.
- **`check_prte_overlap` / `check_pmix_overlap`** encode the specific
  OMPI-name → PRRTE/PMIx-name equivalences (framework renames like OMPI
  `oob`/`if`/`dl`/`reachable`/`hwloc` mapping onto their PRRTE/PMIx
  counterparts).
- **`parse_env`** is the runtime-side companion: it forwards the
  user's `OMPI_MCA_*`/`-x`/tune-file/`--mca` selections that name real
  OMPI frameworks (validated by `check_generic` against the
  `ompi_frameworks[]` prefix list) into the app's `dstenv`, so MPI ranks
  see the MCA settings the user intended for the MPI layer.
- **`ompi_frameworks[]` / `setup_ompi_frameworks` / `check_generic`** —
  the static list of OPAL/OMPI/OSHMEM framework prefixes (`btl`, `pml`,
  `coll`, `osc`, `mpi`, `opal`, …), overridable at runtime via the
  `OMPI_MCA_PREFIXES` envar. `check_generic` uses it to decide whether an
  unknown `--mca fw …` belongs to Open MPI (and should be forwarded to
  the app env) versus PRRTE/PMIx.

---

## Other hooks

- **`allow_run_as_root`** — same shape as prte's, but keyed on
  `OMPI_ALLOW_RUN_AS_ROOT` + `OMPI_ALLOW_RUN_AS_ROOT_CONFIRM`.
- **`set_default_ranking`** — calls the rmaps base default, then, if the
  mapping policy is `PRTE_MAPPING_PPR` and the user did not set a ranking
  policy, forces `PRTE_RANK_BY_SLOT` (dense packing) to match Open MPI's
  expected default. This is the one default-placement override either
  personality installs.
- **`set_default_rto`** — delegates to
  `prte_state_base_set_runtime_options`.
- **`job_info`** — no-op.
- **`setup_fork` / `check_sanity`** — the shared base implementations.

---

## How it differs from `prte`

- **Silent by default** (`warn_deprecations = false`); prte warns.
- **Heavy `parse_env` + `translate_params`** for OMPI-MCA conventions;
  prte's `parse_env` is a no-op.
- **Single `mpirun` option table** with single-dash correction; prte has
  six per-tool tables and no dash correction.
- **Overrides `set_default_ranking`** (PPR dense-pack); prte overrides
  none.
- **`detect_proxy` bids 0 by default**; it must be explicitly selected.
  prte is the fallback.

---

## Gotchas when editing

- **Keep parity with `prte`.** A new placement/output/runtime option
  usually must land in *both* personalities. The two tables drifting is a
  classic "works with `prun`, not `mpirun`" bug.
- **The framework-prefix list is load-bearing.** `ompi_frameworks[]`
  decides what counts as an "Open MPI MCA param" and thus what gets
  forwarded to the app env vs. translated for PRRTE/PMIx. Adding an OMPI
  framework without listing it here means its `--mca` settings silently
  vanish. Users can extend it via `OMPI_MCA_PREFIXES`.
- **Precedence is intentional in `translate_params`.** It never
  overwrites an already-set target var, and it processes env → home file
  → OMPIHOME file so the *first* source seen wins. Don't reorder without
  understanding the "opposite of intended precedence" comment.
- **Single-dash correction only runs in ompi.** If you rely on it
  elsewhere you're in the wrong personality.
- **Edit `help-schizo-ompi.txt` / `schizo-ompi-cli.rstxt`** for
  user-visible changes, then follow the GOLDEN RULE
  (`rm src/util/prte_show_help_content.*` and rebuild) for the help text.
