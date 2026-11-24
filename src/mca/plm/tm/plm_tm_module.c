/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
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
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Launch the daemons of a DVM inside a PBS job as tasks of the job, so
 * that PBS charges the job for each of them and for everything they run,
 * and takes them down with it.
 *
 * Only the TM API starts a task of the job, and PRRTE does not call it:
 * some builds of the TM library carry private copies of libraries PMIx is
 * also linked against, and the two cannot share a process. So every TM
 * call is made by a helper, prte_tm_spawn, installed in
 * $(libexecdir)/prte and linked against the TM library and nothing of
 * ours. One helper is started per launch, for all of that launch's
 * daemons:
 *
 *  - its argv is the daemon's argv, and which argument is the vpid. Its
 *    environment is the one the daemons get - ours, with our PMIX_ and
 *    PRTE_ variables taken out (they are on the daemon's command line) and
 *    the prefix's search paths put in;
 *  - its stdin is the DVM key, as PRTE_DVM_KEY - added to the daemons'
 *    environment and to nothing else - followed by "<vpid> <host>" for
 *    each daemon. The helper hands each daemon the key in the environment
 *    tm_spawn() sends to the mom, and the prted reads it from there, as it
 *    does under srun;
 *  - its stdout tells us of each daemon that could not be started or whose
 *    task has ended. One that ends before it reported in failed to start;
 *    one that had reported has died, and its lost connection already tells
 *    errmgr.
 *
 * The daemon is never told to daemonize: it has to remain the task PBS
 * started, or PBS stops accounting for it. There is no tree spawn - the
 * mother superior's mom fans the spawns out itself.
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/pinstalldirs/pinstalldirs_types.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/state/state.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/prte_dvm_key.h"
#include "src/util/prte_hmac.h"
#include "src/util/prte_show_help.h"

#include "plm_tm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

static int plm_tm_init(void);
static int plm_tm_launch_job(prte_job_t *jdata);
static int plm_tm_terminate_orteds(void);
static int plm_tm_finalize(void);

prte_plm_base_module_t prte_plm_tm_module = {
    .init = plm_tm_init,
    .set_hnp_name = prte_plm_base_set_hnp_name,
    .spawn = plm_tm_launch_job,
    .remote_spawn = NULL,
    .terminate_job = prte_plm_base_prted_terminate_job,
    .terminate_orteds = plm_tm_terminate_orteds,
    .terminate_procs = prte_plm_base_prted_kill_local_procs,
    .signal_job = prte_plm_base_prted_signal_local_procs,
    .finalize = plm_tm_finalize
};

/* One run of the helper: the daemons it was asked to start, and the two
 * pipes to it. On `launches` from the fork until the helper is reaped. */
typedef struct {
    pmix_list_item_t super;
    /* what prte_wait_cb tracks: carries the helper's pid */
    prte_proc_t *helper;
    /* this launch's daemons, retained, and whether each has been settled -
     * reported as failed, or seen to need no report */
    prte_proc_t **daemons;
    bool *settled;
    int ndaemons;
    /* the helper's stdin: what we still have to write to it */
    int in_fd;
    int in_rfd;
    char *in_buf;
    size_t in_len;
    size_t in_sent;
    prte_event_t in_ev;
    bool in_ev_active;
    /* the helper's stdout, and the part of a line read so far */
    int out_fd;
    prte_event_t out_ev;
    bool out_ev_active;
    char out_buf[256];
    size_t out_len;
} tm_launch_t;

static void finish_input(tm_launch_t *l);
static void stop_output(tm_launch_t *l);

static void launch_con(tm_launch_t *l)
{
    l->helper = NULL;
    l->daemons = NULL;
    l->settled = NULL;
    l->ndaemons = 0;
    l->in_fd = -1;
    l->in_rfd = -1;
    l->in_buf = NULL;
    l->in_len = 0;
    l->in_sent = 0;
    /* each event is set when its descriptor is, and the active flags say
     * when that has happened */
    memset(&l->in_ev, 0, sizeof(l->in_ev));
    l->in_ev_active = false;
    l->out_fd = -1;
    memset(&l->out_ev, 0, sizeof(l->out_ev));
    l->out_ev_active = false;
    memset(l->out_buf, 0, sizeof(l->out_buf));
    l->out_len = 0;
}
static void launch_des(tm_launch_t *l)
{
    int n;

    finish_input(l);
    stop_output(l);
    for (n = 0; n < l->ndaemons; n++) {
        if (NULL != l->daemons[n]) {
            PMIX_RELEASE(l->daemons[n]);
        }
    }
    free(l->daemons);
    free(l->settled);
    if (NULL != l->helper) {
        PMIX_RELEASE(l->helper);
    }
}
static PMIX_CLASS_INSTANCE(tm_launch_t, pmix_list_item_t, launch_con, launch_des);

static pmix_list_t launches;
static bool initialized = false;

static void launch_daemons(int fd, short args, void *cbdata);

static int plm_tm_init(void)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS,
                                                       launch_daemons))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_start())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* the helper starts each daemon on the node we name */
    prte_plm_globals.daemon_nodes_assigned_at_launch = true;
    PMIX_CONSTRUCT(&launches, pmix_list_t);
    initialized = true;
    return PRTE_SUCCESS;
}

