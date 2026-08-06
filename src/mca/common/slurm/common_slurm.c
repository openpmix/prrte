/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_string_copy.h"

#include "common_slurm.h"

/* The version thresholds, each named for what it gates.
 *
 * Do not fold these into one "minimum supported version".  They are three
 * different questions with three different answers, and a component that
 * asked the wrong one would either refuse a Slurm it can drive or drive one
 * it cannot.
 */
#define PRTE_SLURM_ANCIENT_MAJOR  17   /* below 17.11 PRRTE cannot launch */
#define PRTE_SLURM_ANCIENT_MINOR  11
#define PRTE_SLURM_EARLY_MAJOR    23   /* below 23.11 srun has no */
#define PRTE_SLURM_EARLY_MINOR    11   /*   --external-launcher */
#define PRTE_SLURM_EXTENDED_MAJOR 24   /* 24.05 and up emit the JSON schema */
#define PRTE_SLURM_EXTENDED_MINOR 5    /*   ras/slurm parses */

static prte_common_slurm_version_t slurm_version = {
    .available = false,
    .version = "unknown",
    .major = 0,
    .minor = 0,
    .ancient = false,
    .early = false,
    .extended = false
};
static bool probed = false;

/* Is (major.minor) at or after (want_major.want_minor)? */
static bool at_least(int major, int minor, int want_major, int want_minor)
{
    if (major > want_major) {
        return true;
    }
    if (major < want_major) {
        return false;
    }
    return (minor >= want_minor);
}

/**
 * Run "<cmd> --version" and parse "slurm <major>.<minor>..." out of it.
 *
 * Returns true when a version was obtained.  Note that a Slurm client
 * establishes a configuration source BEFORE it will print its version, so on
 * a machine with the binaries but no reachable slurm.conf this fails with
 * "Could not establish a configuration source" and no version line at all.
 * That is a legitimate answer -- "there is no Slurm here to speak of" -- not
 * an error to report.
 */
static bool probe_command(const char *cmd)
{
    FILE *fp;
    char line[1024], *ptr, *endptr;
    long major, minor;

    fp = popen(cmd, "r");
    if (NULL == fp) {
        return false;
    }
    if (NULL == fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return false;
    }
    pclose(fp);

    /* The line has to BE a version report.  Slurm prefixes its diagnostics
     * with the tool name ("srun: fatal: ..."), so anything that does not
     * start with "slurm " is an error message, not an answer. */
    if (0 != strncmp(line, "slurm ", 6)) {
        return false;
    }
    ptr = line + 6;

    /* Parse on the dots.  Step over the separator only if there IS one:
     * "slurm 23" with no minor leaves ptr on the terminating NUL, and
     * walking past that reads whatever the uninitialized tail of the fgets
     * buffer happens to hold. */
    major = strtol(ptr, &endptr, 10);
    if (endptr == ptr) {
        return false;
    }
    ptr = endptr;
    if ('\0' != *ptr) {
        ++ptr;
    }
    minor = strtol(ptr, &endptr, 10);
    if (endptr == ptr) {
        minor = 0;
    }

    slurm_version.major = (int) major;
    slurm_version.minor = (int) minor;

    /* keep the reported string, trimmed of its newline, for diagnostics */
    ptr = line + 6;
    endptr = strpbrk(ptr, " \t\r\n");
    if (NULL != endptr) {
        *endptr = '\0';
    }
    pmix_string_copy(slurm_version.version, ptr, PRTE_COMMON_SLURM_VERSION_MAX);

    return true;
}

char *prte_common_slurm_jobid(void)
{
    char *jobid;

    /* The current spelling first, then the historical one.  Both are set by
     * every Slurm in service today, and they carry the same value when both
     * are present. */
    jobid = getenv("SLURM_JOB_ID");
    if (NULL == jobid) {
        jobid = getenv("SLURM_JOBID");
    }
    return jobid;
}

const prte_common_slurm_version_t *prte_common_slurm_version(void)
{
    /* Several commands are tried rather than just srun because the answer is
     * wanted by two components with two different tools in hand, and any of
     * them reports the same installation's version.  If one is missing from
     * PATH -- a login node with the client tools split across packages, a
     * container with only part of the install -- the next one answers. */
    static const char *const cmds[] = {"srun --version 2>/dev/null",
                                       "scontrol --version 2>/dev/null",
                                       "sbatch --version 2>/dev/null",
                                       "sinfo --version 2>/dev/null",
                                       NULL};
    int i;

    if (probed) {
        return &slurm_version;
    }
    probed = true;

    for (i = 0; NULL != cmds[i]; i++) {
        if (probe_command(cmds[i])) {
            slurm_version.available = true;
            break;
        }
    }

    if (!slurm_version.available) {
        return &slurm_version;
    }

    slurm_version.ancient = !at_least(slurm_version.major, slurm_version.minor,
                                      PRTE_SLURM_ANCIENT_MAJOR, PRTE_SLURM_ANCIENT_MINOR);
    slurm_version.early = !at_least(slurm_version.major, slurm_version.minor,
                                    PRTE_SLURM_EARLY_MAJOR, PRTE_SLURM_EARLY_MINOR);
    slurm_version.extended = at_least(slurm_version.major, slurm_version.minor,
                                      PRTE_SLURM_EXTENDED_MAJOR, PRTE_SLURM_EXTENDED_MINOR);

    return &slurm_version;
}
