/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "ras_slurm.h"
#include "src/mca/common/slurm/common_slurm.h"
#include "src/mca/ras/base/base.h"

/**
 * @brief Can we serve an elastic allocation request?
 *
 * The whole modify surface -- extend, release and cancel -- rests on reading
 * "scontrol show job <id> --json", and three independent things can stop it.
 * Each of the three entry points asks here first, so a request is refused
 * cleanly rather than half-served, and so the refusal names the actual
 * reason.
 *
 * Two of the three are settled at configure time and folded into one flag by
 * configure.m4: jansson, and whether the Slurm found there was new enough.
 * The third is only knowable here: PRRTE is very often built somewhere other
 * than where it runs -- a build node, a container image, a shared filesystem
 * older than the cluster around it -- so the version that matters is the one
 * on THIS machine, which src/mca/common/slurm probes once and both Slurm
 * components read.
 *
 * The reasons are reported separately on purpose.  They used to share one
 * message naming jansson, which is a plain falsehood on a machine that has
 * jansson and a Slurm too old to emit the schema the parser reads -- and
 * that machine is every stock Ubuntu 24.04, so it is not a corner case.
 *
 * @return true when the request can be served, false (having said why) when
 *         it cannot.
 */
bool prte_ras_slurm_have_extensions(void)
{
    const prte_common_slurm_version_t *slurm;

#if PRTE_HAVE_SLURM_EXTENSIONS
    if (!prte_ras_slurm_have_jansson()) {
        pmix_output(0, "ras:slurm:modify: "
                    "Jansson support is required but not enabled in this build");
        return false;
    }

    /* The build is capable; is the scheduler in front of us?  A Slurm we
     * cannot interrogate is NOT treated as too old: this component only runs
     * with SLURM_JOBID set, so something launched us, and refusing on a
     * failed popen would turn a transient into a policy. */
    slurm = prte_common_slurm_version();
    if (slurm->available && !slurm->extended) {
        pmix_output(0, "ras:slurm:modify: this Slurm (%s) is older than %s, "
                    "whose JSON schema the allocation extensions read. The "
                    "request cannot be served; extend/release/cancel need a "
                    "newer Slurm.",
                    slurm->version, PRTE_SLURM_MIN_EXT_VERSION);
        return false;
    }
    return true;
#else
    /* Say what is true of the build, and let the reader draw the conclusion.
     * Naming a single cause here would be wrong at least a third of the time:
     * the extensions are switched off by an old Slurm, by a missing jansson,
     * or by --disable-slurm-extensions, and configure folds all three into
     * one flag. */
    slurm = prte_common_slurm_version();
    pmix_output(0, "ras:slurm:modify: the Slurm elastic allocation extensions "
                "were not built into this PRRTE. They need jansson and Slurm "
                "%s or later; this tree was configured against Slurm %s, and "
                "is running under Slurm %s. Rebuild with "
                "--enable-slurm-extensions (and --with-jansson) to enable "
                "them.",
                PRTE_SLURM_MIN_EXT_VERSION, PRTE_SLURM_VERSION_STRING,
                slurm->version);
    return false;
#endif
}

/*
 * Cancel a Slurm job using scancel.
 *
 * If scancel returns an error, the stderr/stdout output is copied
 * into err_msg (truncated to buffer size). On success, err_msg is cleared.
 *
 * @param[in]  slurm_jobid   Null-terminated Slurm job ID string to cancel.
 * @param[out] err_msg       Buffer for error message, or NULL.
 * @param[in]  err_msg_size  Size of err_msg buffer, if applicable
 */
int prte_ras_slurm_kill_job(const char *slurm_jobid, char *err_msg, size_t err_msg_size) {

    if(NULL == slurm_jobid) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    if(NULL != err_msg) {
        err_msg[0] = '\0';
    }

    int err = PRTE_SUCCESS;

    /* Make sure the job ID given is something reasonable */
    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:kill_job: killing job %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), slurm_jobid));

    static const char *cmd_format = "scancel %s 2>&1";

    char *cmd = NULL;

    FILE *fp = NULL;

    if(0 > asprintf(&cmd, cmd_format, slurm_jobid)) {
        cmd = NULL;
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    fp = popen(cmd, "r");

    if(NULL == fp) {
        err = PRTE_ERR_FILE_OPEN_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    err = prte_ras_slurm_drain_cmd_output(fp, err_msg, err_msg_size);

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    int status = pclose(fp);
    fp = NULL;

    if (-1 == status) {
        pmix_output(0, "ras:slurm:kill_job: pclose failed: %s.", strerror(errno));
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        err = PRTE_ERR_SLURM_CANCEL_FAILURE;
        goto cleanup;
    }

    cleanup:

    if(NULL != err_msg && PRTE_ERR_SLURM_CANCEL_FAILURE != err) {
        err_msg[0] = '\0';
    }

    if(NULL != fp) {
        pclose(fp);
    }

    free(cmd);

    return err;
}

/*
 * Check whether a string contains control characters
 *
 * Rejects the string if it contains any control characters
 */
int prte_ras_slurm_token_has_control_chars(const char *s, size_t len, bool *has_control_chars)
{
    if (NULL == s || NULL == has_control_chars) {
        return PRTE_ERR_BAD_PARAM;
    }

    *has_control_chars = false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        /* check if control character */
        if (c < 0x20 || c == 0x7f) {
            *has_control_chars = true;
            break;
        }
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Drain command output and optionally capture it.
 *
 * Consumes all data from a popen() stream. When provided, output is
 * populated with command output, potentially truncated to fit the buffer.
 *
 * @param[in] fp Stream returned by popen().
 * @param[out] output Optional output buffer.
 * @param[in] output_size Size of output buffer.
 */
int prte_ras_slurm_drain_cmd_output(FILE *fp, char *output, size_t output_size)
{
    if (NULL == fp) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    if (NULL != output && 0 == output_size) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    char buf[256];
    size_t used = 0;
    bool truncated = false;

    if (NULL != output) {
        output[0] = '\0';
    }

    while (NULL != fgets(buf, sizeof(buf), fp)) {
        if (NULL == output || truncated) {
            continue;
        }

        if (used >= output_size - 1) {
            truncated = true;
            continue;
        }

        size_t len = strlen(buf);
        size_t avail = output_size - used - 1;

        if (len > avail) {
            memcpy(output + used, buf, avail);
            used += avail;
            output[used] = '\0';
            truncated = true;
        } else {
            memcpy(output + used, buf, len);
            used += len;
            output[used] = '\0';
        }
    }

    if (ferror(fp)) {
        PRTE_ERROR_LOG(PRTE_ERR_FILE_READ_FAILURE);
        return PRTE_ERR_FILE_READ_FAILURE;
    }

    if (NULL != output && truncated && output_size >= 4) {
        strcpy(output + output_size - 4, "...");
    }

    return PRTE_SUCCESS;
}
