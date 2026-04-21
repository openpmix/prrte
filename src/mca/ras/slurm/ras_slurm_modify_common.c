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
#include "src/mca/ras/base/base.h"

/*
 * Validate that a Slurm job ID is valid according to expected syntax
 *
 * A valid Slurm job ID must be non-NULL, non-empty, must not exceed
 * PRTE_SLURM_JOB_ID_MAX_LEN characters, and must contain only decimal digits.
 * 
 * @param[in] slurm_jobid  Null-terminated Slurm job ID string to validate.
 */
static int prte_ras_slurm_validate_jobid(const char *slurm_jobid) {

    if (NULL == slurm_jobid) {
        return PRTE_ERR_BAD_PARAM;
    }

    size_t id_len = strnlen(slurm_jobid, PRTE_SLURM_JOB_ID_MAX_LEN+1);
    if (0 == id_len || id_len > PRTE_SLURM_JOB_ID_MAX_LEN) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (size_t i = 0; i < id_len; ++i) {
        if (!isdigit((unsigned char)slurm_jobid[i])) {
            return PRTE_ERR_BAD_PARAM;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * Convert a Slurm job ID string to uint32_t.
 *
 * Expects a strictly decimal, non-negative string.
 *
 * @param[in]  slurm_jobid           Input string containing digits only.
 * @param[out] slurm_jobid_numeric   Converted value.
 */
static int prte_ras_slurm_convert_jobid(const char *slurm_jobid, uint32_t *slurm_jobid_numeric) {

    if (NULL == slurm_jobid || NULL == slurm_jobid_numeric) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (!isdigit((unsigned char)slurm_jobid[0])) {
        return PRTE_ERR_BAD_PARAM;
    }

    char *end = NULL;

    unsigned long slurm_id_ulong;
    
    errno = 0;
    slurm_id_ulong = strtoul(slurm_jobid, &end, 10);

    if ('\0' != *end || ERANGE == errno || slurm_id_ulong > UINT32_MAX) {
        return PRTE_ERR_BAD_PARAM;
    }

    *slurm_jobid_numeric = (uint32_t)slurm_id_ulong;

    return PRTE_SUCCESS;
}

/*
 * Cancel a Slurm job using scancel.
 *
 * If scancel returns an error, the first line of stderr/stdout output is copied
 * into err_msg. On success, err_msg is cleared.
 *
 * @param[in]  slurm_jobid  Null-terminated Slurm job ID string to cancel.
 * @param[out] err_msg      Writable buffer of size PRTE_SLURM_ERR_STR_MAX_SIZE
 *                          for Slurm output on failure, or NULL if not required.
 */
static int prte_ras_slurm_kill_job(const char *slurm_jobid, char *err_msg) {

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

    if(NULL != err_msg) {
        char *buf = fgets(err_msg, PRTE_SLURM_ERR_STR_MAX_SIZE, fp);

        /* Copy output into provided memory, truncating if necessary */
        if(NULL != buf) {
            size_t len = strcspn(buf, "\n");

            if (buf[len] == '\n') {
                buf[len] = '\0';
            } else if (len == PRTE_SLURM_ERR_STR_MAX_SIZE - 1) {
                memcpy(buf + PRTE_SLURM_ERR_STR_MAX_SIZE - 4, "...", 3);
            }
        } 
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