static int plm_tm_launch_job(prte_job_t *jdata)
{
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    } else {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_INIT);
    }
    return PRTE_SUCCESS;
}

/* A daemon of this launch will not report in. Says so once per daemon,
 * and not for one that has reported - its later end is a lost connection,
 * which errmgr hears of already - nor while the DVM is coming down. */
static void daemon_failed(tm_launch_t *l, int n, int status, const char *reason)
{
    prte_proc_t *daemon = l->daemons[n];
    prte_job_t *jdata;

    if (l->settled[n]) {
        return;
    }
    l->settled[n] = true;
    if (prte_prteds_term_ordered || prte_abnormal_term_ordered || NULL != daemon->rml_uri) {
        return;
    }

    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-plm-tm.txt", "daemon-failed", true,
                   prte_process_info.nodename,
                   (NULL == daemon->node || NULL == daemon->node->name) ? "<unknown>"
                                                                         : daemon->node->name,
                   reason);
    jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    PRTE_UPDATE_EXIT_STATUS((0 < status) ? status : PRTE_ERROR_DEFAULT_EXIT_CODE);
    daemon->state = PRTE_PROC_STATE_FAILED_TO_START;
    if (NULL != jdata) {
        jdata->num_terminated++;
    }
    prte_rml_route_lost(daemon->name.rank);
    PRTE_ACTIVATE_PROC_STATE(&daemon->name, PRTE_PROC_STATE_FAILED_TO_START);
}

