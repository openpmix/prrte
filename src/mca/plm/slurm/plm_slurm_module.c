/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prte_config.h"
#include "src/runtime/prte_globals.h"

#include <string.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif

#include "src/mca/base/pmix_base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/pinstalldirs/pinstalldirs_types.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/prte_dvm_key.h"
#include "src/util/pmix_fd.h"

#include "constants.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "types.h"

#include "src/prted/prted.h"

#include "plm_slurm.h"
#include "src/mca/common/slurm/common_slurm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Local functions
 */
static int plm_slurm_init(void);
static int plm_slurm_launch_job(prte_job_t *jdata);
static int plm_slurm_terminate_prteds(void);
static int plm_slurm_signal_job(pmix_nspace_t jobid, int32_t signal);
static int plm_slurm_finalize(void);

static int plm_slurm_start_proc(int argc, char **argv,
                                char *prefix, char *pmix_prefix,
                                uint32_t job_id, char *nodefile);
static void clear_parent_slurm_allocation_env(void);

/*
 * Global variable
 */
prte_plm_base_module_1_0_0_t prte_plm_slurm_module = {
    .init = plm_slurm_init,
    .set_hnp_name = prte_plm_base_set_hnp_name,
    .spawn = plm_slurm_launch_job,
    .terminate_job = prte_plm_base_prted_terminate_job,
    .terminate_orteds = plm_slurm_terminate_prteds,
    .terminate_procs = prte_plm_base_prted_kill_local_procs,
    .signal_job = plm_slurm_signal_job,
    .finalize = plm_slurm_finalize
};

/*
 * Local variables
 */
static pid_t primary_srun_pid = 0;
static bool primary_pid_set = false;

/* What srun_wait_cb needs to know about the srun it is reaping */
typedef struct {
    uint32_t job_id; /* Slurm job the daemons were launched into */
    char *nodefile;  /* file srun read its nodes from */
} plm_slurm_srun_t;
static void launch_daemons(int fd, short args, void *cbdata);

/* Remove allocation-shape values inherited from the Slurm job containing the
 * DVM master.  A later elastic grow can launch srun against a different job ID
 * whose node count and node list differ from that parent allocation.  Leaving
 * these values set makes srun validate the explicit --nodes/--nodelist request
 * against the old allocation.  Slurm regenerates the corresponding values for
 * the remote tasks in the newly-created step. */
static void clear_parent_slurm_allocation_env(void)
{
    static const char *const vars[] = {
        "SLURM_JOB_NUM_NODES",
        "SLURM_NNODES",
        "SLURM_JOB_NODELIST",
        "SLURM_NODELIST",
        "SLURM_TASKS_PER_NODE",
        NULL
    };

    for (int i = 0; NULL != vars[i]; i++) {
        unsetenv(vars[i]);
    }
}

/*
 * An elastic shrink can make an srun launcher exit non-zero without indicating
 * an unexpected daemon failure. If the Slurm session is gone, PRRTE has already
 * destroyed the session for that job ID, so the launcher termination is
 * expected. If the session remains but only the HNP node survives, the shrink
 * removed every daemon task launched by this srun step while leaving the
 * controlling node alive.
 */
static bool srun_exit_expected(uint32_t job_id)
{
    prte_session_t *session;
    prte_node_t *node, *remaining = NULL;
    int count = 0;

    if (!prte_elastic_mode) {
        return false;
    }

    session = prte_get_session_object(job_id);
    if (NULL == session) {
        return true;
    }

    if (!PRTE_PROC_IS_MASTER || NULL == session->nodes) {
        return false;
    }

    for (int i = 0; i < session->nodes->size; i++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);
        if (NULL == node) {
            continue;
        }
        remaining = node;
        count++;
        if (1 < count) {
            return false;
        }
    }

    if (1 != count || NULL == remaining) {
        return false;
    }

    if (NULL != remaining->daemon &&
        remaining->daemon->name.rank == PRTE_PROC_MY_NAME->rank) {
        return true;
    }

    return NULL != remaining->name && NULL != prte_process_info.nodename &&
           0 == strcmp(remaining->name, prte_process_info.nodename);
}

/**
 * Init the module
 */
