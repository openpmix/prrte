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
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include "src/util/pmix_output.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"

#include "ras_slurm.h"
#include "src/mca/ras/base/base.h"

#define PRTE_SLURM_MAX_SALLOC_ARGS 32

/* Retry budget for a job whose salloc has exited but whose state slurmctld has
 * not caught up with. Doubles from 1ms to a 1s cap, ~3s in total. */
#define PRTE_SLURM_GRANT_RETRY_MIN_USEC 1000       /* 1 ms */
#define PRTE_SLURM_GRANT_RETRY_MAX_USEC 1000000    /* 1 sec */
#define PRTE_SLURM_GRANT_RETRY_MAX 12

/* The token salloc puts in front of the job ID on both of the lines that can
 * carry it: "salloc: Pending job allocation <id>" when the request queues, and
 * "salloc: Granted job allocation <id>" when it does not. */
#define PRTE_SLURM_ALLOC_ID_MARKER "allocation "

/* Longest single line of salloc output the job-ID scan will consider. Its
 * messages are far shorter; a longer line simply cannot be the one we want. */
#define PRTE_SLURM_ALLOC_LINE_MAX 512

/* Bound on what the reap callback will drain out of a finished salloc for the
 * diagnostic message. */
#define PRTE_SLURM_ALLOC_TAIL_MAX 1024

/* Struct for callback after pending job wait is complete */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_pmix_server_req_t *req;
    char *request_id;
    bool user_request_id_provided;
    char *job_id;
    int err;
    uint64_t retry_delay_usec;
    int attempts;
    int tracker_index;
    /* Answered; the answer is posted but not yet delivered */
    bool completing;
} prte_slurm_wait_tracker_t;

/* Everything the reap callback needs, plus the index of the extend this child
 * belongs to. An index and not a reference, because either object can outlive
 * the other: a stale index reads back NULL, and the job ID catches a slot the
 * array has since reused. */
typedef struct {
    pmix_list_item_t super;
    pid_t pid;
    int outfd;
    char *job_id;
    int tracker_index;
} prte_slurm_salloc_child_t;

/* Bound on how long finalize waits for a killed salloc to become reapable */
#define PRTE_SLURM_REAP_WAIT_USEC 1000000    /* 1 sec */
#define PRTE_SLURM_REAP_POLL_USEC 10000      /* 10 ms */

/* Extends still in flight, for finalize to clean up */
static pmix_list_t prte_slurm_salloc_children;
static pmix_pointer_array_t prte_slurm_wait_trackers;
static bool extend_initialized = false;

/*
 * Local functions
 */
static void swt_con(prte_slurm_wait_tracker_t *p);
static void swt_des(prte_slurm_wait_tracker_t *p);
static void ssc_con(prte_slurm_salloc_child_t *p);
static void ssc_des(prte_slurm_salloc_child_t *p);
static void salloc_wait_cb(int fd, short args, void *cbdata);
static int prte_ras_slurm_make_salloc_arg(pmix_hash_table_t *fields, const char *field_name, const char *field_format, bool obj_num, int *argc, char **argv);
static int prte_ras_slurm_exec_salloc(char * const *argv, char *job_id);
static int prte_ras_slurm_launch_expander_job(pmix_hash_table_t *fields);
static int prte_ras_slurm_reject_node_duplicates(pmix_list_t *node_list);
static int prte_ras_slurm_extract_reused_nodes(const char *slurm_jobid,
                                               pmix_list_t *node_list,
                                               pmix_pointer_array_t *reused_nodes);
static int prte_ras_slurm_add_reused_nodes_to_session(const char *slurm_jobid,
                                                      pmix_pointer_array_t *reused_nodes);
static void prte_ras_slurm_rollback_session(const char *slurm_jobid);
static void prte_ras_slurm_extend_wait_complete(int fd, short args, void *cbdata);
static void slurm_grant_check_cb(int fd, short args, void *cbdata);
static prte_slurm_wait_tracker_t *prte_ras_slurm_tracker_for_child(const prte_slurm_salloc_child_t *child);

PMIX_CLASS_INSTANCE(prte_slurm_wait_tracker_t, pmix_object_t, swt_con, swt_des);
PMIX_CLASS_INSTANCE(prte_slurm_salloc_child_t, pmix_list_item_t, ssc_con, ssc_des);

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

/* Slurm salloc parameters formats */
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
 * Constructor for prte_slurm_wait_tracker_t
 */
static void swt_con(prte_slurm_wait_tracker_t *p)
{
    p->req = NULL;
    p->request_id = NULL;
    p->user_request_id_provided = false;
    p->job_id = NULL;
    p->err = PRTE_SUCCESS;
    p->retry_delay_usec = PRTE_SLURM_GRANT_RETRY_MIN_USEC;
    p->attempts = 0;
    p->tracker_index = -1;
    p->completing = false;
}

/*
 * Destructor for prte_slurm_wait_tracker_t
 */
static void swt_des(prte_slurm_wait_tracker_t *p)
{
    if (NULL != p->job_id) {
        free(p->job_id);
    }

    if (NULL != p->request_id) {
        free(p->request_id);
    }

    if (NULL != p->req) {
        PMIX_RELEASE(p->req);
    }
}

/*
 * Constructor for prte_slurm_salloc_child_t
 */
static void ssc_con(prte_slurm_salloc_child_t *p)
{
    p->pid = -1;
    p->outfd = -1;
    p->job_id = NULL;
    p->tracker_index = -1;
}

/*
 * Destructor for prte_slurm_salloc_child_t
 */
static void ssc_des(prte_slurm_salloc_child_t *p)
{
    if (0 <= p->outfd) {
        close(p->outfd);
    }

    if (NULL != p->job_id) {
        free(p->job_id);
    }
}

/*
 * Set up the records of work in flight.
 */
