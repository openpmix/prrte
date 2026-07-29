# AGENTS.md — `src/tools/prte` (start a DVM)

Orientation for the `prte` executable. Start with
[`src/tools/AGENTS.md`](../AGENTS.md) for what all the tools share.

---

## There is no code here

`main.c` is three lines:

```c
int main(int argc, char *argv[])
{
    return prte(argc, argv);
}
```

`prte()` is [`src/prted/prte.c`](../../prted/prte.c) — the longest
function in the tree — and
[`src/prted/AGENTS.md`](../../prted/AGENTS.md) documents it in detail.
**That is the file to read and to change.** This directory exists to
produce the binary and to install it under two names.

---

## Two names, one binary

`install-exec-hook` symlinks `prterun` → `prte`. Unlike the legacy
symlinks in the other tool directories, this one is *not* conditional on
`--with-legacy-tools`, because `prterun` is not a legacy name: it is a
second personality of the same program.

- **`prte`** starts a persistent DVM and waits for `prun` to submit jobs
  to it.
- **`prterun`** runs one job to completion and exits — a self-contained
  `mpirun` equivalent. With `--dvm` it instead hands straight off to
  `prun_common()` and never starts a DVM at all.

`prte()` tells them apart through `prte_tool_actual` (set from the
basename before anything else) and `prte_schizo_base_detect_proxy()`,
and the two get **different option tables and different help files** in
[`schizo_prte.c`](../../mca/schizo/prte/schizo_prte.c) — `prteoptions`
/`help-prte.txt` versus `prterunoptions`/`help-prterun.txt`. An option
added for one is not available in the other unless you add it to both.

`uninstall-local` here removes the symlink *and* the binary; automake
would remove the binary anyway, so the second `rm` is belt-and-braces.

---

## The `AM_CFLAGS` block

Every tool directory that reports build provenance
(`PRTE_CONFIGURE_USER`, `PRTE_BUILD_DATE`, `PRTE_CC_ABSOLUTE`, …) defines
those macros in its own `Makefile.am`, because they are stamped at
compile time into the object that prints them. If you add a field to the
version/config report, it has to be added to *each* of those
`Makefile.am` files that needs it — they are not shared.

---

## Testing

Nothing here to unit test. The DVM startup path this binary opens is
covered by the live smoke test (`prte --daemonize` → `prun` → `pterm`),
by `test/unit/prted/`, and by
[`contrib/dockerswarm/`](../../../contrib/dockerswarm/) for everything
that only exists across nodes.
