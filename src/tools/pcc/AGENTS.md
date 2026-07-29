# AGENTS.md — `src/tools/pcc` (the compiler wrapper that isn't ours)

Orientation for the `pcc` "tool". Start with
[`src/tools/AGENTS.md`](../AGENTS.md) for what all the tools share.

---

## There is nothing to build here

This directory contains a `Makefile.am` and no source, because `pcc` is
not a PRRTE program. It is a symlink to PMIx's `pmixcc`:

```make
if PRTE_HAVE_PMIXCC
install-exec-hook:
	(cd $(DESTDIR)$(bindir); rm -f pcc; $(LN_S) $(PMIXCC_PATH)$(EXEEXT) pcc)
```

`PMIXCC_PATH` and the `PRTE_HAVE_PMIXCC` conditional are set by
[`config/prte_setup_pmix.m4`](../../../config/prte_setup_pmix.m4), which
looks for `pmixcc` in the PMIx installation and probes it with
`--showme:version`. If that fails, configure warns and no `pcc` is
installed — that is not an error, and nothing in PRRTE depends on it.

---

## Why PRRTE ships it at all

PRRTE has no linkable library, so there is nothing for a PRRTE compiler
wrapper to link against. `pcc` is here purely as a convenience for
applications that embed **PMIx** directly and are being built alongside a
PRRTE installation — it compiles and links against PMIx, not PRRTE.

Consequences:

- **Do not add a PRRTE compiler wrapper here.** If an application needs
  something from PRRTE at link time, that is a design question for the
  project, not a wrapper change.
- **Bugs in the wrapper's behavior belong to PMIx** — file them at
  <https://github.com/openpmix/openpmix>. The only thing this directory
  can get wrong is the symlink.
- **A stale `pcc` symlink** pointing at a PMIx that has been moved or
  removed is the one failure mode to look for here; it is an absolute
  path baked in at install time.