/* one line of the helper's output: "<vpid> <event> <value>" */
static void helper_line(tm_launch_t *l, const char *line)
{
    unsigned long vpid;
    char event[32], *reason = NULL;
    int value, n;

    if (3 != sscanf(line, "%lu %31s %d", &vpid, event, &value)) {
        pmix_output_verbose(1, prte_plm_base_framework.framework_output,
                            "%s plm:tm: unexpected line from the helper: %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), line);
        return;
    }
    for (n = 0; n < l->ndaemons; n++) {
        if (l->daemons[n]->name.rank == (pmix_rank_t) vpid) {
            break;
        }
    }
    if (n == l->ndaemons) {
        return;
    }
    pmix_output_verbose(5, prte_plm_base_framework.framework_output,
                        "%s plm:tm: daemon %s %s %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_VPID_PRINT(l->daemons[n]->name.rank), event, value);

    if (0 == strcmp(event, "exited")) {
        pmix_asprintf(&reason, "its task ended (exit value %d) before it reported to the HNP",
                      value);
    } else if (0 == strcmp(event, "spawn-failed")) {
        pmix_asprintf(&reason, "PBS did not start it (TM error %d)", value);
    } else if (0 == strcmp(event, "no-such-host")) {
        reason = strdup("the host is not a node of the job's PBS_NODEFILE");
    } else {
        return;
    }
    daemon_failed(l, n, value, (NULL == reason) ? event : reason);
    free(reason);
}

/* Read what the helper has written, handling each whole line. Returns
 * true once it has seen the end of the helper's output. */
static bool drain_output(tm_launch_t *l)
{
    char *nl;
    ssize_t r;
    size_t used;

    if (0 > l->out_fd) {
        return true;
    }
    for (;;) {
        r = read(l->out_fd, l->out_buf + l->out_len, sizeof(l->out_buf) - 1 - l->out_len);
        if (0 > r) {
            if (EINTR == errno) {
                continue;
            }
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                return false;
            }
            break;
        }
        if (0 == r) {
            break;
        }
        l->out_len += r;
        l->out_buf[l->out_len] = '\0';
        used = 0;
        while (NULL != (nl = strchr(l->out_buf + used, '\n'))) {
            *nl = '\0';
            helper_line(l, l->out_buf + used);
            used = nl - l->out_buf + 1;
        }
        memmove(l->out_buf, l->out_buf + used, l->out_len - used);
        l->out_len -= used;
        if (sizeof(l->out_buf) - 1 == l->out_len) {
            /* no line of the helper's is this long */
            l->out_len = 0;
        }
    }
    stop_output(l);
    return true;
}

static void read_output(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
    (void) drain_output((tm_launch_t *) cbdata);
}

static void stop_output(tm_launch_t *l)
{
    if (l->out_ev_active) {
        prte_event_del(&l->out_ev);
        l->out_ev_active = false;
    }
    if (0 <= l->out_fd) {
        close(l->out_fd);
        l->out_fd = -1;
    }
}

/* Done with the helper's stdin, whether it was all written or not. The
 * buffer holds the DVM key. */
static void finish_input(tm_launch_t *l)
{
    if (l->in_ev_active) {
        prte_event_del(&l->in_ev);
        l->in_ev_active = false;
    }
    if (0 <= l->in_fd) {
        close(l->in_fd);
        l->in_fd = -1;
    }
    if (0 <= l->in_rfd) {
        close(l->in_rfd);
        l->in_rfd = -1;
    }
    if (NULL != l->in_buf) {
        prte_secure_zero(l->in_buf, l->in_len);
        free(l->in_buf);
        l->in_buf = NULL;
    }
}

/* Write as much of the helper's input as its pipe takes, and come back
 * for the rest. We hold the pipe's read end open as well as the helper
 * does, so a write never meets a closed pipe - which would raise SIGPIPE,
 * and prte aborts on that - however soon the helper goes; if it goes
 * first, its reaping ends the write. */
static void write_input(int fd, short args, void *cbdata)
{
    tm_launch_t *l = (tm_launch_t *) cbdata;
    ssize_t w;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    while (l->in_sent < l->in_len) {
        w = write(l->in_fd, l->in_buf + l->in_sent, l->in_len - l->in_sent);
        if (0 > w) {
            if (EINTR == errno) {
                continue;
            }
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                if (!l->in_ev_active) {
                    prte_event_set(prte_event_base, &l->in_ev, l->in_fd,
                                   PRTE_EV_WRITE | PRTE_EV_PERSIST, write_input, l);
                    prte_event_add(&l->in_ev, 0);
                    l->in_ev_active = true;
                }
                return;
            }
            /* the helper will report that its input ended early */
            break;
        }
        l->in_sent += w;
    }
    finish_input(l);
}

/* The helper has ended. Anything it wrote is still to be read; then any
 * daemon it said nothing about is lost with it. */
static void helper_exited(int sd, short flags, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    tm_launch_t *l = (tm_launch_t *) t2->cbdata;
    char *reason = NULL;
    int status, n;
    PRTE_HIDE_UNUSED_PARAMS(sd, flags);

    finish_input(l);
    (void) drain_output(l);

    status = l->helper->exit_code;
    if (WIFEXITED(status)) {
        pmix_asprintf(&reason, "%s exited with status %d without starting it",
                      prte_mca_plm_tm_component.helper, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        pmix_asprintf(&reason, "%s was killed by signal %d without starting it",
                      prte_mca_plm_tm_component.helper, WTERMSIG(status));
    }
    for (n = 0; n < l->ndaemons; n++) {
        daemon_failed(l, n, PRTE_ERROR_DEFAULT_EXIT_CODE,
                      (NULL == reason) ? "the TM helper ended without starting it" : reason);
    }
    free(reason);

    pmix_list_remove_item(&launches, &l->super);
    PMIX_RELEASE(l);
    PMIX_RELEASE(t2);
}

static void helper_child(char **argv, char **env, int in, int out) __prte_attribute_noreturn__;
static void helper_child(char **argv, char **env, int in, int out)
{
    sigset_t sigs;

#if HAVE_SETPGID
    /* keep a Ctrl-C to us from reaching the helper: it has to report on
     * the daemons while we bring them down - see plm/ssh */
    (void) setpgid(0, 0);
#endif
    if (0 > dup2(in, 0) || 0 > dup2(out, 1)) {
        _exit(127);
    }
    pmix_close_open_file_descriptors(-1);

    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);

    execve(argv[0], argv, env);
    fprintf(stderr, "plm:tm: execve of %s failed: %s\n", argv[0], strerror(errno));
    _exit(127);
}

