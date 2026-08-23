/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * DVM state logging.
 *
 * The DVM controller and each prted can be asked to record every job and/or
 * process state transition it orders, to a file, for post-mortem inspection
 * of how a job progressed.  It is asked for with the state_base_log_jobstate
 * / log_procstate / log_path MCA parameters, and only with those.
 *
 * It is deliberately NOT a prte.conf key.  An early draft of the bootstrap
 * configuration carried ControllerLogJobState / ControllerLogProcState /
 * ControllerLogPath and their PRTEDLog* twins; they were removed from the
 * format.  A key in that file is written once and then applies to every
 * daemon of every DVM the cluster starts, with nobody watching, while the
 * volume of a per-transition record scales with the number of processes
 * launched and lands on the same disk the session directories live on.  A
 * knob with that blast radius has to be asked for by the run that wants it,
 * which is what an MCA parameter is (docs/configuration.rst).
 *
 * This is deliberately NOT the framework's verbose stream.  That stream
 * carries the same three fields, but it is a debugging aid: it goes to
 * stderr, it is all-or-nothing across every framework message, and it is
 * addressed to whoever is watching the terminal.  What is wanted here is an
 * operational record - one file per daemon, on disk, surviving the run,
 * selectable per job/proc, and cheap enough to leave on.
 *
 * Nor does it go through pmix_output's file support.  A pmix_output stream
 * resolves its filename lazily, at the first write, from the *global*
 * output_dir/output_prefix that pmix_output_set_output_file_info() last set
 * - and PRRTE sets those to the proc session directory during ess init.  A
 * stream opened here would therefore land in whatever directory happened to
 * be current when the first transition fired, not in the configured log
 * path.  A plain FILE* is both simpler and correct.
 */

#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/pmix/pmix-internal.h"
#include "src/util/error_strings.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_printf.h"
#include "src/util/prte_show_help.h"
#include "src/util/proc_info.h"

#include "src/runtime/prte_globals.h"

#include "src/mca/state/base/base.h"

/* Resolve the directory the log is written into.
 *
 * An absolute @c path is taken as given.  A relative one is created under
 * @c base - which is the DVM temporary directory, i.e. what DVMTempDir (or
 * the prte_tmpdir_base MCA parameter) set - and so is the absence of a path
 * altogether.  Kept separate from the open below, and taking @c base as an
 * argument rather than reading prte_process_info, so the rule can be tested
 * without a session directory.
 */
int prte_state_base_log_resolve_dir(const char *path, const char *base, char **dir)
{
    *dir = NULL;

    if (NULL != path && '/' == path[0]) {
        *dir = strdup(path);
        return (NULL == *dir) ? PRTE_ERR_OUT_OF_RESOURCE : PRTE_SUCCESS;
    }
    /* everything else is relative to the DVM temporary directory, so we
     * cannot answer without one */
    if (NULL == base || '\0' == base[0]) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL == path || '\0' == path[0]) {
        *dir = strdup(base);
    } else {
        pmix_asprintf(dir, "%s/%s", base, path);
    }
    return (NULL == *dir) ? PRTE_ERR_OUT_OF_RESOURCE : PRTE_SUCCESS;
}

/* Timestamp a record.  The documented content of a log entry is the
 * date/time at which the transition was ordered, so this is wall-clock and
 * human-readable rather than the bare seconds-since-epoch double the
 * verbose stream prints. */
static void stamp(char *buf, size_t len)
{
    struct timeval tv;
    struct tm tm;
    size_t n;

    gettimeofday(&tv, NULL);
    if (NULL == localtime_r(&tv.tv_sec, &tm)) {
        pmix_snprintf(buf, len, "<unknown-time>");
        return;
    }
    n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
    if (0 == n) {
        pmix_snprintf(buf, len, "<unknown-time>");
        return;
    }
    pmix_snprintf(buf + n, len - n, ".%06ld", (long) tv.tv_usec);
}

/* Format a rank into a caller-supplied buffer.
 *
 * Deliberately not PMIX_RANK_PRINT: that returns a pointer into a rotating
 * array of static buffers, and proc-state transitions are ordered from more
 * than one thread - the odls activates RUNNING from a worker thread while
 * the terminate join runs on the progress thread - so two records could be
 * handed the same slot and one of them would name the wrong rank. */
static void rankstr(pmix_rank_t rank, char *buf, size_t len)
{
    switch (rank) {
    case PMIX_RANK_UNDEF:
        pmix_snprintf(buf, len, "UNDEF");
        break;
    case PMIX_RANK_WILDCARD:
        pmix_snprintf(buf, len, "WILDCARD");
        break;
    case PMIX_RANK_INVALID:
        pmix_snprintf(buf, len, "INVALID");
        break;
    default:
        pmix_snprintf(buf, len, "%u", (unsigned) rank);
        break;
    }
}

