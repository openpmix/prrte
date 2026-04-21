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

#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

#include "src/util/pmix_output.h"

#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_slurm.h"
#include "src/mca/ras/base/base.h"

#define PRTE_SLURM_JOB_ID_MAX_LEN 20
#define PRTE_SLURM_ERR_STR_MAX_SIZE 256
#define PRTE_SLURM_MAX_SBATCH_ARGS 32

/*
 * Local functions
 */
static int prte_ras_slurm_make_sbatch_arg(pmix_hash_table_t *fields, const char *field_name, const char *field_format, bool obj_num, int *argc, char **argv);
static int prte_ras_slurm_token_has_control_chars(const char *s, size_t len, bool *has_control_chars);
static int prte_ras_slurm_convert_jobid(const char *slurm_jobid, uint32_t *slurm_jobid_numeric);
static int prte_ras_slurm_exec_sbatch(char * const *argv, char *job_id);
static int prte_ras_slurm_validate_jobid(const char *slurm_jobid);
static int prte_ras_slurm_kill_job(const char *slurm_jobid, char *err_msg);

/* String fields to read from "parent" Slurm job JSON */
const char *const str_fields[STR_FIELD_COUNT] = {
    [STR_ACCOUNT]   = "account",
    [STR_PARTITION] = "partition",
    [STR_QOS]       = "qos",
    [STR_CWD]       = "current_working_directory",
};

/* Numberic object fields to read from "parent" Slurm job JSON */
const char *const num_obj_fields[NUM_OBJ_FIELD_COUNT] = {
    [NUM_OBJ_MEMORY_PER_CPU]   = "memory_per_cpu",
    [NUM_OBJ_MEMORY_PER_NODE]  = "memory_per_node",
    [NUM_OBJ_TIME_LIMIT]       = "time_limit",
    [NUM_OBJ_THREADS_PER_CORE] = "threads_per_core",
};

/* Fields to expect inside given Slurm numeric objects */
const char *const num_obj_subfields[NUM_OBJ_SUBFIELD_COUNT] = {
    [NUM_OBJ_SUBFIELD_SET]      = "set",
    [NUM_OBJ_SUBFIELD_INFINITE] = "infinite",
    [NUM_OBJ_SUBFIELD_NUMBER]   = "number",
};

/* Fields for internal PRRTE record keeping */
const char *const record_job_data_fields[PRTE_JOB_DATA_COUNT] = {
    [PRTE_JOB_DATA_NODES]  = "nodes",
    [PRTE_JOB_DATA_JOB_ID] = "job_id",
};

/* Number of fields to expect inside job record hash table */
const size_t total_fields_len =
    STR_FIELD_COUNT + NUM_OBJ_FIELD_COUNT + PRTE_JOB_DATA_COUNT;

/* Slurm sbatch parameters formats */
static const char *account_format   = "--account=%s";
static const char *partition_format = "--partition=%s";
static const char *qos_format       = "--qos=%s";
static const char *cwd_format       = "--chdir=%s";
static const char *mem_per_cpu_format  = "--mem-per-cpu=%s";
static const char *mem_per_node_format = "--mem=%s";
static const char *time_format = "--time=%s";
static const char *nodes_format = "--nodes=%s";
static const char *threads_per_core_format = "--threads-per-core=%s";

/*
 * Append a formatted sbatch argument from a pmix hash table field.
 *
 * Looks up a value in the provided hash table and, if present and usable,
 * formats it according to the given format string and appends it to the
 * sbatch argv array.
 *
 * @param[in] fields
 *     Hash table containing job configuration data.
 * @param[in] field_name
 *     Key used to retrieve the value from the hash table.
 * @param[in] field_format
 *     printf-style format string used to construct the sbatch argument.
 * @param[in] obj_num
 *     Indicates whether the field represents a numeric object; enables
 *     filtering of special sentinel values (e.g., "unset", "infinite").
 * @param[in,out] argc
 *     Current argument count. Incremented if an argument is appended.
 * @param[in,out] argv
 *     Argument vector to append to (size PRTE_SLURM_MAX_SBATCH_ARGS+1).
 */