/* Fork the helper for `l`, whose daemons and input are filled in */
static int start_helper(tm_launch_t *l, char **dargv, int vpid_index, char **env)
{
    int inpipe[2] = {-1, -1}, outpipe[2] = {-1, -1};
    char **hargv = NULL, *tmp;
    const char *step;
    pid_t pid;
    int n;

    if (0 != pipe(inpipe)) {
        step = "pipe";
        goto fail;
    }
    if (0 != pipe(outpipe)) {
        step = "pipe";
        goto fail;
    }

    PMIx_Argv_append_nosize(&hargv, prte_mca_plm_tm_component.helper);
    pmix_asprintf(&tmp, "%d", vpid_index);
    PMIx_Argv_append_nosize(&hargv, tmp);
    free(tmp);
    for (n = 0; NULL != dargv[n]; n++) {
        PMIx_Argv_append_nosize(&hargv, dargv[n]);
    }

    pid = fork();
    if (0 > pid) {
        step = "fork";
        goto fail;
    }
    if (0 == pid) {
        helper_child(hargv, env, inpipe[0], outpipe[1]);
    }
#if HAVE_SETPGID
    (void) setpgid(pid, pid);
#endif
    PMIx_Argv_free(hargv);
    close(outpipe[1]);

    l->in_fd = inpipe[1];
    l->in_rfd = inpipe[0];
    l->out_fd = outpipe[0];
    (void) pmix_fd_set_cloexec(l->in_fd);
    (void) pmix_fd_set_cloexec(l->in_rfd);
    (void) pmix_fd_set_cloexec(l->out_fd);
    (void) fcntl(l->in_fd, F_SETFL, fcntl(l->in_fd, F_GETFL) | O_NONBLOCK);
    (void) fcntl(l->out_fd, F_SETFL, fcntl(l->out_fd, F_GETFL) | O_NONBLOCK);

    /* we fork on the progress thread, as the reaper runs, so the helper
     * cannot be reaped before it is registered */
    l->helper = PMIX_NEW(prte_proc_t);
    l->helper->pid = pid;
    PRTE_FLAG_SET(l->helper, PRTE_PROC_FLAG_ALIVE);
    pmix_list_append(&launches, &l->super);
    prte_wait_cb(l->helper, helper_exited, (void *) l);

    prte_event_set(prte_event_base, &l->out_ev, l->out_fd, PRTE_EV_READ | PRTE_EV_PERSIST,
                   read_output, l);
    prte_event_add(&l->out_ev, 0);
    l->out_ev_active = true;

    write_input(l->in_fd, 0, l);
    return PRTE_SUCCESS;

fail:
    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-plm-tm.txt", "helper-failed", true,
                   prte_mca_plm_tm_component.helper, step, strerror(errno));
    for (n = 0; n < 2; n++) {
        if (0 <= inpipe[n]) {
            close(inpipe[n]);
        }
        if (0 <= outpipe[n]) {
            close(outpipe[n]);
        }
    }
    PMIx_Argv_free(hargv);
    return PRTE_ERR_SILENT;
}