void prte_state_base_log_open(void)
{
    char *dir = NULL;
    char tsbuf[64];
    int rc;

    if (!prte_state_base.log_jobstate && !prte_state_base.log_procstate) {
        return;
    }
    if (NULL != prte_state_base.log_fp) {
        return;
    }

    rc = prte_state_base_log_resolve_dir(prte_state_base.log_path,
                                         prte_process_info.tmpdir_base, &dir);
    if (PRTE_SUCCESS != rc) {
        prte_show_help("help-state-base.txt", "state-log-no-path", true,
                       prte_process_info.nodename,
                       (NULL == prte_state_base.log_path) ? "(none)" : prte_state_base.log_path);
        goto disable;
    }
    rc = pmix_os_dirpath_create(dir, S_IRWXU);
    if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
        prte_show_help("help-state-base.txt", "state-log-open-failed", true,
                       prte_process_info.nodename, dir, PMIx_Error_string(rc));
        goto disable;
    }

    /* the two roles write distinct files, so a controller co-resident with a
     * daemon does not have them fighting over one */
    pmix_asprintf(&prte_state_base.log_file, "%s/%s-%s-log", dir,
                  PRTE_PROC_IS_MASTER ? "prtectrlr" : "prted",
                  (NULL == prte_process_info.nodename) ? "unknown"
                                                       : prte_process_info.nodename);
    if (NULL == prte_state_base.log_file) {
        goto disable;
    }

    /* append rather than truncate: the file is named for the node and the
     * role, so a second DVM - or a restart of this one - would otherwise
     * erase the record of the first.  The banner below separates the runs. */
    prte_state_base.log_fp = fopen(prte_state_base.log_file, "a");
    if (NULL == prte_state_base.log_fp) {
        prte_show_help("help-state-base.txt", "state-log-open-failed", true,
                       prte_process_info.nodename, prte_state_base.log_file, strerror(errno));
        goto disable;
    }
    /* one write per record (each ends in a newline), so a record is on disk
     * before the transition it names has been acted on - which is the point
     * of keeping the log at all */
    setvbuf(prte_state_base.log_fp, NULL, _IOLBF, 0);
#ifdef HAVE_FCNTL_H
    /* the odls forks application processes; none of them should inherit this */
    (void) fcntl(fileno(prte_state_base.log_fp), F_SETFD, FD_CLOEXEC);
#endif

    stamp(tsbuf, sizeof(tsbuf));
    fprintf(prte_state_base.log_fp, "# %s state log opened by pid %lu on %s (job=%s proc=%s)\n",
            tsbuf, (unsigned long) getpid(),
            (NULL == prte_process_info.nodename) ? "unknown" : prte_process_info.nodename,
            prte_state_base.log_jobstate ? "on" : "off",
            prte_state_base.log_procstate ? "on" : "off");
    free(dir);
    return;

disable:
    /* we could not produce the record we were asked for.  Say so once, then
     * stop pretending - leaving the flags set would cost a check on every
     * transition for a file nobody is writing. */
    prte_state_base.log_jobstate = false;
    prte_state_base.log_procstate = false;
    if (NULL != dir) {
        free(dir);
    }
    if (NULL != prte_state_base.log_file) {
        free(prte_state_base.log_file);
        prte_state_base.log_file = NULL;
    }
}

void prte_state_base_log_close(void)
{
    FILE *fp;

    /* clear the pointer before closing it, not after: a proc-state
     * activation can still arrive from a worker thread while teardown runs,
     * and the emitters gate on this being non-NULL */
    fp = prte_state_base.log_fp;
    prte_state_base.log_fp = NULL;
    if (NULL != fp) {
        fclose(fp);
    }
    if (NULL != prte_state_base.log_file) {
        free(prte_state_base.log_file);
        prte_state_base.log_file = NULL;
    }
}

/* The state name is the last field on the line because several of them
 * contain spaces ("PENDING DAEMON LAUNCH"); everything ahead of it stays
 * column-addressable for a reader that wants to grep or cut. */
void prte_state_base_log_job(prte_job_t *jdata, prte_job_state_t state)
{
    char tsbuf[64];

    if (NULL == prte_state_base.log_fp) {
        return;
    }
    stamp(tsbuf, sizeof(tsbuf));
    /* A job reaches PRTE_JOB_STATE_INIT before anything has stamped its
     * nspace, so the field can be empty as well as the job absent.  Both are
     * written as "-": an empty field would leave the state where a reader
     * splitting on whitespace expects the namespace. */
    fprintf(prte_state_base.log_fp, "%s JOB %s %s\n", tsbuf,
            (NULL == jdata || '\0' == jdata->nspace[0]) ? "-" : jdata->nspace,
            prte_job_state_to_str(state));
}

void prte_state_base_log_proc(const pmix_proc_t *proc, prte_proc_state_t state)
{
    char tsbuf[64];
    char rbuf[24];

    if (NULL == prte_state_base.log_fp) {
        return;
    }
    stamp(tsbuf, sizeof(tsbuf));
    if (NULL == proc) {
        fprintf(prte_state_base.log_fp, "%s PROC - %s\n", tsbuf,
                prte_proc_state_to_str(state));
        return;
    }
    rankstr(proc->rank, rbuf, sizeof(rbuf));
    fprintf(prte_state_base.log_fp, "%s PROC %s:%s %s\n", tsbuf,
            ('\0' == proc->nspace[0]) ? "-" : proc->nspace,
            rbuf, prte_proc_state_to_str(state));
}