int prte_ras_slurm_modify_extend_init(void)
{
    if (extend_initialized || !prte_ras_slurm_have_extensions()) {
        return PRTE_SUCCESS;
    }

    int err;

    PMIX_CONSTRUCT(&prte_slurm_salloc_children, pmix_list_t);
    PMIX_CONSTRUCT(&prte_slurm_wait_trackers, pmix_pointer_array_t);

    err = pmix_pointer_array_init(&prte_slurm_wait_trackers, 0, INT_MAX, 16);

    if (PMIX_SUCCESS != err) {
        PMIX_DESTRUCT(&prte_slurm_wait_trackers);
        PMIX_DESTRUCT(&prte_slurm_salloc_children);
        return prte_pmix_convert_status(err);
    }

    extend_initialized = true;

    return PRTE_SUCCESS;
}

/*
 * Find the extend a finished salloc child belongs to, or NULL if it has none.
 */
static prte_slurm_wait_tracker_t *prte_ras_slurm_tracker_for_child(const prte_slurm_salloc_child_t *child)
{
    prte_slurm_wait_tracker_t *trk;

    if (0 > child->tracker_index || NULL == child->job_id) {
        return NULL;
    }

    trk = (prte_slurm_wait_tracker_t *)
              pmix_pointer_array_get_item(&prte_slurm_wait_trackers, child->tracker_index);

    if (NULL == trk || NULL == trk->job_id || 0 != strcmp(trk->job_id, child->job_id)) {
        return NULL;
    }

    if (trk->completing) {
        return NULL;
    }

    return trk;
}

/*
 * Point the salloc child that created a job at the extend waiting on it. Not
 * done in exec_salloc: the tracker needs the job ID that call goes to get.
 */
static void prte_ras_slurm_link_child_to_tracker(const char *job_id, int tracker_index)
{
    prte_slurm_salloc_child_t *child;

    PMIX_LIST_FOREACH(child, &prte_slurm_salloc_children, prte_slurm_salloc_child_t)
    {
        if (NULL != child->job_id && 0 == strcmp(child->job_id, job_id)) {
            child->tracker_index = tracker_index;
            return;
        }
    }
}

/*
 * Clean up any extend still in flight.
 *
 * Only the trackers and the processes: the allocations behind them belong to
 * the cancel half, which prte_ras_slurm_finalize runs first. Done inline
 * because the event base has stopped - neither a retry timer nor the SIGCHLD
 * that reaps a child will fire again.
 */
int prte_ras_slurm_modify_extend_finalize(void)
{
    if (!extend_initialized) {
        return PRTE_SUCCESS;
    }

    prte_slurm_salloc_child_t *child;

    for (int i = 0; i < prte_slurm_wait_trackers.size; i++) {
        prte_slurm_wait_tracker_t *trk = (prte_slurm_wait_tracker_t *)
            pmix_pointer_array_get_item(&prte_slurm_wait_trackers, i);

        if (NULL == trk) {
            continue;
        }

        /* The requester is not told, because the only caller runs after
         * pmix_server_finalize(): the request has no other reference left and
         * nothing to answer through. Finalizing this component with the DVM
         * still up would have to complete these instead of dropping them.
         * The timer only runs between retries, but serve_extend_req always sets
         * it, so deleting it is safe either way. */
        prte_event_evtimer_del(&trk->ev);
        pmix_pointer_array_set_item(&prte_slurm_wait_trackers, i, NULL);
        PMIX_RELEASE(trk);
    }

    while (NULL != (child = (prte_slurm_salloc_child_t *)
                                pmix_list_remove_first(&prte_slurm_salloc_children))) {
        /* No SIGTERM first: nothing is left to ask for. Its allocation is
         * somebody else's to give back - the cancel half, or the session
         * drain - and a submission that failed was already signalled by
         * exec_salloc. salloc does not reliably act on one anyway. */
        kill(child->pid, SIGKILL);

        /* Reap it. PID 1 is not always something that reaps, and an orphan
         * nobody waits on is a zombie for the life of the machine. */
        for (uint64_t waited = 0; PRTE_SLURM_REAP_WAIT_USEC > waited;
             waited += PRTE_SLURM_REAP_POLL_USEC) {
            struct timespec tp = {0, PRTE_SLURM_REAP_POLL_USEC * 1000};
            pid_t reaped = waitpid(child->pid, NULL, WNOHANG);

            if (0 > reaped && EINTR == errno) {
                continue;
            }

            /* Reaped, or never ours to wait for */
            if (0 != reaped) {
                break;
            }

            nanosleep(&tp, NULL);
        }

        /* This child's prte_wait_tracker_t is left holding a dangling cbdata,
         * which is what salloc_wait_cb's finalized check is for. */
        PMIX_RELEASE(child);
    }

    PMIX_DESTRUCT(&prte_slurm_wait_trackers);
    PMIX_DESTRUCT(&prte_slurm_salloc_children);
    extend_initialized = false;

    return PRTE_SUCCESS;
}

/*
 * Append a formatted salloc argument from a pmix hash table field.
 *
 * Looks up a value in the provided hash table and, if present and usable,
 * formats it according to the given format string and appends it to the
 * salloc argv array. Missing and empty values return PRTE_ERR_NOT_FOUND so
 * callers can omit optional Slurm attributes.
 *
 * @param[in] fields
 *     Hash table containing job configuration data.
 * @param[in] field_name
 *     Key used to retrieve the value from the hash table.
 * @param[in] field_format
 *     printf-style format string used to construct the salloc argument.
 * @param[in] obj_num
 *     Indicates whether the field represents a numeric object; enables
 *     filtering of special sentinel values (e.g., "unset", "infinite").
 * @param[in,out] argc
 *     Current argument count. Incremented if an argument is appended.
 * @param[in,out] argv
 *     Argument vector to append to (size PRTE_SLURM_MAX_SALLOC_ARGS+1).
 */