/* prepend `dir` to the search path `name` in `env` */
static void prepend_path(char ***env, const char *name, const char *dir)
{
    char *old = NULL, *new = NULL;
    int n;
    size_t len = strlen(name);

    for (n = 0; NULL != *env && NULL != (*env)[n]; n++) {
        if (0 == strncmp((*env)[n], name, len) && '=' == (*env)[n][len]) {
            old = (*env)[n] + len + 1;
            break;
        }
    }
    if (NULL != old && '\0' != *old) {
        pmix_asprintf(&new, "%s:%s", dir, old);
    } else {
        new = strdup(dir);
    }
    if (NULL != new) {
        PMIx_Setenv(name, new, true, env);
        free(new);
    }
}

/* The environment the helper and every daemon run in. Ours, less the
 * PMIX_ and PRTE_ variables, which reach the daemon on its command line
 * and would only confuse a daemon that inherited them - as plm/slurm does
 * for srun. Then what finds the prefix's daemon and libraries. */
static char **daemon_env(const char *prefix_dir, const char *pmix_prefix)
{
    char **env = NULL, *tmp, *base;
    mode_t current_umask;
    int n;

    for (n = 0; NULL != prte_launch_environ && NULL != prte_launch_environ[n]; n++) {
        if (0 == strncmp(prte_launch_environ[n], "PMIX_", 5) ||
            0 == strncmp(prte_launch_environ[n], "PRTE_", 5)) {
            continue;
        }
        PMIx_Argv_append_nosize(&env, prte_launch_environ[n]);
    }
    if (NULL != prefix_dir) {
        base = pmix_basename(prte_install_dirs.bindir);
        pmix_asprintf(&tmp, "%s/%s", prefix_dir, base);
        free(base);
        prepend_path(&env, "PATH", tmp);
        free(tmp);
        base = pmix_basename(prte_install_dirs.libdir);
        pmix_asprintf(&tmp, "%s/%s", prefix_dir, base);
        free(base);
        prepend_path(&env, "LD_LIBRARY_PATH", tmp);
        free(tmp);
        PMIx_Setenv("PRTE_PREFIX", prefix_dir, true, &env);
    }
    if (NULL != pmix_prefix) {
        base = pmix_basename(pmix_pinstall_dirs.libdir);
        pmix_asprintf(&tmp, "%s/%s", pmix_prefix, base);
        free(base);
        prepend_path(&env, "LD_LIBRARY_PATH", tmp);
        free(tmp);
        PMIx_Setenv("PMIX_PREFIX", pmix_prefix, true, &env);
    }

    /* our umask - see the note in prted.c */
    current_umask = umask(0);
    umask(current_umask);
    pmix_asprintf(&tmp, "0%o", current_umask);
    PMIx_Setenv("PRTE_DAEMON_UMASK_VALUE", tmp, true, &env);
    free(tmp);

    return env;
}

/* The helper's input: the key, then each daemon of this launch */
static int helper_input(tm_launch_t *l, prte_job_map_t *map)
{
    char keyhex[PRTE_DVM_KEY_HEXLEN + 1];
    prte_node_t *node;
    const char *host;
    size_t cap, len;
    char *buf;
    int n, k;

    for (n = 0; n < map->nodes->size; n++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, n);
        if (NULL != node && NULL != node->daemon &&
            !PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            l->ndaemons++;
        }
    }
    l->daemons = calloc(l->ndaemons, sizeof(prte_proc_t *));
    l->settled = calloc(l->ndaemons, sizeof(bool));
    if (NULL == l->daemons || NULL == l->settled) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    cap = strlen(PRTE_DVM_KEY_ENVAR) + PRTE_DVM_KEY_HEXLEN + 4;
    k = 0;
    for (n = 0; n < map->nodes->size; n++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, n);
        if (NULL == node || NULL == node->daemon ||
            PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            continue;
        }
        host = (NULL != node->rawname) ? node->rawname : node->name;
        if (NULL == host) {
            return PRTE_ERR_BAD_PARAM;
        }
        cap += strlen(host) + 24;
        PMIX_RETAIN(node->daemon);
        l->daemons[k++] = node->daemon;
    }
    if (NULL == (buf = malloc(cap))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    prte_dvm_key_to_hex(prte_dvm_key, keyhex);
    len = snprintf(buf, cap, "%s=%s\n\n", PRTE_DVM_KEY_ENVAR, keyhex);
    prte_secure_zero(keyhex, sizeof(keyhex));
    for (k = 0; k < l->ndaemons; k++) {
        node = l->daemons[k]->node;
        host = (NULL != node->rawname) ? node->rawname : node->name;
        len += snprintf(buf + len, cap - len, "%u %s\n", (unsigned) l->daemons[k]->name.rank,
                        host);
    }
    l->in_buf = buf;
    l->in_len = len;
    return PRTE_SUCCESS;
}