static int plm_slurm_init(void)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_start())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* we assign daemon nodes at launch: srun places the daemons in the
     * order launch_daemons lists their nodes */
    prte_plm_globals.daemon_nodes_assigned_at_launch = true;

    /* point to our launch command */
    if (PRTE_SUCCESS
        != (rc = prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS, launch_daemons))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    return rc;
}

/* When working in this function, ALWAYS jump to "cleanup" if
 * you encounter an error so that prun will be woken up and
 * the job can cleanly terminate
 */
static int plm_slurm_launch_job(prte_job_t *jdata)
{
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        /* this is a restart situation - skip to the mapping stage */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    } else {
        /* new job - set it up */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_INIT);
    }
    return PRTE_SUCCESS;
}

/* srun reads the file with slurm_read_hostfile, which ends a name at a
 * newline, splits it at a comma and reads '*N' after it as N copies.
 * Refuse the names that would silently become other hosts. */
static bool nodefile_name_ok(const char *name)
{
    const char *star = strchr(name, '*');

    if (NULL != strpbrk(name, "\n,")) {
        return false;
    }
    /* slurm_read_hostfile looks only at the first '*' */
    if (NULL != star && 0 != atoi(star + 1)) {
        return false;
    }
    return true;
}

/* Write one name and its newline at out and return the bytes written, or
 * with out NULL only count them.  A '#' in the name must be escaped as
 * '\#', two bytes: slurm_read_hostfile reads a bare '#' as the start of a
 * comment. */
static size_t nodefile_line(char *out, const char *name)
{
    const char *c;
    size_t len = 0;

    for (c = name; '\0' != *c; c++) {
        if ('#' == *c) {
            if (NULL != out) {
                out[len] = '\\';
            }
            len++;
        }
        if (NULL != out) {
            out[len] = *c;
        }
        len++;
    }
    if (NULL != out) {
        out[len] = '\n';
    }
    return len + 1;
}

/* Write the names, one per line, to a file named for the launch's first
 * vpid in the daemon job's session directory, which only this user can
 * write.  O_EXCL refuses anything already at the path, a symlink included. */