static int prte_ras_slurm_make_salloc_arg(pmix_hash_table_t *fields,
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

    if(*argc >= PRTE_SLURM_MAX_SALLOC_ARGS) {
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
        return PRTE_ERR_NOT_FOUND;
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
 * Reap a salloc child, and check on the extend it belongs to.
 *
 * "salloc --no-shell" exits when the job leaves PENDING, which is what the
 * extend is waiting for. The exit status is diagnostic only - it does not say
 * whether the job was granted or cancelled, so the check asks Slurm.
 *
 * @param[in] cbdata The prte_wait_tracker_t, NOT the data handed to
 *                   prte_wait_cb - that is at its cbdata member.
 */
static void salloc_wait_cb(int fd, short args, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    prte_slurm_salloc_child_t *child;
    prte_slurm_wait_tracker_t *trk;
    char tail[PRTE_SLURM_ALLOC_TAIL_MAX + 1];
    size_t n = 0;
    int status;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    /* Past finalize, cbdata names a child this component has already killed,
     * reaped and freed. Reaching here would mean the event base was dispatched
     * after the module closed, which does not happen - but the alternative to
     * checking is a use-after-free on a shutdown path. */
    if (!extend_initialized) {
        PMIX_RELEASE(t2);
        return;
    }

    child = (prte_slurm_salloc_child_t *) t2->cbdata;

    /* Collect whatever salloc had left to say. The child is gone and we hold
     * the only remaining descriptor, so this reads to EOF without blocking -
     * and the read end can finally be closed. Closing it any earlier would
     * have risked handing salloc an EPIPE part way through the handshake that
     * secures the allocation. */
    while (PRTE_SLURM_ALLOC_TAIL_MAX > n) {
        ssize_t r = read(child->outfd, tail + n, PRTE_SLURM_ALLOC_TAIL_MAX - n);

        if (0 > r && EINTR == errno) {
            continue;
        }

        if (0 >= r) {
            break;
        }

        n += (size_t) r;
    }

    tail[n] = '\0';
    close(child->outfd);
    child->outfd = -1;

    /* prte_wait.c records the raw waitpid status here */
    status = t2->child->exit_code;

    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                             "%s ras:slurm:salloc_wait: salloc (pid %lu) for job %s "
                             "exited with status %d: %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned long) child->pid,
                             NULL == child->job_id ? "<unknown>" : child->job_id,
                             status, tail));
    } else {
        PMIX_OUTPUT_VERBOSE((10, prte_ras_base_framework.framework_output,
                             "%s ras:slurm:salloc_wait: salloc (pid %lu) for job %s "
                             "has handed off its allocation",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned long) child->pid,
                             NULL == child->job_id ? "<unknown>" : child->job_id));
    }

    /* Read before releasing the child */
    trk = prte_ras_slurm_tracker_for_child(child);

    pmix_list_remove_item(&prte_slurm_salloc_children, &child->super);
    PMIX_RELEASE(child);

    /* The tracker owns the dummy proc; nothing owns cbdata but us */
    PMIX_RELEASE(t2);

    /* No tracker: the extend was already answered, or never got one */
    if (NULL != trk) {
        slurm_grant_check_cb(-1, 0, trk);
    }
}

/*
 * Extract the Slurm job ID from one line of salloc output.
 *
 * @param[in] line One line of salloc output, null terminated.
 * @param[out] job_id Buffer of size PRTE_SLURM_JOB_ID_MAX_LEN+1 that receives
 *                    the null-terminated numeric job ID on a match.
 */
static bool prte_ras_slurm_line_job_id(const char *line, char *job_id)
{
    const char *p = strstr(line, PRTE_SLURM_ALLOC_ID_MARKER);
    /* Assembled here and only copied out on a match: a caller that is told
     * "no job on this line" must be left with the buffer it had, or a
     * rejected run of digits becomes a job ID somebody later scancels. */
    char scratch[PRTE_SLURM_JOB_ID_MAX_LEN + 1];
    size_t n = 0;

    if (NULL == p) {
        return false;
    }

    p += strlen(PRTE_SLURM_ALLOC_ID_MARKER);

    while (isdigit((unsigned char) *p)) {
        /* Too long to be a job ID, so this was not one */
        if (PRTE_SLURM_JOB_ID_MAX_LEN <= n) {
            return false;
        }
        scratch[n++] = *p++;
    }

    if (0 == n) {
        return false;
    }

    scratch[n] = '\0';
    memcpy(job_id, scratch, n + 1);

    return true;
}

/*
 * Run salloc and capture the Slurm job ID it announces.
 *
 * Executes the command specified by argv in a child process and reads that
 * child's output until it names the job it created. salloc announces the job
 * as soon as the request reaches slurmctld, whether or not it can be satisfied
 * immediately: "salloc: Pending job allocation <id>" when the request queues,
 * "salloc: Granted job allocation <id>" when it does not.
 *
 * The child is NOT waited on here. Under "--no-shell" salloc is the process
 * holding the handshake with Slurm, and it does not exit until the allocation
 * is granted; killing it while the job is pending revokes the allocation. So
 * it is left running and handed to the SIGCHLD machinery, which reaps it
 * whenever it finishes (see salloc_wait_cb). The caller gets the job ID
 * immediately and can proceed to poll Slurm for the allocation.
 *
 * @param[in] argv NULL-terminated argument vector for execvp().
 * @param[out] job_id Buffer of size PRTE_SLURM_JOB_ID_MAX_LEN+1 that receives
 *                    the null-terminated numeric job ID on success.
 */