static int prte_ras_slurm_make_sbatch_arg(pmix_hash_table_t *fields,
                                          const char *field_name,
                                          const char *field_format,
                                          bool obj_num,
                                          int *argc,
                                          char **argv
                                          )
{    
    if(NULL == fields || NULL == field_name || NULL == field_format 
    || NULL == argv || NULL == argc || *argc < 0) {
        return PRTE_ERR_BAD_PARAM;
    }

    if(*argc >= PRTE_SLURM_MAX_SBATCH_ARGS) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    char *stored_val = NULL;

    int pmix_err = pmix_hash_table_get_value_ptr(fields, field_name,
                        strlen(field_name), (void**)&stored_val);

    if(PMIX_SUCCESS != pmix_err) {
        /* converts PMIX_ERR_NOT_FOUND->PRTE_ERR_NOT_FOUND if not found */
        return prte_pmix_convert_status(pmix_err);
    }

    if(NULL == stored_val || '\0' == stored_val[0]) {
        return PRTE_ERR_DATA_VALUE_NOT_FOUND;
    }

    if(obj_num) {
        /* handle both as just unset for now */
        if(0 == strcmp(stored_val, PRTE_SLURM_UNSET_NUM_MARKER)
        || 0 == strcmp(stored_val, PRTE_SLURM_INFINITE_NUM_MARKER)) {
            return PRTE_ERR_NOT_FOUND;
        }
    }

    int rc = asprintf(&argv[*argc], field_format, stored_val);

    if(0 > rc) {
        argv[*argc] = NULL;
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    (*argc)++;
    argv[*argc] = NULL;

    return PRTE_SUCCESS;
}

/*
 * Check whether a string contains control characters
 *
 * Rejects the string if it contains any control characters
 */
static int prte_ras_slurm_token_has_control_chars(const char *s, size_t len, bool *has_control_chars)
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
 * Run sbatch and capture the submitted Slurm job ID.
 *
 * Executes the command specified by argv in a child process, captures the
 * child's standard output through a pipe, and extracts the leading decimal job
 * ID from that output. The child is then waited on and the result is validated.
 *
 * The function expects output compatible with Slurm's --parsable mode, such
 * as "12345" or "12345;cluster". Only the leading numeric job ID is
 * stored in job_id.
 *
 * @param[in] argv NULL-terminated argument vector for execvp().
 * @param[out] job_id Buffer of size PRTE_SLURM_JOB_ID_MAX_LEN+1 that receives
 *                    the null-terminated numeric job ID on success.
 */
static int prte_ras_slurm_exec_sbatch(char * const *argv, char *job_id) {
    
    if(NULL == argv || NULL == argv[0] || NULL == job_id) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;

    job_id[0] = '\0';

    int status;

    size_t n = 0;

    bool overflow = false;

    bool pipe_draining = false;
    bool pipe_drained = false;

    pid_t pid;

    int pipefd[2] = {-1, -1};
    int pipe_err = pipe(pipefd);

    if(pipe_err < 0) {
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        char *strerr = strerror(errno);
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:exec_sbatch: pipe failed: %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
        goto cleanup;   
    }

    pid = fork();

    if(pid < 0) {
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        char *strerr = strerror(errno);
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:exec_sbatch: fork failed: %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
        goto cleanup;
    }

    if (pid == 0) {

        /* Child writes; close read end */
        close(pipefd[0]);
        pipefd[0] = -1;

        /* Redirect output to the pipe */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }

        /* No longer needed */
        close(pipefd[1]);
        pipefd[1] = -1;

        execvp(argv[0], argv);
        
        /* Something went wrong if we reached this point */
        _exit(127);
    }

    /* Parent reads; close write end */
    close(pipefd[1]);
    pipefd[1] = -1;

    /* Try to get job ID from pipe and drain it after */
    while(!pipe_drained) {
        char c;
        ssize_t r = read(pipefd[0], &c, 1);

        if(1 == r && !pipe_draining)
        {
            /* Slurm job ID, exclusively from digits 0-9 */
            if((n < PRTE_SLURM_JOB_ID_MAX_LEN) 
                && ('0' <= c && c <= '9')) {
                job_id[n++] = c;
            }

            /* Saw more digits, but had no space for them */
            else if('0' <= c && c <= '9') {
                overflow = true;
                pipe_draining = true;
            }

            /* Ignore initial whitespace */
            else if(!(0 == n && isspace((unsigned char)c))) {
                pipe_draining = true;
            }
        }
        
        /* Nothing more to read */
        else if(0 == r) {
            pipe_drained = true;
        }

        /* Tolerate interruptions */
        else if (r < 0 && errno == EINTR) {
            continue;
        } 

        /* Something went wrong */
        else {
            char *strerr = strerror(errno);
            err = PRTE_ERR_PIPE_READ_FAILURE;
            PRTE_ERROR_LOG(err);
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:exec_sbatch: pipe read failed: %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
            break; /* Continue execution to wait for child */
        }
    }

    close(pipefd[0]);
    pipefd[0] = -1;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue; 
        }

        char *strerr = strerror(errno);
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
        "%s ras:slurm:exec_sbatch: waitpid failed: %s",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
        goto cleanup;
    }

    /* Pipe read failed earlier */
    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
        "%s ras:slurm:exec_sbatch: sbatch failed or exited non-zero",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        err = PRTE_ERR_SLURM_SUBMIT_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if(n == 0 || overflow) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
        "%s ras:slurm:exec_sbatch: sbatch exited normally, but got unexpected/truncated output",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        err = PRTE_ERR_SLURM_SUBMIT_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    job_id[n] = '\0';

    cleanup:

    if(pipefd[0] >= 0) {
        close(pipefd[0]);
    }

    if(pipefd[1] >= 0) {
        close(pipefd[1]);
    }

    return err;
}

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