static void launch_daemons(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *state = (prte_state_caddy_t *) cbdata;
    prte_job_t *daemons;
    prte_job_map_t *map;
    tm_launch_t *l = NULL;
    char **dargv = NULL, **env = NULL, *prefix_dir = NULL, *pmix_prefix = NULL;
    int argc = 0, idx, vpid_index, rc, n;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(state);

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto cleanup;
    }
    if (PRTE_SUCCESS != (rc = prte_plm_base_setup_virtual_machine(state->jdata))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* nothing to start: a map display, or every node already has one */
    map = daemons->map;
    if (PRTE_ATTR_IS_TRUE(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH) ||
        (NULL != map && 0 == map->num_new_daemons)) {
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PMIX_RELEASE(state);
        return;
    }
    if (NULL == map) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        goto cleanup;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:tm: launching %d daemons through %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) map->num_new_daemons,
                         prte_mca_plm_tm_component.helper));

    /* the daemon's own argv; it is never told to daemonize */
    idx = prte_plm_base_setup_prted_cmd(&argc, &dargv);
    if (NULL == dargv || NULL == dargv[idx]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        goto cleanup;
    }
    argc = PMIx_Argv_count(dargv);
    prte_plm_base_prted_append_basic_args(&argc, &dargv, "env", &vpid_index);

    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_PREFIX, (void **) &prefix_dir,
                            PMIX_STRING)) {
        prefix_dir = NULL;
    }
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_PMIX_PREFIX, (void **) &pmix_prefix,
                            PMIX_STRING)) {
        pmix_prefix = NULL;
    }
    env = daemon_env(prefix_dir, pmix_prefix);

    if (0 < pmix_output_get_verbosity(prte_plm_base_framework.framework_output)) {
        char *param = PMIx_Argv_join(dargv, ' ');
        pmix_output(prte_plm_base_framework.framework_output, "%s plm:tm: daemon argv: %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (NULL == param) ? "NULL" : param);
        free(param);
    }

    l = PMIX_NEW(tm_launch_t);
    if (PRTE_SUCCESS != (rc = helper_input(l, map))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    if (PRTE_SUCCESS != start_helper(l, dargv, vpid_index, env)) {
        goto cleanup;
    }
    for (n = 0; n < l->ndaemons; n++) {
        l->daemons[n]->state = PRTE_PROC_STATE_RUNNING;
    }
    /* the launch list owns it now */
    l = NULL;

    state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
    PMIx_Argv_free(dargv);
    PMIx_Argv_free(env);
    free(prefix_dir);
    free(pmix_prefix);
    PMIX_RELEASE(state);
    return;

cleanup:
    if (NULL != l) {
        PMIX_RELEASE(l);
    }
    PMIx_Argv_free(dargv);
    PMIx_Argv_free(env);
    free(prefix_dir);
    free(pmix_prefix);
    PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_FAILED_TO_START);
    PMIX_RELEASE(state);
}

static int plm_tm_terminate_orteds(void)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_exit(PRTE_DAEMON_EXIT_CMD))) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int plm_tm_finalize(void)
{
    tm_launch_t *l;
    int rc;

    if (!initialized) {
        return PRTE_SUCCESS;
    }
    initialized = false;
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
        PRTE_ERROR_LOG(rc);
    }

    /* A helper still running is waiting on daemons that are going, or
     * never got the order to. Nothing is left to read its reports: let it
     * go. The daemons are PBS's tasks and do not depend on it. */
    while (NULL != (l = (tm_launch_t *) pmix_list_remove_first(&launches))) {
        prte_wait_cb_cancel(l->helper);
        if (0 < l->helper->pid) {
            (void) kill(l->helper->pid, SIGTERM);
        }
        PMIX_RELEASE(l);
    }
    PMIX_DESTRUCT(&launches);
    return rc;
}