static int nodefile_write(prte_job_t *daemons, pmix_rank_t vpid_start,
                          char **names, char **path)
{
    char *file, *text;
    size_t len = 0, off = 0;
    int fd, n, err = 0;

    *path = NULL;
    for (n = 0; NULL != names[n]; n++) {
        if (!nodefile_name_ok(names[n])) {
            pmix_output(0, "%s plm:slurm: cannot pass node name \"%s\" to srun",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), names[n]);
            return PRTE_ERR_BAD_PARAM;
        }
        len += nodefile_line(NULL, names[n]);
    }
    if (NULL == daemons->session_dir) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    text = malloc(len);
    if (NULL == text) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    for (n = 0; NULL != names[n]; n++) {
        off += nodefile_line(text + off, names[n]);
    }

    if (0 > pmix_asprintf(&file, "%s/srun-nodes.%u", daemons->session_dir,
                          (unsigned) vpid_start)) {
        free(text);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    fd = open(file, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    if (0 > fd) {
        err = errno;
    } else {
        if (PMIX_SUCCESS != pmix_fd_write(fd, (int) len, text)) {
            err = errno;
        }
        if (0 != close(fd) && 0 == err) {
            err = errno;
        }
        if (0 != err) {
            unlink(file);
        }
    }
    free(text);
    if (0 != err) {
        pmix_output(0, "%s plm:slurm: cannot write srun's node file %s: %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), file, strerror(err));
        free(file);
        return PRTE_ERR_SILENT;
    }

    *path = file;
    return PRTE_SUCCESS;
}

static void launch_daemons(int fd, short args, void *cbdata)
{
    prte_node_t *node;
    int32_t n;
    prte_job_map_t *map;
    char *param;
    char **argv = NULL;
    int argc;
    int rc;
    char *tmp;
    char *nodelist_flat;
    char **nodelist_argv = NULL;
    char *name_string;
    char **custom_strings;
    int num_args, i;
    char *cur_prefix = NULL;
    char *pmix_prefix = NULL;
    int proc_vpid_index;
    bool failed_launch = true;
    prte_job_t *daemons;
    prte_state_caddy_t *state = (prte_state_caddy_t *) cbdata;
    uint32_t job_id = UINT32_MAX;
    uint32_t node_job_id;
    void *data = &node_job_id;
    prte_session_t *session = NULL;
    char *nodefile = NULL;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(state);

    PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:slurm: LAUNCH DAEMONS CALLED",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* start by setting up the virtual machine */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (PRTE_SUCCESS != (rc = prte_plm_base_setup_virtual_machine(state->jdata))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if we don't want to launch, then don't attempt to
     * launch the daemons - the user really wants to just
     * look at the proposed process map
     */
    if (PRTE_ATTR_IS_TRUE(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH)) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PMIX_RELEASE(state);
        return;
    }

    /* Get the map for this job */
    if (NULL == (map = daemons->map)) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (0 == map->num_new_daemons) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:slurm: no new daemons to launch",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PMIX_RELEASE(state);
        return;
    }

    /*
     * start building argv array
     */
    argv = NULL;
    argc = 0;

    /*
     * SLURM srun OPTIONS
     */

    /* add the srun command */
    pmix_argv_append(&argc, &argv, "srun");

    // add the external launcher flag if necessary -- srun only grew it in
    // 23.11, and the version is the one src/mca/common/slurm probed once
    if (!prte_common_slurm_version()->early) {
        pmix_argv_append(&argc, &argv, "--external-launcher");
    }

    /* start one orted on each node */
    pmix_argv_append(&argc, &argv, "--ntasks-per-node=1");

    /* Never let Slurm kill the step because one prted died.
     *
     * This is unconditional on purpose.  --kill-on-bad-exit tells srun to
     * take down the WHOLE step when any one task exits non-zero, and --no-kill
     * is what stops a single node's failure doing the same - so between them
     * they decide whether losing one daemon costs us one daemon or all of the
     * daemons that srun launched.  PRRTE decides what a lost daemon means
     * (see errmgr/dvm, and the recoverable/continuous runtime options); the
     * scheduler must not pre-empt that decision by killing the survivors.
     *
     * It matters far more now than it used to: a prted stays inside its step
     * rather than daemonizing out of it, so these flags reach real, live
     * daemons instead of a fork's parent that had already exited. */
    pmix_argv_append(&argc, &argv, "--no-kill");
    pmix_argv_append(&argc, &argv, "--kill-on-bad-exit=0");

    /* our daemons are not an MPI task */
    pmix_argv_append(&argc, &argv, "--mpi=none");

    /* ensure the orteds are not bound to a single processor,
     * just in case the TaskAffinity option is set by default.
     * This will *not* release the orteds from any cpu-set
     * constraint, but will ensure it doesn't get
     * bound to only one processor
     */
    pmix_argv_append(&argc, &argv, "--cpu-bind=none");

    /* Append user defined arguments to srun */
    if (NULL != prte_mca_plm_slurm_component.custom_args) {
        custom_strings = PMIx_Argv_split(prte_mca_plm_slurm_component.custom_args, ' ');
        num_args = PMIx_Argv_count(custom_strings);
        for (i = 0; i < num_args; ++i) {
            pmix_argv_append(&argc, &argv, custom_strings[i]);
        }
        PMIx_Argv_free(custom_strings);
    }

    /* create the nodelist, in vpid order, from this launch's daemons, which
     * setup_vm gave the consecutive vpids from daemon_vpid_start.  The
     * daemon map also holds the nodes of an earlier launch whose daemons
     * have not reported yet, which a walk of the map would include. */
    node = NULL;
    for (n = 0; n < map->num_new_daemons; n++) {
        prte_proc_t *daemon = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs,
                                                    (int) (map->daemon_vpid_start + (pmix_rank_t) n));
        if (NULL == daemon || NULL == daemon->node) {
            rc = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        PMIx_Argv_append_nosize(&nodelist_argv, daemon->node->name);
        if (0 == n) {
            node = daemon->node;
        }
    }

    /* find job ID of the first node to launch on; we make the assumption
     * here that all other nodes in the launch share that job ID */
    if (prte_get_attribute(&node->attributes, PRTE_NODE_ALLOC_ID, &data, PMIX_UINT32)) {
        job_id = node_job_id;
    }

    /* could not find job ID of nodes to launch */
    if(UINT32_MAX == job_id) {
        rc = PRTE_ERR_NOT_FOUND;
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    session = prte_get_session_object(job_id);

    if(NULL == session) {
        rc = PRTE_ERR_NOT_FOUND;
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* by specifying a job ID explicitly, we can launch
    *  daemons into other allocations than the one we are in,
    *  if necessary. */
    pmix_asprintf(&tmp, "--jobid=%"PRIu32, job_id);
    pmix_argv_append(&argc, &argv, tmp);
    free(tmp);

    /* Pass the nodes in a file: as one --nodelist argument the list hits
     * the kernel's per-argument limit at scale.  srun reads a --nodelist
     * value containing '/' as a file, which a session-dir path always is. */
    rc = nodefile_write(daemons, map->daemon_vpid_start, nodelist_argv, &nodefile);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }

    /* Each daemon takes the base vpid plus its task index in this step, so
     * the tasks must be numbered in vpid order, the order of the file.
     * Slurm's own node order is not that order once a node released earlier
     * is granted again: the node keeps its old place in the pool.  The
     * arbitrary distribution numbers the tasks in the order the file lists
     * the nodes.  It takes the node count from the file and refuses
     * --nodes. */
    pmix_argv_append(&argc, &argv, "--distribution=arbitrary");

    pmix_asprintf(&tmp, "--nodelist=%s", nodefile);
    pmix_argv_append(&argc, &argv, tmp);
    free(tmp);

    /* tell srun how many tasks to run */
    pmix_asprintf(&tmp, "--ntasks=%lu", (unsigned long) map->num_new_daemons);
    pmix_argv_append(&argc, &argv, tmp);
    free(tmp);

    /* the srun command line shows only the file, so log the nodes at the
     * verbosity that command line is logged at */
    if (0 < pmix_output_get_verbosity(prte_plm_base_framework.framework_output)) {
        nodelist_flat = PMIx_Argv_join(nodelist_argv, ',');
        pmix_output(prte_plm_base_framework.framework_output,
                    "%s plm:slurm: launching on nodes %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    nodelist_flat);
        free(nodelist_flat);
    }

    /*
     * PRTED OPTIONS
     */

    /* add the daemon command (as specified by user) */
    prte_plm_base_setup_prted_cmd(&argc, &argv);

    /* Add basic orted command line options, including debug flags */
    prte_plm_base_prted_append_basic_args(&argc, &argv, "slurm", &proc_vpid_index);

    /* tell the new daemons the base of the name list so they can compute
     * their own name on the other end
     */
    rc = prte_util_convert_vpid_to_string(&name_string, map->daemon_vpid_start);
    if (PRTE_SUCCESS != rc) {
        pmix_output(0, "plm_slurm: unable to get daemon vpid as string");
        goto cleanup;
    }

    free(argv[proc_vpid_index]);
    argv[proc_vpid_index] = strdup(name_string);
    free(name_string);

    /*
     * Any prefix was installed in the DAEMON job object, so
     * we only need to look there to find it. This covers any
     * prefix by default, PRTE_PREFIX given in the environment,
     * and '--prefix' from the cmd line
     */
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_PREFIX, (void **) &cur_prefix, PMIX_STRING)) {
        cur_prefix = NULL;
    }
    /* Similarly, we have to check for any PMIx prefix that was specified */
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_PMIX_PREFIX, (void **) &pmix_prefix, PMIX_STRING)) {
        pmix_prefix = NULL;
    }

    /* protect the args in case someone has a script wrapper around srun */
    prte_plm_base_wrap_args(argv);

    if (0 < pmix_output_get_verbosity(prte_plm_base_framework.framework_output)) {
        param = PMIx_Argv_join(argv, ' ');
        pmix_output(prte_plm_base_framework.framework_output,
                    "%s plm:slurm: final top-level argv:\n\t%s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    (NULL == param) ? "NULL" : param);
        if (NULL != param)
            free(param);
    }

    /* exec the daemon(s) */
    if (PRTE_SUCCESS != (rc = plm_slurm_start_proc(argc, argv, cur_prefix,
                                                   pmix_prefix, job_id, nodefile))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    /* the srun's tracker owns the file now */
    nodefile = NULL;

    /* indicate that the daemons for this job were launched */
    state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
    daemons->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;

    /* flag that launch was successful, so far as we currently know */
    failed_launch = false;

cleanup:
    if (NULL != argv) {
        PMIx_Argv_free(argv);
    }
    if (NULL != nodelist_argv) {
        PMIx_Argv_free(nodelist_argv);
    }
    if (NULL != nodefile) {
        unlink(nodefile);
        free(nodefile);
    }
    if (NULL != cur_prefix) {
        free(cur_prefix);
    }
    if (NULL != pmix_prefix) {
        free(pmix_prefix);
    }
    /* check for failed launch - if so, force terminate */
    if (failed_launch) {
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
    }

    /* cleanup the caddy */
    PMIX_RELEASE(state);
}

/**
 * Terminate the orteds for a given job
 */
static int plm_slurm_terminate_prteds(void)
{
    int rc = PRTE_SUCCESS;
    prte_job_t *jdata;

    /* check to see if the primary pid is set. If not, this indicates
     * that we never launched any additional daemons, so we cannot
     * not wait for a waitpid to fire and tell us it's okay to
     * exit. Instead, we simply trigger an exit for ourselves
     */
    if (primary_pid_set) {
        if (PRTE_SUCCESS != (rc = prte_plm_base_prted_exit(PRTE_DAEMON_EXIT_CMD))) {
            PRTE_ERROR_LOG(rc);
        }
    } else {
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:slurm: primary daemons complete!",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
        /* need to set the #terminated value to avoid an incorrect error msg */
        jdata->num_terminated = jdata->num_procs;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
    }

    return rc;
}

/**
 * Signal all the processes in the child srun by sending the signal directly to it
 */
static int plm_slurm_signal_job(pmix_nspace_t jobid, int32_t signal)
{
    int rc = PRTE_SUCCESS;

    /* order them to pass this signal to their local procs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_signal_local_procs(jobid, signal))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

static int plm_slurm_finalize(void)
{
    int rc;

    /* cleanup any pending recvs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
        PRTE_ERROR_LOG(rc);
    }

    return PRTE_SUCCESS;
}

/* Remove a reaped srun's node file, and free its tracker and the state it
 * carries */
static void srun_release(prte_wait_tracker_t *t2)
{
    plm_slurm_srun_t *srun = (plm_slurm_srun_t *) t2->cbdata;

    if (NULL != srun) {
        if (NULL != srun->nodefile) {
            unlink(srun->nodefile);
            free(srun->nodefile);
        }
        free(srun);
    }
    PMIX_RELEASE(t2);
}

static void srun_wait_cb(int sd, short fd, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    prte_proc_t *proc = t2->child;
    plm_slurm_srun_t *srun = (plm_slurm_srun_t *) t2->cbdata;
    prte_job_t *jdata;
    const prte_common_slurm_version_t *slurm;
    PRTE_HIDE_UNUSED_PARAMS(sd, fd);

    jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* need to check that we are at least version 17.11 */
    slurm = prte_common_slurm_version();
    if (slurm->ancient) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-plm-slurm.txt", "ancient-version", true,
                       slurm->major, slurm->minor);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
        srun_release(t2);
        return;
    }

    /* According to the SLURM folks, srun always returns the highest exit
     code of our remote processes. Thus, a non-zero exit status doesn't
     necessarily mean that srun failed - it could be that an orted returned
     a non-zero exit status. Of course, that means the orted failed(!), so
     the end result is the same - the job didn't start.

     As a result, we really can't do much with the exit status itself - it
     could be something in errno (if srun itself failed), or it could be
     something returned by an orted, or it could be something returned by
     the OS (e.g., couldn't find the orted binary). Somebody is welcome
     to sort out all the options and pretty-print a better error message. For
     now, though, the only thing that really matters is that
     srun failed. Report the error and make sure that prun
     wakes up - otherwise, do nothing!

     Unfortunately, the pid returned here is the srun pid, not the pid of
     the proc that actually died! So, to avoid confusion, just use -1 as the
     pid so nobody thinks this is real
     */


    /* abort only if the status returned is non-zero - i.e., if
     * the orteds exited with an error
     */
    if (0 != proc->exit_code) {
        if (NULL != srun && srun_exit_expected(srun->job_id)) {
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:slurm: srun for elastic job %" PRIu32
                                 " exited with status %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 srun->job_id, proc->exit_code));
            srun_release(t2);
            return;
        }

        /* an orted must have died unexpectedly - report
         * that the daemon has failed so we exit
         */
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-plm-slurm.txt", "srun-failed", true,
                       proc->exit_code);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
    } else {
        /* otherwise, check to see if this is the primary pid.
         *
         * We never pass --daemonize to the prteds we launch, so the process
         * srun is tracking IS the daemon and not a fork's parent that exits
         * the moment the daemon is up - see src/tools/prted/AGENTS.md for why
         * a prted must stay inside its Slurm step. A clean exit here is
         * therefore the daemons genuinely ending, so fire the trigger that
         * lets prun/the HNP exit.
         */
        if (primary_srun_pid == proc->pid) {
            /* An elastic release ends the daemons this srun launched on
             * purpose, and because the prted stays attached it ends them
             * CLEANLY - so a zero exit reaches us for the same reason a
             * non-zero one does below, and needs the same question asked of
             * it. Without this the release of a node belonging to the
             * primary step reads as the DVM ending and takes the whole DVM
             * down with it. */
            if (NULL != srun && srun_exit_expected(srun->job_id)) {
                PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                     "%s plm:slurm: srun for elastic job %" PRIu32
                                     " exited cleanly after its daemons were released",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), srun->job_id));
                srun_release(t2);
                return;
            }
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:slurm: primary daemons complete!",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* need to set the #terminated value to avoid an incorrect error msg */
            jdata->num_terminated = jdata->num_procs;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
        }
    }

    /* done with this dummy */
    srun_release(t2);
}

