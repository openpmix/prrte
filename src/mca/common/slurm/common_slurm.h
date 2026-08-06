/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * What version of Slurm are we running under, and what does it support?
 *
 * Two components need this answer and neither owns it.  `plm/slurm` has to
 * know whether `srun` takes `--external-launcher` and whether the release is
 * too old to launch with at all; `ras/slurm` has to know whether
 * `scontrol show job --json` emits the schema its parser reads.  Asking is
 * not free -- it is a `popen()` of a Slurm client, which on a busy login node
 * is not something to do twice -- and, worse, two components asking
 * separately is two places for the answer to drift.
 *
 * So the question is asked once, here, on first use, and the answer is
 * cached.  This library is built into libprrte (or, in a DSO build, is a
 * single shared library both components link), so "once" means once per
 * process.
 *
 * NOTE: the probe runs a subprocess, so call it from the progress thread
 * only -- which every current caller does, since both are inside component
 * selection or an allocation request.
 */
#ifndef PRTE_COMMON_SLURM_H
#define PRTE_COMMON_SLURM_H

#include "prte_config.h"
#include "types.h"

BEGIN_C_DECLS

#define PRTE_COMMON_SLURM_VERSION_MAX 64

/**
 * What the local Slurm installation is, and what that implies.
 *
 * The three booleans are named for the question each component actually
 * asks, not for a version number, because the version each of them cares
 * about is different and none of them is the "current" release:
 */
typedef struct {
    /** A Slurm command answered with a version we could parse.  When false
     *  every field below is meaningless: there is no Slurm here, or it
     *  cannot reach a configuration source (a client that cannot find
     *  slurm.conf exits without printing a version at all). */
    bool available;
    /** The version as Slurm reported it, e.g. "24.11.6"; "unknown" when
     *  !available.  Kept verbatim for diagnostics -- a message that names
     *  the version the user is actually running is the difference between
     *  a fixable report and a puzzle. */
    char version[PRTE_COMMON_SLURM_VERSION_MAX];
    int major;
    int minor;

    /** Older than 17.11: PRRTE cannot launch under it at all (plm/slurm). */
    bool ancient;
    /** Older than 23.11: `srun` has no `--external-launcher` (plm/slurm). */
    bool early;
    /** 24.05 or newer: `scontrol show job --json` reports job_resources in
     *  the nested form ras/slurm's parser reads (data parser v0.0.41+).
     *  Older releases answer with a flat "allocated_nodes" array and the
     *  parse fails, which is what gates the elastic extensions. */
    bool extended;
} prte_common_slurm_version_t;

/**
 * Get the local Slurm version, probing once and caching the answer.
 *
 * Never returns NULL: when no Slurm can be interrogated the returned struct
 * has available == false and version == "unknown", which every caller must
 * handle -- it is the ordinary state on a machine that is not a Slurm
 * client.
 */
PRTE_EXPORT const prte_common_slurm_version_t *prte_common_slurm_version(void);

/**
 * The Slurm job id of the allocation we are running in, or NULL if there is
 * none -- which is also the test for "are we under Slurm at all", and is what
 * every Slurm component gates its availability on.
 *
 * Slurm renamed this variable: SLURM_JOBID is the historical spelling and
 * SLURM_JOB_ID is the current one, and which of them is present depends on
 * the Slurm release and on how the job was started.  Reading only the old
 * name means that on a Slurm which has stopped setting it, NO Slurm component
 * ever becomes available: ras/slurm does not discover the allocation,
 * plm/slurm does not launch with srun, ess/slurm does not select in the
 * daemon -- and none of that is reported as an error, because "not my
 * environment" is a legitimate answer everywhere it is asked. The DVM simply
 * falls back to ssh and a single-node allocation, and looks like it works.
 *
 * So there is exactly one place that knows both spellings, and it is here.
 */
/* Returns what getenv() returns, so this is a drop-in replacement for the
 * getenv() call each component used to make -- including the type. */
PRTE_EXPORT char *prte_common_slurm_jobid(void);

END_C_DECLS

#endif /* PRTE_COMMON_SLURM_H */