static int prte_ras_slurm_exec_salloc(char * const *argv, char *job_id)
{
    if(NULL == argv || NULL == argv[0] || NULL == job_id) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;

    job_id[0] = '\0';

    char line[PRTE_SLURM_ALLOC_LINE_MAX + 1];
    size_t n = 0;

    bool found = false;
    bool child_adopted = false;

    pid_t pid;

    prte_proc_t *dummy = NULL;
    prte_slurm_salloc_child_t *child = NULL;

    int pipefd[2] = {-1, -1};

    /* Everything the reap needs is allocated up front: once the child exists
     * there must be no failure path that cannot hand it over. */
    child = PMIX_NEW(prte_slurm_salloc_child_t);

    if(NULL == child) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    dummy = PMIX_NEW(prte_proc_t);

    if(NULL == dummy) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    int pipe_err = pipe(pipefd);

    if(pipe_err < 0) {
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        char *strerr = strerror(errno);
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:exec_salloc: pipe failed: %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
        goto cleanup;
    }

    pid = fork();

    if(pid < 0) {
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        char *strerr = strerror(errno);
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:exec_salloc: fork failed: %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
        goto cleanup;
    }

    if (pid == 0) {
        int devnull;

        /* Child writes; close read end */
        close(pipefd[0]);
        pipefd[0] = -1;

        /* salloc announces the job on stderr, so BOTH streams have to come
         * back to us */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }

        /* No longer needed */
        close(pipefd[1]);
        pipefd[1] = -1;

        devnull = open("/dev/null", O_RDONLY);

        if (0 <= devnull) {
            dup2(devnull, STDIN_FILENO);
            if (STDIN_FILENO != devnull) {
                close(devnull);
            }
        }

        /* This child holds a pending allocation on the DVM's behalf, possibly
         * for a long time. Take it out of the launching shell's process group
         * and session so that a ^C or a hangup there cannot cancel an
         * allocation the DVM is waiting on. */
        signal(SIGHUP, SIG_IGN);
        setpgid(0, 0);

        execvp(argv[0], argv);

        /* Something went wrong if we reached this point */
        _exit(127);
    }

    /* Parent reads; close write end */
    close(pipefd[1]);
    pipefd[1] = -1;

    /* Hand the child over before reading a byte of its output. PRRTE installs
     * a process-wide SIGCHLD handler that reaps every child it has, so this is
     * the only way to learn how salloc finished - and the child has to outlive
     * this call regardless, so there is nothing here to wait for. */
    child->pid = pid;
    child->outfd = pipefd[0];
    pipefd[0] = -1;
    child_adopted = true;

    /* The list owns our reference from here */
    pmix_list_append(&prte_slurm_salloc_children, &child->super);

    dummy->pid = pid;
    /* be sure to mark it as alive so we don't instantly fire */
    PRTE_FLAG_SET(dummy, PRTE_PROC_FLAG_ALIVE);
    prte_wait_cb(dummy, salloc_wait_cb, child);
    /* prte_wait_cb retains it; the tracker owns it from here */
    PMIX_RELEASE(dummy);
    dummy = NULL;

    /* Read salloc's output a line at a time until one names the job */
    while (!found) {
        char c;
        ssize_t r = read(child->outfd, &c, 1);

        /* Tolerate interruptions */
        if (0 > r && EINTR == errno) {
            continue;
        }

        if (0 > r) {
            char *strerr = strerror(errno);
            err = PRTE_ERR_PIPE_READ_FAILURE;
            PRTE_ERROR_LOG(err);
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:exec_salloc: pipe read failed: %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerr));
            break;
        }

        /* salloc is done talking. A final line without a newline - which is
         * what a failure diagnostic can arrive as - still counts. */
        if (0 == r) {
            line[n] = '\0';
            found = prte_ras_slurm_line_job_id(line, job_id);
            break;
        }

        if ('\n' != c) {
            /* Anything longer than this cannot be the line we want, so stop
             * storing it rather than growing the buffer */
            if (PRTE_SLURM_ALLOC_LINE_MAX > n) {
                line[n++] = c;
            }
            continue;
        }

        line[n] = '\0';
        n = 0;
        found = prte_ras_slurm_line_job_id(line, job_id);
    }

    if (!found) {
        if (PRTE_SUCCESS == err) {
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
            "%s ras:slurm:exec_salloc: salloc named no job before it stopped talking",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            err = PRTE_ERR_SLURM_SUBMIT_FAILURE;
            PRTE_ERROR_LOG(err);
        }
        goto cleanup;
    }

    /* Purely so the reap can name the job it belonged to */
    child->job_id = strdup(job_id);

    cleanup:

    /* A child that named no job may still be about to create one, and only it
     * can revoke a pending allocation, so ask it to. Its reap still runs and
     * still gives the pipe back. */
    if(PRTE_SUCCESS != err && child_adopted) {
        kill(child->pid, SIGTERM);
    }

    if(!child_adopted && NULL != child) {
        PMIX_RELEASE(child);
    }

    if(NULL != dummy) {
        PMIX_RELEASE(dummy);
    }

    if(pipefd[0] >= 0) {
        close(pipefd[0]);
    }

    if(pipefd[1] >= 0) {
        close(pipefd[1]);
    }

    return err;
}

/*
 * Construct and launch a Slurm "expander" job via salloc.
 *
 * Constructs a salloc command using parameters stored in the provided
 * hash table. Fields read from the original Slurm job are optionally
 * propagated depending on MCA component configuration.
 *
 * "--no-shell" is what makes the resulting allocation shrinkable. Allocating
 * with sbatch instead meant a batch script, and a batch script IS the job:
 * the job lives exactly as long as it runs, which is why that script had to
 * be "sleep infinity". It runs on the job's first node, so releasing that
 * node would have killed the script and taken the whole allocation with it -
 * the one node that could never be handed back on its own. "--no-shell" runs
 * nothing at all, so no node anchors the job and any of them can go.
 *
 * On success, the resulting SLURM job ID is stored back into the hash table
 * under PRTE_JOB_DATA_JOB_ID.
 *
 * @param[in,out] fields
 *     Hash table containing job configuration inputs and receiving the
 *     resulting job ID on success.
 */