static int plm_slurm_start_proc(int argc, char **argv,
                                char *prefix, char *pmix_prefix,
                                uint32_t job_id, char *nodefile)
{
    int fd;
    int srun_pid;
    int n;
    char **tmp = NULL, *p;
    char *exec_argv = pmix_path_findv(argv[0], 0, environ, NULL);
    prte_proc_t *dummy;
    char *oldenv, *newenv;
    plm_slurm_srun_t *tracked;
    PRTE_HIDE_UNUSED_PARAMS(argc);

    if (NULL == exec_argv) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-plm-slurm.txt", "no-srun", true);
        return PRTE_ERR_SILENT;
    }

    srun_pid = fork();
    if (-1 == srun_pid) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
        free(exec_argv);
        return PRTE_ERR_SYS_LIMITS_CHILDREN;
    }
    /* if this is the primary launch - i.e., not a comm_spawn of a
     * child job - then save the pid
     */
    if (0 < srun_pid && !primary_pid_set) {
        primary_srun_pid = srun_pid;
        primary_pid_set = true;
    }

    if (0 < srun_pid) {
        /* setup a dummy proc object to track the srun */
        dummy = PMIX_NEW(prte_proc_t);
        if (NULL == dummy) {
            kill(srun_pid, SIGTERM);
            free(exec_argv);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        tracked = malloc(sizeof(*tracked));
        if (NULL == tracked) {
            kill(srun_pid, SIGTERM);
            PMIX_RELEASE(dummy);
            free(exec_argv);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        tracked->job_id = job_id;
        tracked->nodefile = nodefile;
        dummy->pid = srun_pid;
        /* be sure to mark it as alive so we don't instantly fire */
        PRTE_FLAG_SET(dummy, PRTE_PROC_FLAG_ALIVE);
        /* setup the waitpid so we can find out if srun succeeds! */
        prte_wait_cb(dummy, srun_wait_cb, tracked);
    }

    if (0 == srun_pid) { /* child */
        char *bin_base = NULL, *lib_base = NULL;

        clear_parent_slurm_allocation_env();

        /* Slurm forwards the entire environment, which we
         * REALLY don't want them to do as it might contain
         * envars pertaining to tool connections. So purge
         * the environment of anything PMIx/PRRTE related -
         * we will have put anything we need on the cmd line */
        for (n=0; NULL != environ[n]; n++) {
            if (0 == strncmp(environ[n], "PMIX_", 5) ||
                0 == strncmp(environ[n], "PRTE_", 5)) {
                PMIx_Argv_append_nosize(&tmp, environ[n]);
            }
        }
        if (NULL != tmp) {
            for (n=0; NULL != tmp[n]; n++) {
                p = strchr(tmp[n], '=');
                if (NULL == p) {
                    /* not an assignment - nothing to unset */
                    continue;
                }
                *p = '\0';
                unsetenv(tmp[n]);
            }
            PMIx_Argv_free(tmp);
        }

        /* ...except the DVM key, which goes nowhere else: srun hands its
         * environment to every daemon it starts, and an environment is
         * private to its own user and root - unlike the command line we
         * put everything else on */
        if (prte_oob_authenticate) {
            prte_dvm_key_setenv(&environ);
        }

        /* Figure out the basenames for the libdir and bindir.  There
           is a lengthy comment about this in plm_rsh_module.c
           explaining all the rationale for how / why we're doing
           this. */

        lib_base = pmix_basename(prte_install_dirs.libdir);
        bin_base = pmix_basename(prte_install_dirs.bindir);

        /* If we have a prefix, then modify the PATH and
           LD_LIBRARY_PATH environment variables.  */
        if (NULL != prefix) {

            /* Reset PATH */
            oldenv = getenv("PATH");
            if (NULL != oldenv) {
                pmix_asprintf(&newenv, "%s/%s:%s", prefix, bin_base, oldenv);
            } else {
                pmix_asprintf(&newenv, "%s/%s", prefix, bin_base);
            }
            PMIx_Setenv("PATH", newenv, true, &environ);
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:slurm: reset PATH: %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 newenv));
            free(newenv);

            /* Reset LD_LIBRARY_PATH */
            oldenv = getenv("LD_LIBRARY_PATH");
            if (NULL != oldenv) {
                pmix_asprintf(&newenv, "%s/%s:%s", prefix, lib_base, oldenv);
            } else {
                pmix_asprintf(&newenv, "%s/%s", prefix, lib_base);
            }
            PMIx_Setenv("LD_LIBRARY_PATH", newenv, true, &environ);
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:slurm: reset LD_LIBRARY_PATH: %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), newenv));
            free(newenv);

            // need to export it as well so srun will propagate it
            PMIx_Setenv("PRTE_PREFIX", prefix, true, &environ);
        }

        /* for pmix_prefix, we only have to modify the library path.
         * NOTE: obviously, we cannot know the lib_base used for the
         * PMIx library. All we can do is hope they used the same one
         * for PMIx as they did for the one they linked to PRRTE */
        if (NULL != pmix_prefix) {
            oldenv = getenv("LD_LIBRARY_PATH");
            p = pmix_basename(pmix_pinstall_dirs.libdir);
            if (NULL != oldenv) {
                pmix_asprintf(&newenv, "%s/%s:%s", pmix_prefix, p, oldenv);
            } else {
                pmix_asprintf(&newenv, "%s/%s", pmix_prefix, p);
            }
            free(p);
            PMIx_Setenv("LD_LIBRARY_PATH", newenv, true, &environ);
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:slurm: reset LD_LIBRARY_PATH: %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), newenv));
            free(newenv);
             // need to export it as well so srun will propagate it
            PMIx_Setenv("PMIX_PREFIX", pmix_prefix, true, &environ);
       }

        fd = open("/dev/null", O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (fd >= 0) {
            dup2(fd, 0);
            /* When not in debug mode and --debug-daemons was not passed,
             * tie stdout/stderr to dev null so we don't see messages from orted
             * EXCEPT if the user has requested that we leave sessions attached
             */
            if (0 > pmix_output_get_verbosity(prte_plm_base_framework.framework_output)
                && !prte_debug_daemons_flag && !prte_leave_session_attached) {
                dup2(fd, 1);
                dup2(fd, 2);
            }

            /* Don't leave the extra fd to /dev/null open */
            if (fd > 2) {
                close(fd);
            }
        }

        /* srun lives as long as the daemons it launched, so anything
         * else it inherited - our sockets to other daemons among them -
         * would stay open on its account until the job ended */
        pmix_close_open_file_descriptors(-1);

        /* get the srun process out of prun's process group so that
           signals sent from the shell (like those resulting from
           cntl-c) don't get sent to srun */
        setpgid(0, 0);

        execvp(exec_argv, argv);

        pmix_output(0, "plm:slurm:start_proc: exec failed");
        /* don't return - need to exit - returning would be bad -
           we're not in the calling process anymore */
        exit(1);
    } else { /* parent */
        /* just in case, make sure that the srun process is not in our
           process group any more.  Stevens says always do this on both
           sides of the fork... */
        setpgid(srun_pid, srun_pid);

        free(exec_argv);
    }

    return PRTE_SUCCESS;
}