/*
 * Construct and launch a Slurm "expander" job via sbatch.
 *
 * Constructs an sbatch command using parameters stored in the provided
 * hash table. Fields read from the original Slurm job are optionally
 * propagated depending on MCA component configuration.
 *
 * On success, the resulting SLURM job ID is stored back into the hash table
 * under PRTE_JOB_DATA_JOB_ID.
 *
 * @param[in,out] fields
 *     Hash table containing job configuration inputs and receiving the
 *     resulting job ID on success.
 */
int prte_ras_slurm_launch_expander_job(pmix_hash_table_t *fields)
{
    if(NULL == fields) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;
    int pmix_err = PMIX_SUCCESS;

    char *argv[PRTE_SLURM_MAX_SBATCH_ARGS+1] = {NULL};
    int argc = 0;

    bool have_mem_per_cpu = false;

    char job_id[PRTE_SLURM_JOB_ID_MAX_LEN+1] = {0};
    char *job_id_dyn = NULL;

    const char * const initial_args[] = {"sbatch",
                                "--wrap=sleep infinity", 
                                "--parsable",
                                "--exclusive",
                                NULL };

    for (int i = 0; initial_args[i] != NULL; i++) {
        if (argc >= PRTE_SLURM_MAX_SBATCH_ARGS ||
            NULL == (argv[argc] = strdup(initial_args[i]))) {
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
        argc++;
    }
    
    err = prte_ras_slurm_make_sbatch_arg(fields, record_job_data_fields[PRTE_JOB_DATA_NODES], nodes_format, false, &argc, argv);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (prte_mca_ras_slurm_component.propagate_account) {
        err = prte_ras_slurm_make_sbatch_arg(fields, str_fields[STR_ACCOUNT], account_format, false, &argc, argv);

        /* Tolerate not found errors */
        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if (prte_mca_ras_slurm_component.propagate_partition) {
        err = prte_ras_slurm_make_sbatch_arg(fields, str_fields[STR_PARTITION], partition_format, false, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if (prte_mca_ras_slurm_component.propagate_qos) {
        err = prte_ras_slurm_make_sbatch_arg(fields, str_fields[STR_QOS], qos_format, false, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

    }

    if (prte_mca_ras_slurm_component.propagate_cwd) {
        err = prte_ras_slurm_make_sbatch_arg(fields, str_fields[STR_CWD], cwd_format, false, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if(prte_mca_ras_slurm_component.propagate_mem_per_cpu) {
        err = prte_ras_slurm_make_sbatch_arg(fields, num_obj_fields[NUM_OBJ_MEMORY_PER_CPU], 
                                            mem_per_cpu_format, true, &argc, argv);

        if(PRTE_SUCCESS == err) {
            have_mem_per_cpu = true;
        }
        else if(PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    /* Mem per node; only if mem per CPU not already set */
    if(!have_mem_per_cpu && prte_mca_ras_slurm_component.propagate_mem_per_node) {
        err = prte_ras_slurm_make_sbatch_arg(fields, num_obj_fields[NUM_OBJ_MEMORY_PER_NODE], 
                                            mem_per_node_format, true, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if(prte_mca_ras_slurm_component.propagate_time) {

        err = prte_ras_slurm_make_sbatch_arg(fields, num_obj_fields[NUM_OBJ_TIME_LIMIT], 
                                            time_format, true, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if(prte_mca_ras_slurm_component.propagate_threads_per_core) {

        err = prte_ras_slurm_make_sbatch_arg(fields, num_obj_fields[NUM_OBJ_THREADS_PER_CORE], 
                                            threads_per_core_format, true, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    err = prte_ras_slurm_exec_sbatch(argv, job_id);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    PMIX_OUTPUT_VERBOSE((10, prte_ras_base_framework.framework_output,
                "%s ras:slurm:launch_expander_job: got job ID %s",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), job_id));

    job_id_dyn = strdup(job_id);

    if(NULL == job_id_dyn) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }

    pmix_err = pmix_hash_table_set_value_ptr(fields, record_job_data_fields[PRTE_JOB_DATA_JOB_ID],
                        strlen(record_job_data_fields[PRTE_JOB_DATA_JOB_ID]), (void*)job_id_dyn);

    if(PMIX_SUCCESS != pmix_err) {
        err = prte_pmix_convert_status(pmix_err);
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    /* Now owned by the table */
    job_id_dyn = NULL;

    cleanup:

    if(PRTE_SUCCESS != err && job_id[0] != '\0') {
        /* Prevent hanging resources if failed */
        prte_ras_slurm_kill_job(job_id, NULL);
    }

    if(NULL != job_id_dyn) {
        free(job_id_dyn);
    }

    for(int i = 0; i<PRTE_SLURM_MAX_SBATCH_ARGS+1 && NULL != argv[i]; i++) {
        free(argv[i]);
    }

    return err;
}

/*
 * Create and register a PRRTE session from a node list belonging to a Slurm allocation.
 *
 * Creates a new prte_session_t using slurm_jobid as the session ID,
 * associates the nodes in node_list with the session, and adds it to
 * the global session table.
 *
 * @param[in] slurm_jobid  Slurm job ID string (must be convertible to uint32_t)
 * @param[in] alloc_refid  Optional allocation reference ID (may be NULL)
 * @param[in] node_list    List of prte_node_t to attach to the session
 */
int prte_ras_slurm_assign_new_session(const char *slurm_jobid, const char *alloc_refid, pmix_list_t *node_list) {
    
    if(NULL == slurm_jobid || NULL == node_list) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }
    
    prte_session_t *session = NULL;

    uint32_t slurm_id_uint;
    
    err = prte_ras_slurm_convert_jobid(slurm_jobid, &slurm_id_uint);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    char *alloc_refid_dup = NULL;

    if(NULL != alloc_refid) {
        alloc_refid_dup = strdup(alloc_refid);
        if(NULL == alloc_refid_dup) {
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            return err;
        }
    }

    session = PMIX_NEW(prte_session_t);

    if (NULL == session) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    session->session_id = slurm_id_uint;

    if(NULL != alloc_refid_dup) {
        session->alloc_refid = alloc_refid_dup;
        /* Now owned by the session */
        alloc_refid_dup = NULL;
    }

    prte_node_t *node = NULL;

    PMIX_LIST_FOREACH(node, node_list, prte_node_t) {

        /* Tag the nodes with the session ID */
        err = prte_set_attribute(&node->attributes, PRTE_NODE_MODIFY_ID, PRTE_ATTR_LOCAL,
            &slurm_id_uint, PMIX_UINT32);

        if(PRTE_SUCCESS != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        PMIX_RETAIN(node);

        int idx = pmix_pointer_array_add(session->nodes, node);
        if (0 > idx) {
            /* Negative returned idx indicates PMIX error */
            err = prte_pmix_convert_status(idx);
            PMIX_RELEASE(node);
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    err = prte_set_session_object(session);
    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    cleanup:

    free(alloc_refid_dup);

    if(NULL != session && err != PRTE_SUCCESS) {
        PMIX_RELEASE(session);
    }

    return err;
}

/**
 * Reject nodes that already exist in the global node pool.
 *
 * Iterates over the provided node list and checks whether any node
 * already exists in prte_node_pool using prte_node_match().
 * If a match is found, the function returns PRTE_EXISTS.
 *
 * @param[in] node_list  List of prte_node_t to validate
 */
int prte_ras_slurm_reject_node_duplicates(pmix_list_t *node_list) {

    if (NULL == node_list) {
        return PRTE_ERR_BAD_PARAM;
    }

    prte_node_t *node, *existing;

    PMIX_LIST_FOREACH(node, node_list, prte_node_t) {
        existing = prte_node_match(NULL, node->name);
        if (NULL != existing) {
            return PRTE_EXISTS;
        }
    }
    return PRTE_SUCCESS;
}

#ifndef HAVE_JANSSON

/*
 * Extract SLURM job fields; returns PRTE_ERR_NOT_SUPPORTED if built without Jansson.
 */
int prte_ras_slurm_extract_job_fields(pmix_hash_table_t *values_table)
{
    PRTE_HIDE_UNUSED_PARAMS(values_table);
    pmix_output(0, "ras:slurm:extract_job_fields: "
                "Jansson support is not enabled in this build");

    return PRTE_ERR_NOT_SUPPORTED;
}

/**
 * Add new SLURM job resources; returns PRTE_ERR_NOT_SUPPORTED if built without Jansson.
 */
int prte_ras_slurm_add_modified_resources(const char *slurm_jobid,
                                                 pmix_list_t *node_list)
{
    PRTE_HIDE_UNUSED_PARAMS(slurm_jobid, node_list);

    pmix_output(0, "ras:slurm:add_modified_resources: "
                "Jansson support is not enabled in this build");
    return PRTE_ERR_NOT_SUPPORTED;
}

/**
 * Wait for SLURM job resources; returns PRTE_ERR_NOT_SUPPORTED if built without Jansson.
 */
int prte_ras_slurm_wait_resources(const char *slurm_jobid)
{
    PRTE_HIDE_UNUSED_PARAMS(slurm_jobid);
    pmix_output(0, "ras:slurm:wait_resources: "
                "Jansson support is not enabled in this build");
    return PRTE_ERR_NOT_SUPPORTED;
}

#endif