static int prte_ras_slurm_launch_expander_job(pmix_hash_table_t *fields)
{
    if(NULL == fields) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int err = PRTE_SUCCESS;
    int pmix_err = PMIX_SUCCESS;

    char *argv[PRTE_SLURM_MAX_SALLOC_ARGS+1] = {NULL};
    int argc = 0;

    bool have_mem_per_cpu = false;

    char job_id[PRTE_SLURM_JOB_ID_MAX_LEN+1] = {0};
    char *job_id_dyn = NULL;

    const char * const initial_args[] = {"salloc",
                                "--no-shell",
                                "--exclusive",
                                NULL };

    for (int i = 0; initial_args[i] != NULL; i++) {
        if (argc >= PRTE_SLURM_MAX_SALLOC_ARGS ||
            NULL == (argv[argc] = strdup(initial_args[i]))) {
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
        argc++;
    }
    
    err = prte_ras_slurm_make_salloc_arg(fields, record_job_data_fields[PRTE_JOB_DATA_NODES], nodes_format, false, &argc, argv);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (prte_mca_ras_slurm_component.propagate_account) {
        err = prte_ras_slurm_make_salloc_arg(fields, str_fields[STR_ACCOUNT], account_format, false, &argc, argv);

        /* Tolerate not found errors */
        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if (prte_mca_ras_slurm_component.propagate_partition) {
        err = prte_ras_slurm_make_salloc_arg(fields, str_fields[STR_PARTITION], partition_format, false, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if (prte_mca_ras_slurm_component.propagate_qos) {
        err = prte_ras_slurm_make_salloc_arg(fields, str_fields[STR_QOS], qos_format, false, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

    }

    if (prte_mca_ras_slurm_component.propagate_cwd) {
        err = prte_ras_slurm_make_salloc_arg(fields, str_fields[STR_CWD], cwd_format, false, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if(prte_mca_ras_slurm_component.propagate_mem_per_cpu) {
        err = prte_ras_slurm_make_salloc_arg(fields, num_obj_fields[NUM_OBJ_MEMORY_PER_CPU], 
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
        err = prte_ras_slurm_make_salloc_arg(fields, num_obj_fields[NUM_OBJ_MEMORY_PER_NODE], 
                                            mem_per_node_format, true, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if(prte_mca_ras_slurm_component.propagate_time) {

        err = prte_ras_slurm_make_salloc_arg(fields, num_obj_fields[NUM_OBJ_TIME_LIMIT], 
                                            time_format, true, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    if(prte_mca_ras_slurm_component.propagate_threads_per_core) {

        err = prte_ras_slurm_make_salloc_arg(fields, num_obj_fields[NUM_OBJ_THREADS_PER_CORE], 
                                            threads_per_core_format, true, &argc, argv);

        if(PRTE_SUCCESS != err && PRTE_ERR_NOT_FOUND != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
    }

    err = prte_ras_slurm_exec_salloc(argv, job_id);

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
        prte_ras_slurm_kill_job(job_id, NULL, 0);
    }

    if(NULL != job_id_dyn) {
        free(job_id_dyn);
    }

    for(int i = 0; i<PRTE_SLURM_MAX_SALLOC_ARGS+1 && NULL != argv[i]; i++) {
        free(argv[i]);
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
static int prte_ras_slurm_reject_node_duplicates(pmix_list_t *node_list)
{
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

/**
 * @brief Move reusable node duplicates out of the new-node list.
 *
 * If Slurm regrants a node that PRRTE already knows about from an earlier
 * shrink, reuse the existing global node object instead of treating the
 * Slurm-discovered node as a duplicate. Only daemon-less nodes can be reused.
 *
 * @param[in] slurm_jobid    Slurm job ID for the new allocation.
 * @param[in,out] node_list  Newly discovered Slurm nodes.
 * @param[out] reused_nodes  Existing nodes to add back to the DVM.
 */
static int prte_ras_slurm_extract_reused_nodes(const char *slurm_jobid,
                                               pmix_list_t *node_list,
                                               pmix_pointer_array_t *reused_nodes)
{
    if (NULL == slurm_jobid || NULL == node_list || NULL == reused_nodes) {
        return PRTE_ERR_BAD_PARAM;
    }

    int err;
    uint32_t slurm_id_uint;
    prte_node_t *node, *next, *existing;

    err = prte_ras_slurm_convert_jobid(slurm_jobid, &slurm_id_uint);
    if (PRTE_SUCCESS != err) {
        return err;
    }

    PMIX_LIST_FOREACH(node, node_list, prte_node_t) {
        existing = prte_node_match(NULL, node->name);
        if (NULL == existing) {
            continue;
        }

        if (NULL != existing->daemon ||
            PRTE_FLAG_TEST(existing, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            return PRTE_EXISTS;
        }
    }

    PMIX_LIST_FOREACH_SAFE(node, next, node_list, prte_node_t) {
        existing = prte_node_match(NULL, node->name);
        if (NULL == existing) {
            continue;
        }

        err = prte_set_attribute(&existing->attributes, PRTE_NODE_ALLOC_ID,
                                 PRTE_ATTR_LOCAL, &slurm_id_uint, PMIX_UINT32);
        if (PRTE_SUCCESS != err) {
            return err;
        }

        existing->slots = node->slots;
        existing->slots_max = node->slots_max;
        existing->slots_inuse = 0;
        existing->state = PRTE_NODE_STATE_ADDED;

        if (0 > pmix_pointer_array_add(reused_nodes, existing)) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        pmix_list_remove_item(node_list, &node->super);
        PMIX_RELEASE(node);
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Add reused global nodes to the dynamic Slurm session.
 *
 * @param[in] slurm_jobid   Slurm job ID for the destination session.
 * @param[in] reused_nodes  Existing nodes to attach to the session.
 */
static int prte_ras_slurm_add_reused_nodes_to_session(const char *slurm_jobid,
                                                      pmix_pointer_array_t *reused_nodes)
{
    if (NULL == slurm_jobid || NULL == reused_nodes) {
        return PRTE_ERR_BAD_PARAM;
    }

    int added = 0;
    prte_session_t *session;
    prte_session_stack_item_t *item;
    prte_node_t *node;

    session = prte_get_session_object_from_id(slurm_jobid);
    if (NULL == session) {
        return PRTE_ERR_NOT_FOUND;
    }

    for (int i = 0; i < reused_nodes->size; i++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(reused_nodes, i);
        if (NULL == node) {
            continue;
        }

        PMIX_RETAIN(node);
        if (0 > pmix_pointer_array_add(session->nodes, node)) {
            PMIX_RELEASE(node);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        added++;
    }

    PMIX_LIST_FOREACH(item, prte_slurm_session_stack, prte_session_stack_item_t) {
        if (item->session == session) {
            item->nodes_in_session += added;
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_NOT_FOUND;
}

/**
 * @brief Remove a newly-created Slurm session after extend failure.
 *
 * @param[in] slurm_jobid Slurm job ID for the session to roll back.
 */
static void prte_ras_slurm_rollback_session(const char *slurm_jobid)
{
    prte_session_t *session;
    prte_session_stack_item_t *item, *next;

    session = prte_get_session_object_from_id(slurm_jobid);
    if (NULL == session) {
        return;
    }

    PMIX_LIST_FOREACH_SAFE(item, next, prte_slurm_session_stack, prte_session_stack_item_t) {
        if (item->session == session) {
            pmix_list_remove_item(prte_slurm_session_stack, &item->super);
            PMIX_RELEASE(item);
            break;
        }
    }

    PMIX_RELEASE(session);
}

/**
 * @brief Finalize a Slurm resource extension request.
 *
 * Processes newly allocated Slurm resources after the wait phase completes.
 * Newly added nodes are validated, assigned to a session, and inserted into
 * the global node pool. On success, daemon launch is triggered on the new
 * resources and the original request callback is completed.
 */
static void prte_ras_slurm_extend_wait_complete(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
 
    pmix_list_t added_nodes;
    pmix_pointer_array_t reused_nodes;
    bool have_added_nodes = false;
    bool have_reused_nodes = false;
    bool resources_added = false;
    bool requester_recorded = false;
    int added_node_count = 0;
    int reused_node_count = 0;
    
    prte_slurm_wait_tracker_t *trk = cbdata;
    prte_pmix_server_req_t *req = trk->req;

    char *job_id = trk->job_id;
    char *request_id = trk->request_id;

    int err = trk->err;

    /* The only place a tracker is released, so the only place it is dropped */
    if (0 <= trk->tracker_index) {
        pmix_pointer_array_set_item(&prte_slurm_wait_trackers, trk->tracker_index, NULL);
        trk->tracker_index = -1;
    }

    if(PRTE_SUCCESS != err) {
        goto complete;
    }

    PMIX_CONSTRUCT(&added_nodes, pmix_list_t);
    have_added_nodes = true;
    PMIX_CONSTRUCT(&reused_nodes, pmix_pointer_array_t);
    have_reused_nodes = true;

    err = prte_ras_slurm_add_modified_resources(job_id, &added_nodes);

    if(PRTE_SUCCESS != err) {
        goto complete;
    }

    err = prte_ras_slurm_extract_reused_nodes(job_id, &added_nodes, &reused_nodes);

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto complete;
    }

    /* Reject any remaining nodes that are already present in prte_node_pool.
     * Nodes that are safe to relaunch are removed above and tracked
     * separately as reused nodes. */
    err = prte_ras_slurm_reject_node_duplicates(&added_nodes);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto complete;
    }

    /* Tag nodes with session ID (Slurm job ID) */
    err = prte_ras_slurm_tag_node_allocation(job_id, &added_nodes);

    if(PRTE_SUCCESS != err) {
        goto complete;
    }

    added_node_count = pmix_list_get_size(&added_nodes);
    for (int i = 0; i < reused_nodes.size; i++) {
        if (NULL != pmix_pointer_array_get_item(&reused_nodes, i)) {
            reused_node_count++;
        }
    }

    /* Create a session  */
    err = prte_ras_slurm_assign_new_session(job_id,
                                            trk->user_request_id_provided ? request_id : NULL,
                                            &added_nodes, true);

    if(PRTE_SUCCESS != err) {
        goto complete;
    }

    /* Record who asked. The grow campaign resolves its requesters through the
     * node's PRTE_NODE_ALLOC_ID to this session, so without it neither the
     * completion nor a launch failure reaches anyone. Only for an extend: an
     * allocation discovered at startup was requested by no one. */
    {
        prte_session_t *session = prte_get_session_object_from_id(job_id);
        if (NULL != session) {
            PMIX_XFER_PROCID(&session->requestor, &req->tproc);
            requester_recorded = true;
        }
    }

    err = prte_ras_slurm_add_reused_nodes_to_session(job_id, &reused_nodes);

    if (PRTE_SUCCESS != err) {
        prte_ras_slurm_rollback_session(job_id);
        PRTE_ERROR_LOG(err);
        goto complete;
    }
    
    /* Insert into global node list. This consumes the list. */
    err = prte_ras_base_node_insert(&added_nodes, NULL);

    if(PRTE_SUCCESS != err) {
        prte_ras_slurm_rollback_session(job_id);
        PRTE_ERROR_LOG(err);
        goto complete;
    }

    prte_num_allocated_nodes += added_node_count + reused_node_count;
    resources_added = true;

    int pending_err = prte_ras_slurm_remove_pending_req(request_id);
    if(PRTE_SUCCESS != pending_err) {
        pmix_output(0, "ras:slurm:modify: failed to remove completed request %s "
                       "from the pending cancellation list: %s",
                       request_id, prte_strerror(pending_err));
    }

    complete:

    if(PRTE_ERR_JOB_CANCELLED == err) {
        /* Gone already, whether we asked or Slurm did. Drop the record rather
         * than cancel it, or the job id outlives the job and is scancelled
         * again later. */
        prte_ras_slurm_remove_pending_req(request_id);
    } else if(PRTE_SUCCESS != err && !resources_added) {
        prte_ras_slurm_cancel_pending_req(request_id);
    }

    if(have_added_nodes) {
        PMIX_DESTRUCT(&added_nodes);
        have_added_nodes = false;
    }

    if (have_reused_nodes) {
        PMIX_DESTRUCT(&reused_nodes);
        have_reused_nodes = false;
    }

    req->pstatus = prte_pmix_convert_rc(err);

    /* Report back the job ID. Only a phase one that stays PMIX_SUCCESS
     * delivers it: a client stops unpacking a reply whose status is anything
     * else, so nothing sent with PMIX_OPERATION_IN_PROGRESS arrives. Where
     * that happens the completion event carries the id instead. */
    if (PMIX_SUCCESS == req->pstatus) {
        pmix_info_t *result_info = NULL;

        PMIX_INFO_CREATE(result_info, 1);
        if (NULL == result_info) {
            req->pstatus = PMIX_ERR_NOMEM;
        } else {
            if (req->copy && NULL != req->info) {
                PMIX_INFO_FREE(req->info, req->ninfo);
            }
            PMIX_INFO_LOAD(&result_info[0], PMIX_ALLOC_ID, job_id, PMIX_STRING);
            req->info = result_info;
            req->ninfo = 1;
            req->copy = true;
        }
    }

    /* Launch daemons on the newly secured resources */
    if (PMIX_SUCCESS == req->pstatus) {
        prte_ras_base_activate_dvm_grow();

        /* A grant is not usable nodes, so the campaign's PMIX_DVM_IS_READY is
         * the answer, as it is for a release. Only where that event can
         * arrive: outside elastic mode no campaign is recorded, so the grant
         * stays the answer. Unlike serve_release_req the extend does not
         * refuse there - it has already done what was asked of Slurm. */
#if PRTE_HAVE_DVM_MOD_EVENTS
        if (prte_elastic_mode && requester_recorded) {
            req->pstatus = PMIX_OPERATION_IN_PROGRESS;
        }
#endif
    }

    /* Execute callback if necessary */
    if (NULL != req->infocbfunc) {
        req->infocbfunc(req->pstatus, req->info, req->ninfo,
                        req->cbdata, prte_pmix_server_req_release, req);
        PMIX_RELEASE(trk);
        return;
    }

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                req->local_index, NULL);

    PMIX_RELEASE(req);
    PMIX_RELEASE(trk);
}

/**
 * @brief Ask Slurm what became of an expander job.
 *
 * Runs when the submitting salloc exits. Retries briefly and boundedly: this
 * closes a gap of milliseconds, not a queue wait.
 */
static void slurm_grant_check_cb(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    prte_slurm_wait_tracker_t *trk = cbdata;

    int err;

    err = prte_ras_slurm_check_resources(trk->job_id);

    if (PRTE_ERR_RESOURCE_BUSY == err && PRTE_SLURM_GRANT_RETRY_MAX > trk->attempts) {

        struct timeval delay = {
            .tv_sec = trk->retry_delay_usec / 1000000,
            .tv_usec = trk->retry_delay_usec % 1000000
        };

        trk->attempts++;
        prte_event_evtimer_add(&trk->ev, &delay);

        /* double the waiting time with each failed attempt,
         * up to the defined maximum */
        if (PRTE_SLURM_GRANT_RETRY_MAX_USEC > trk->retry_delay_usec) {
            trk->retry_delay_usec *= 2;
            if (PRTE_SLURM_GRANT_RETRY_MAX_USEC < trk->retry_delay_usec) {
                trk->retry_delay_usec = PRTE_SLURM_GRANT_RETRY_MAX_USEC;
            }
        }

        return;
    }

    if (PRTE_ERR_RESOURCE_BUSY == err) {
        pmix_output(0, "ras:slurm:modify: job %s still has no resources after "
                       "the salloc that requested them exited; giving up",
                       trk->job_id);
        err = PRTE_ERR_TIMEOUT;
    }

    if (PRTE_ERR_JOB_CANCELLED == err) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                            "%s ras:slurm:extend_wait: request %s was cancelled",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), trk->job_id));
    }

    trk->err = err;

    prte_ras_slurm_extend_wait_complete(-1, 0, trk);
}

/**
 * @brief Answer an extend whose Slurm job has just been cancelled.
 *
 * Call after cancelling the job and removing its pending record; the completion
 * reads that record to decide whether the job needs cancelling.
 *
 * The answer is posted, not delivered here, so the cancel is answered before
 * the extend it cancelled.
 *
 * @param[in] request_id PMIx request identifier of the extend to answer.
 */
void prte_ras_slurm_extend_abort_request(const char *request_id)
{
    struct timeval immediate = {0, 0};

    if (!extend_initialized || NULL == request_id) {
        return;
    }

    for (int i = 0; i < prte_slurm_wait_trackers.size; i++) {
        prte_slurm_wait_tracker_t *trk =
            (prte_slurm_wait_tracker_t *) pmix_pointer_array_get_item(&prte_slurm_wait_trackers, i);

        if (NULL == trk || NULL == trk->request_id
            || 0 != strcmp(trk->request_id, request_id)) {
            continue;
        }

        /* Claim it before posting: the scancel kills this extend's salloc, and
         * that reap could otherwise answer first and release the tracker. Left
         * in the registry so a finalize in the same window still frees it. */
        trk->completing = true;
        trk->err = PRTE_ERR_JOB_CANCELLED;

        /* Only pending if a retry was in flight */
        prte_event_evtimer_del(&trk->ev);
        prte_event_set(prte_event_base, &trk->ev, -1, 0,
                       prte_ras_slurm_extend_wait_complete, trk);
        prte_event_evtimer_add(&trk->ev, &immediate);
        return;
    }
}

/**
 * @brief Coordinate a resource-extension request with Slurm
 *
 * Service a PMIx allocation request (PMIX_ALLOC_EXTEND) by requesting 
 * additional nodes from Slurm and adding the resulting resources to PRRTE.
 * Current implementation requires specifying PMIX_ALLOC_NUM_NODES as a PMIX_UINT64.
 *
 * @param[in] req PMIx server request describing the resource extension.
 */
int prte_ras_slurm_serve_extend_req(prte_pmix_server_req_t *req)
{
    if (!prte_ras_slurm_have_extensions()) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    int err = PRTE_SUCCESS;
    int pmix_err = PMIX_SUCCESS;

    pmix_hash_table_t slurm_jobfields;
    bool have_slurm_jobfields = false;
    bool pending_req_added = false;
    
    char *nodes_string = NULL;
    char *job_id = NULL;
    char *request_id = NULL;
    bool user_request_id_provided = false;

    uint64_t num_nodes;
    bool found = false;

    for (size_t i = 0; i < req->ninfo; i++) {

        if (0 == strcmp(req->info[i].key, PMIX_ALLOC_NUM_NODES)) {

            if (req->info[i].value.type != PMIX_UINT64) {
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }
        
            num_nodes = req->info[i].value.data.uint64;
            found = true;
        } else if (0 == strcmp(req->info[i].key, PMIX_ALLOC_REQ_ID)) {
            if (req->info[i].value.type != PMIX_STRING) {
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }
            request_id = req->info[i].value.data.string;
            user_request_id_provided = (NULL != request_id && '\0' != request_id[0]);
        }
    }

    if(!found) {
        pmix_output(0, "ras:slurm:modify: modify request invalid or unsupported.");
        err = PRTE_ERR_REQUEST;
        goto cleanup;
    }
    
    PMIX_CONSTRUCT(&slurm_jobfields, pmix_hash_table_t);

    have_slurm_jobfields = true;

    pmix_err = pmix_hash_table_init(&slurm_jobfields, total_fields_len);

    if(PMIX_SUCCESS != pmix_err) {
        err = prte_pmix_convert_status(pmix_err);
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }
    
    err = prte_ras_slurm_extract_job_fields(&slurm_jobfields);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    int rc = asprintf(&nodes_string, "%" PRIu64, num_nodes);
    
    if(0 > rc) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    pmix_err = pmix_hash_table_set_value_ptr(&slurm_jobfields, record_job_data_fields[PRTE_JOB_DATA_NODES],
                            strlen(record_job_data_fields[PRTE_JOB_DATA_NODES]), (void*)nodes_string);

    if(PMIX_SUCCESS != pmix_err) {
        err = prte_pmix_convert_status(pmix_err);
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    /* Now owned by hash table */
    nodes_string = NULL;

    err = prte_ras_slurm_launch_expander_job(&slurm_jobfields);

    if(PRTE_SUCCESS != err) {
        pmix_output(0, "ras:slurm:modify: error launching Slurm job with new resources.");
        goto cleanup;
    }

    pmix_err = pmix_hash_table_get_value_ptr(&slurm_jobfields, record_job_data_fields[PRTE_JOB_DATA_JOB_ID],
                    strlen(record_job_data_fields[PRTE_JOB_DATA_JOB_ID]), (void**)&job_id);

    if(PMIX_SUCCESS != pmix_err) {
        err = prte_pmix_convert_status(pmix_err);
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        request_id = job_id;
    }

    err = prte_ras_slurm_add_pending_req(request_id, job_id);

    if(PRTE_SUCCESS != err) {
        prte_ras_slurm_kill_job(job_id, NULL, 0);
        goto cleanup;
    }

    pending_req_added = true;

    /* Nothing is asked of Slurm here. salloc holds the handshake until the job
     * leaves PENDING, so its exit is the notice that there is something to
     * ask about. */

    prte_slurm_wait_tracker_t *trk;
    
    trk = PMIX_NEW(prte_slurm_wait_tracker_t);

    if(NULL == trk) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    trk->req = req;
    PMIX_RETAIN(req);

    trk->request_id = strdup(request_id);

    if(NULL == trk->request_id) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        PMIX_RELEASE(trk);
        goto cleanup;
    }

    trk->user_request_id_provided = user_request_id_provided;

    trk->job_id = strdup(job_id);

    if(NULL == trk->job_id) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        PMIX_RELEASE(trk);
        goto cleanup;
    }

    trk->tracker_index = pmix_pointer_array_add(&prte_slurm_wait_trackers, trk);

    if(0 > trk->tracker_index) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        PMIX_RELEASE(trk);
        goto cleanup;
    }

    /* Set but not added: the timer is only for retries. Setting it here lets
     * finalize delete it without knowing whether it ever armed. */
    prte_event_set(prte_event_base, &trk->ev, -1, 0, slurm_grant_check_cb, trk);

    prte_ras_slurm_link_child_to_tracker(job_id, trk->tracker_index);

    /* Return control to application while we wait */
    err = PRTE_ERR_OP_IN_PROGRESS;

    cleanup:

    if(PRTE_SUCCESS != err && PRTE_ERR_OP_IN_PROGRESS != err && pending_req_added) {
        prte_ras_slurm_cancel_pending_req(request_id);
    }

    free(nodes_string);

    if(have_slurm_jobfields) {
        void *key;
        void *val;

        PMIX_HASH_TABLE_FOREACH_PTR(key, val, &slurm_jobfields, {
            free(val);
        });

        PMIX_DESTRUCT(&slurm_jobfields);
    }

    return err;
}
