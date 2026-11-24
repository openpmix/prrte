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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/attr.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/proc_info.h"

#include "src/util/prte_dvm_key.h"
#include "src/util/prte_show_help.h"

/* Emit a message we have decided we are the one to deliver.
 *
 * "emit_directly" says the caller knows nobody else will show it, and it has
 * to be said explicitly because pmix_show_help_norender() is not, on a
 * daemon, a local write. PMIx routes a SERVER peer's log through IOF, and
 * IOF then honors PMIX_IOF_LOCAL_OUTPUT - which WE set false for a
 * persistent DVM and for every prted (pmix_server.c), so that a job's stdout
 * never lands on a daemon's own terminal. That is right for application
 * output and wrong for a daemon's own diagnostic, which is not application
 * output and has nowhere else to go when no tool is subscribed. Both callers
 * below reached a point where they had concluded the message was theirs to
 * show; without this it was simply dropped.
 *
 * But it can only be dropped once there is a PMIx to drop it. That routing
 * is PMIx's, and until PMIx has been initialized in this process
 * pmix_show_help_norender() has nowhere to route to and writes the message
 * to stderr itself - so writing it here as well says everything twice.
 *
 * That window is not an obscure one: prte parses and validates its entire
 * command line before prte_init(), and prte_persistent is already true by
 * then (prte.c turns it off again only once it discovers an app was given).
 * So EVERY command-line diagnostic prte and prterun produce was emitted in
 * exactly this window, and every one of them was printed twice.
 *
 * KNOWN GAP: the direct write is not suppressed.  PMIx keys duplicate
 * suppression in plog (pmix_help_check_dups, reached from
 * plog_base_stubs.c), which is downstream of the delivery this path has
 * just established will be dropped - so while a persistent DVM is still
 * starting, the same message relayed by N daemons is printed N times
 * rather than once with a count.  Calling pmix_help_check_dups() from here
 * is NOT the fix: its list is process-global and carries no lock, and it
 * documents that it must be called on the PMIx progress thread, which is
 * not the thread any caller below is on.  What is missing is a PMIx entry
 * point meaning "write this to my own stderr, with suppression", and it
 * belongs in PMIx beside pmix_show_help_norender() rather than as a second
 * suppression table here.
 *
 * And once PMIx is up, handing it the message does not deliver it: PMIx
 * thread-shifts it to its own progress thread and logs it from there, some
 * time later.  While PRRTE is not initialized - starting up, or coming down
 * - that is a message nobody will see.  A startup failure is the common
 * case: prte_init() fails, and prte/prterun return from main without
 * finalizing PMIx, so whatever PMIx had not yet written goes with the
 * process.  "prte_if_exclude eth0" lost its "no usable network interfaces"
 * diagnostic, and the "prte_init failed" one after it, on about three runs
 * in five, leaving only the bare error-log lines.  So in that window write
 * it ourselves, synchronously, and do not hand it to PMIx as well: where
 * PMIx did get to it, it would be printed twice. */
static void deliver_locally(const char *nspace, const char *filename,
                            const char *topic, const char *output,
                            bool emit_directly, const pmix_info_t *dirs, size_t ndirs)
{
    if (0 != PMIx_Initialized()) {
        if (!prte_initialized) {
            fprintf(stderr, "%s", output);
            fflush(stderr);
            return;
        }
        if (emit_directly) {
            fprintf(stderr, "%s", output);
            fflush(stderr);
        }
    }
    pmix_show_help_norender_dirs(nspace, filename, topic, output, dirs, ndirs);
}

/*
 * Who a message about a request is for - see prte_show_help.h.
 */
typedef enum {
    SH_EVERYONE = 0, /* about the DVM itself */
    SH_JOB = 1,      /* about the job named by the message's nspace */
    SH_PROC = 2      /* about a request the proc made */
} sh_about_t;

/* A message names the job it is about (see prte_show_help.h), and the job
 * is what says whom to tell. Our own namespace is the DVM's. */
static sh_about_t about_nspace(const char *nspace)
{
    if (NULL == nspace || '\0' == nspace[0] ||
        PMIX_CHECK_NSPACE(nspace, PRTE_PROC_MY_NAME->nspace)) {
        return SH_EVERYONE;
    }
    return SH_JOB;
}

/* the deepest a chain of spawning jobs is followed */
#define SH_MAX_HOPS 64

/* The tool at the top of a request: start from the job it is about, or
 * from the proc that made it. A job's requester is its launch proxy; when
 * that is a proc of another job - an application that spawned it - that
 * job's requester is taken, up to the tool that started it all. Our own
 * namespace is the top too: under prterun the HNP is the tool. Returns
 * false when the job has no requester, which makes it the DVM's own. */
static bool top_requester(const prte_job_t *jdata, const pmix_proc_t *proc, pmix_proc_t *top)
{
    const prte_job_t *jd = jdata;
    pmix_proc_t *proxy;
    int hops;

    if (NULL != proc) {
        PMIX_XFER_PROCID(top, proc);
        if (PMIX_CHECK_NSPACE(top->nspace, PRTE_PROC_MY_NAME->nspace)) {
            return true;
        }
        jd = prte_get_job_data_object(top->nspace);
        if (NULL == jd || PRTE_FLAG_TEST(jd, PRTE_JOB_FLAG_TOOL)) {
            return true;
        }
    } else if (NULL == jd) {
        return false;
    } else if (PRTE_FLAG_TEST(jd, PRTE_JOB_FLAG_TOOL)) {
        /* a message about a tool's own job is for the tool */
        PMIX_LOAD_PROCID(top, jd->nspace, PMIX_RANK_WILDCARD);
        return true;
    }
    for (hops = 0; hops < SH_MAX_HOPS; hops++) {
        proxy = NULL;
        if (prte_get_attribute((pmix_list_t *) &jd->attributes, PRTE_JOB_LAUNCH_PROXY,
                               (void **) &proxy, PMIX_PROC) &&
            NULL != proxy) {
            PMIX_XFER_PROCID(top, proxy);
            PMIX_PROC_RELEASE(proxy);
        } else if (!PMIX_NSPACE_INVALID(jd->originator.nspace) &&
                   PMIX_RANK_INVALID != jd->originator.rank) {
            /* a spawn whose request is still being parsed: the proxy is
             * set once the request is accepted, but whoever made it is
             * known from the start */
            PMIX_XFER_PROCID(top, &jd->originator);
        } else {
            /* a proc we already stepped to stays the answer: the top of
             * its tree is the job it is in */
            return (NULL != proc || 0 < hops);
        }
        if (PMIX_CHECK_NSPACE(top->nspace, PRTE_PROC_MY_NAME->nspace)) {
            return true;
        }
        jd = prte_get_job_data_object(top->nspace);
        if (NULL == jd || PRTE_FLAG_TEST(jd, PRTE_JOB_FLAG_TOOL)) {
            return true;
        }
    }
    return true;
}

/* Whether a requester is reached through the scheduler: one with no job
 * here - neither a tool connected to us nor one of our jobs - made its
 * request through the scheduler, which knows where it is. */
static bool via_scheduler(const pmix_proc_t *top)
{
    return !PMIX_CHECK_NSPACE(top->nspace, PRTE_PROC_MY_NAME->nspace) &&
           NULL == prte_get_job_data_object(top->nspace) &&
           prte_pmix_server_globals.scheduler_connected &&
           !PMIX_NSPACE_INVALID(prte_pmix_server_globals.scheduler.nspace);
}

/* PMIX_LOG_TARGET for the top requesters, into dirs[0] - each once, and the
 * scheduler in place of any it is reached through - and PMIX_REQUESTOR
 * into dirs[1] when exactly one is reached that way, to say which. Returns
 * how many it loaded: 0 only when out of memory, and then the message goes
 * to everyone, as it always did. */
static size_t load_targets(const pmix_proc_t *tops, size_t ntops, pmix_info_t dirs[2])
{
    pmix_data_array_t darray;
    pmix_proc_t *targets;
    const pmix_proc_t *t, *via = NULL;
    size_t n, k, nt = 0, nvia = 0;

    targets = (pmix_proc_t *) calloc(ntops, sizeof(pmix_proc_t));
    if (NULL == targets) {
        return 0;
    }
    for (n = 0; n < ntops; n++) {
        t = &tops[n];
        if (via_scheduler(t)) {
            if (NULL == via || !PMIX_CHECK_PROCID(via, t)) {
                ++nvia;
            }
            via = t;
            t = &prte_pmix_server_globals.scheduler;
        }
        for (k = 0; k < nt; k++) {
            if (PMIX_CHECK_NSPACE(targets[k].nspace, t->nspace) && targets[k].rank == t->rank) {
                break;
            }
        }
        if (k == nt) {
            PMIX_XFER_PROCID(&targets[nt], t);
            ++nt;
        }
    }
    darray.type = PMIX_PROC;
    darray.size = nt;
    darray.array = targets;
    PMIX_INFO_LOAD(&dirs[0], PMIX_LOG_TARGET, &darray, PMIX_DATA_ARRAY);
    free(targets);
    if (1 == nvia) {
        PMIX_INFO_LOAD(&dirs[1], PMIX_REQUESTOR, via, PMIX_PROC);
        return 2;
    }
    return 1;
}

/* the directives a message is delivered with, on the master: those it was
 * given, and the target of the request it is about. *out is NULL and *nout
 * 0 when it is for everyone; release with PMIX_INFO_FREE */
static void resolve_target(sh_about_t about, const char *nspace, const prte_job_t *jdata,
                           const pmix_proc_t *who, size_t nwho, const pmix_info_t *dirs,
                           size_t ndirs, pmix_info_t **out, size_t *nout)
{
    pmix_info_t tgt[2];
    pmix_proc_t top, *tops = NULL;
    size_t n, ntops = 0, ntgt = 0;

    *out = NULL;
    *nout = 0;
    if (SH_JOB == about) {
        /* the job itself when we were given it: one still being set up -
         * a spawn whose request is being parsed - has neither its
         * namespace nor its place in the registry yet */
        if (NULL == jdata) {
            jdata = prte_get_job_data_object(nspace);
        }
        if (top_requester(jdata, NULL, &top)) {
            ntgt = load_targets(&top, 1, tgt);
        }
    } else if (SH_PROC == about && 0 < nwho) {
        tops = (pmix_proc_t *) calloc(nwho, sizeof(pmix_proc_t));
        if (NULL != tops) {
            for (n = 0; n < nwho; n++) {
                if (top_requester(NULL, &who[n], &tops[ntops])) {
                    ++ntops;
                }
            }
            if (0 < ntops) {
                ntgt = load_targets(tops, ntops, tgt);
            }
            free(tops);
        }
    }
    if (0 == ndirs + ntgt) {
        return;
    }
    PMIX_INFO_CREATE(*out, ndirs + ntgt);
    if (NULL == *out) {
        /* the message still goes - to everyone, as it always did */
        for (n = 0; n < ntgt; n++) {
            PMIX_INFO_DESTRUCT(&tgt[n]);
        }
        return;
    }
    for (n = 0; n < ndirs; n++) {
        PMIX_INFO_XFER(&(*out)[n], &dirs[n]);
    }
    for (n = 0; n < ntgt; n++) {
        /* a move: the array now owns what tgt held */
        memcpy(&(*out)[ndirs + n], &tgt[n], sizeof(pmix_info_t));
    }
    *nout = ndirs + ntgt;
}

/* the entries of a requester info array a message is delivered with -
 * the others say whose permissions to use, which a message has no use for */
static bool is_delivery_key(const pmix_info_t *info)
{
    return PMIX_CHECK_KEY(info, PMIX_LOG_TARGET) || PMIX_CHECK_KEY(info, PMIX_REQUESTOR);
}

/* Carries a rendered message from whatever thread produced it over to the
 * progress thread, which is the only one allowed to touch the RML.
 *
 * This is not hypothetical: the odls dispatches each fork to a worker
 * thread from its own pool, so render_child_msg() - the whole reason a
 * daemon renders help at all - runs off the progress thread whenever a
 * node has enough local procs to engage that pool. The caddy owns all
 * three strings. */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_nspace_t nspace;
    char *filename;
    char *topic;
    char *output;
    uint8_t about;    /* an sh_about_t */
    pmix_proc_t *who; /* the requesters, for SH_PROC */
    size_t nwho;
    pmix_info_t *dirs;
    size_t ndirs;
} prte_show_help_caddy_t;

static void shcon(prte_show_help_caddy_t *p)
{
    PMIX_LOAD_NSPACE(p->nspace, NULL);
    p->filename = NULL;
    p->topic = NULL;
    p->output = NULL;
    p->about = SH_EVERYONE;
    p->who = NULL;
    p->nwho = 0;
    p->dirs = NULL;
    p->ndirs = 0;
}
static void shdes(prte_show_help_caddy_t *p)
{
    if (NULL != p->filename) {
        free(p->filename);
    }
    if (NULL != p->topic) {
        free(p->topic);
    }
    if (NULL != p->output) {
        free(p->output);
    }
    if (NULL != p->dirs) {
        PMIX_INFO_FREE(p->dirs, p->ndirs);
    }
    if (NULL != p->who) {
        free(p->who);
    }
}
static PMIX_CLASS_INSTANCE(prte_show_help_caddy_t, pmix_object_t, shcon, shdes);

/* Runs on the PRRTE progress thread: pack the message and send it to the
 * HNP. Any failure falls back to local delivery rather than dropping a
 * diagnostic on the floor. */
static void relay_to_hnp(int sd, short args, void *cbdata)
{
    prte_show_help_caddy_t *cd = (prte_show_help_caddy_t *) cbdata;
    pmix_data_buffer_t *buf;
    pmix_status_t prc;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_DATA_BUFFER_CREATE(buf);
    /* The job, filename and topic travel with the text: the HNP needs all
     * three to key duplicate suppression, and the last two are what
     * identify the message.  A DVM is single-version by rule, so this
     * wire has no format marker - packer and unpacker change together or
     * not at all (see prte_show_help_recv). */
    prc = PMIx_Data_pack(NULL, buf, &cd->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->filename, 1, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->topic, 1, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->output, 1, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    /* who it is for: the master resolves a job's or a proc's requester
     * from its own data, which knows the tools; directives already
     * resolved go as they are */
    prc = PMIx_Data_pack(NULL, buf, &cd->about, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->nwho, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    if (0 < cd->nwho) {
        prc = PMIx_Data_pack(NULL, buf, cd->who, (int32_t) cd->nwho, PMIX_PROC);
        if (PMIX_SUCCESS != prc) {
            goto fallback;
        }
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    if (0 < cd->ndirs) {
        prc = PMIx_Data_pack(NULL, buf, cd->dirs, (int32_t) cd->ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != prc) {
            goto fallback;
        }
    }

    /* reliable, like the logging relay: a help message most often
     * accompanies a failure, which is exactly when a daemon between us and
     * the HNP may be on its way out */
    PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_SHOW_HELP);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(buf);
        /* the relay is what would have shown it, and it did not go -
         * we have no tools, so this is our own stderr */
        deliver_locally(cd->nspace, cd->filename, cd->topic, cd->output, true, NULL, 0);
    }
    PMIX_RELEASE(cd);
    return;

fallback:
    PMIX_DATA_BUFFER_RELEASE(buf);
    deliver_locally(cd->nspace, cd->filename, cd->topic, cd->output, true, NULL, 0);
    PMIX_RELEASE(cd);
}

static int vdeliver(const pmix_nspace_t nspace, sh_about_t about, const prte_job_t *jdata,
                    const pmix_proc_t *who, size_t nwho, const pmix_info_t *dirs, size_t ndirs,
                    const char *filename, const char *topic, int want_error_header,
                    va_list arglist)
{
    char *output;
    prte_show_help_caddy_t *cd;
    pmix_info_t *rdirs;
    size_t nrdirs, n, k;

    output = pmix_show_help_vstring(filename, topic, want_error_header, arglist);

    /* nothing came back - the topic rendered empty, or was not found */
    if (NULL == output) {
        return PRTE_SUCCESS;
    }

    /* The HNP is where the delivery machinery actually works: its PMIx
     * server is the one holding the tool connection (and under prterun it
     * IS the tool). A tool or an application delivers its own output too.
     * Only a non-master daemon has to hand the message off. */
    if (PRTE_PROC_IS_MASTER || !PRTE_PROC_IS_DAEMON) {
        /* A persistent DVM stays quiet once it is running: a tool holds the
         * connection and IOF puts the message where the user actually is.
         * Until it has started, though, there is no tool and no IOF
         * subscriber, and the terminal it was launched from is the only place
         * anyone can be looking - so a startup failure has to say so itself.
         * prte_dvm_started is latched, which prte_dvm_ready is not: that one
         * goes false again on every grow, and a grow is exactly when these
         * messages fire. A tool or an application is its own endpoint and
         * PMIx writes for it. */
        resolve_target(about, nspace, jdata, who, nwho, dirs, ndirs, &rdirs, &nrdirs);
        deliver_locally(nspace, filename, topic, output,
                        prte_persistent && !prte_dvm_started, rdirs, nrdirs);
        if (NULL != rdirs) {
            PMIX_INFO_FREE(rdirs, nrdirs);
        }
        free(output);
        return PRTE_SUCCESS;
    }

    /* We are a prted. If we do not have an HNP to send to yet - very early
     * in startup, or while coming down - emit locally rather than lose the
     * message: our stderr may well still be connected, and a message that
     * might be seen beats one that certainly will not. Knowing the HNP's
     * name is not enough: until we hold the DVM key no daemon will take a
     * connection from us, so a message about failing to get the key - or
     * anything else that fails before we have it - can go nowhere else */
    if (prte_finalizing || !prte_dvm_key_ready || PMIX_RANK_INVALID == PRTE_PROC_MY_HNP->rank ||
        PMIX_NSPACE_INVALID(PRTE_PROC_MY_HNP->nspace)) {
        /* unconditionally direct: a prted always carries
         * PMIX_IOF_LOCAL_OUTPUT=false, so handing this to PMIx is handing it
         * nowhere, and we have just established there is no HNP to relay to
         * either. Our stderr may well still be connected */
        deliver_locally(nspace, filename, topic, output, true, NULL, 0);
        free(output);
        return PRTE_SUCCESS;
    }

    /* Hand off to the progress thread to do the sending. We may well be on
     * one of the odls spawn-pool threads - render_child_msg() is called
     * from there - and the RML is progress-thread-only state. */
    cd = PMIX_NEW(prte_show_help_caddy_t);
    if (NULL == cd) {
        /* out of memory is exactly when a diagnostic is worth having, and
         * our stderr may still be connected - say it here rather than
         * faulting on the way to saying it somewhere better */
        deliver_locally(nspace, filename, topic, output, true, NULL, 0);
        free(output);
        return PRTE_SUCCESS;
    }
    PMIX_LOAD_NSPACE(cd->nspace, nspace);
    cd->about = (uint8_t) about;
    if (0 < nwho) {
        cd->who = (pmix_proc_t *) calloc(nwho, sizeof(pmix_proc_t));
        if (NULL != cd->who) {
            memcpy(cd->who, who, nwho * sizeof(pmix_proc_t));
            cd->nwho = nwho;
        }
    }
    if (0 < ndirs) {
        PMIX_INFO_CREATE(cd->dirs, ndirs);
        if (NULL != cd->dirs) {
            for (n = 0, k = 0; n < ndirs; n++) {
                PMIX_INFO_XFER(&cd->dirs[k++], &dirs[n]);
            }
            cd->ndirs = ndirs;
        }
    }
    /* A strdup that fails costs the message its label, not the message:
     * both the packer and the HNP's delivery carry a NULL through, and
     * PMIx's suppression then keys it on the empty pair. */
    cd->filename = (NULL == filename) ? NULL : strdup(filename);
    cd->topic = (NULL == topic) ? NULL : strdup(topic);
    cd->output = output; /* the caddy takes it */
    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, relay_to_hnp);
    return PRTE_SUCCESS;
}

int prte_show_help(const pmix_nspace_t nspace, const char *filename,
                   const char *topic, int want_error_header, ...)
{
    va_list arglist;
    int rc;

    va_start(arglist, want_error_header);
    rc = vdeliver(nspace, about_nspace(nspace), NULL, NULL, 0, NULL, 0, filename, topic,
                  want_error_header, arglist);
    va_end(arglist);
    return rc;
}

int prte_show_help_job(const struct prte_job_t *jdata, const char *filename, const char *topic,
                       int want_error_header, ...)
{
    va_list arglist;
    int rc;

    va_start(arglist, want_error_header);
    if (NULL == jdata) {
        rc = vdeliver(PRTE_PROC_MY_NAME->nspace, SH_EVERYONE, NULL, NULL, 0, NULL, 0, filename,
                      topic, want_error_header, arglist);
    } else {
        /* our own namespace is the DVM's job - the daemons' - and any
         * other job, named or not yet, says whom to tell */
        rc = vdeliver(jdata->nspace,
                      PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace) &&
                              '\0' != jdata->nspace[0] ? SH_EVERYONE : SH_JOB,
                      jdata, NULL, 0, NULL, 0, filename, topic, want_error_header, arglist);
    }
    va_end(arglist);
    return rc;
}

int prte_show_help_proc(const pmix_proc_t *requester, const char *filename, const char *topic,
                        int want_error_header, ...)
{
    va_list arglist;
    int rc;

    va_start(arglist, want_error_header);
    if (NULL == requester) {
        rc = vdeliver(PRTE_PROC_MY_NAME->nspace, SH_EVERYONE, NULL, NULL, 0, NULL, 0, filename,
                      topic, want_error_header, arglist);
    } else {
        rc = vdeliver(requester->nspace, SH_PROC, NULL, requester, 1, NULL, 0, filename, topic,
                      want_error_header, arglist);
    }
    va_end(arglist);
    return rc;
}

int prte_show_help_procs(const pmix_proc_t *procs, size_t nprocs, const char *filename,
                         const char *topic, int want_error_header, ...)
{
    va_list arglist;
    int rc;

    pmix_proc_t *jobs = NULL;
    size_t n, k, njobs = 0;

    /* a requester is found from a proc's job, so the jobs are all that
     * matter - and a collective may name each of thousands of procs */
    if (NULL != procs && 0 < nprocs) {
        jobs = (pmix_proc_t *) calloc(nprocs, sizeof(pmix_proc_t));
    }
    for (n = 0; NULL != jobs && n < nprocs; n++) {
        for (k = 0; k < njobs; k++) {
            if (PMIX_CHECK_NSPACE(jobs[k].nspace, procs[n].nspace)) {
                break;
            }
        }
        if (k == njobs) {
            PMIX_LOAD_PROCID(&jobs[njobs], procs[n].nspace, PMIX_RANK_WILDCARD);
            ++njobs;
        }
    }

    va_start(arglist, want_error_header);
    if (0 == njobs) {
        rc = vdeliver(PRTE_PROC_MY_NAME->nspace, SH_EVERYONE, NULL, NULL, 0, NULL, 0, filename,
                      topic, want_error_header, arglist);
    } else {
        rc = vdeliver(jobs[0].nspace, SH_PROC, NULL, jobs, njobs, NULL, 0, filename, topic,
                      want_error_header, arglist);
    }
    va_end(arglist);
    free(jobs);
    return rc;
}

int prte_show_help_info(const pmix_info_t *req, size_t nreq, const char *filename,
                        const char *topic, int want_error_header, ...)
{
    va_list arglist;
    pmix_info_t dirs[2];
    size_t n, ndirs = 0;
    const char *nspace = PRTE_PROC_MY_NAME->nspace;
    int rc;

    /* shallow: vdeliver copies what it keeps */
    for (n = 0; NULL != req && n < nreq; n++) {
        if (is_delivery_key(&req[n]) && ndirs < 2) {
            memcpy(&dirs[ndirs++], &req[n], sizeof(pmix_info_t));
        } else if (PMIX_CHECK_KEY(&req[n], PMIX_NSPACE) && PMIX_STRING == req[n].value.type &&
                   NULL != req[n].value.data.string) {
            nspace = req[n].value.data.string;
        }
    }
    va_start(arglist, want_error_header);
    /* the array names its target; one that names none is about its job */
    rc = vdeliver(nspace, (0 < ndirs) ? SH_EVERYONE : about_nspace(nspace), NULL, NULL, 0,
                  (0 < ndirs) ? dirs : NULL, ndirs, filename, topic, want_error_header, arglist);
    va_end(arglist);
    return rc;
}

static int requester_info(const char *nspace, const pmix_proc_t *top, bool have, uid_t uid,
                          gid_t gid, pmix_info_t **info, size_t *ninfo)
{
    pmix_info_t tgt[2];
    size_t n, ntgt = 0;

    *info = NULL;
    *ninfo = 0;
    if (have) {
        ntgt = load_targets(top, 1, tgt);
    }
    PMIX_INFO_CREATE(*info, 3 + ntgt);
    if (NULL == *info) {
        for (n = 0; n < ntgt; n++) {
            PMIX_INFO_DESTRUCT(&tgt[n]);
        }
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    PMIX_INFO_LOAD(&(*info)[0], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&(*info)[1], PMIX_GRPID, &gid, PMIX_UINT32);
    PMIX_INFO_LOAD(&(*info)[2], PMIX_NSPACE, nspace, PMIX_STRING);
    for (n = 0; n < ntgt; n++) {
        memcpy(&(*info)[3 + n], &tgt[n], sizeof(pmix_info_t));
    }
    *ninfo = 3 + ntgt;
    return PRTE_SUCCESS;
}

int prte_requester_info_job(const struct prte_job_t *jdata, pmix_info_t **info, size_t *ninfo)
{
    pmix_proc_t top;
    uid_t uid;
    gid_t gid;
    bool have;

    if (NULL == jdata) {
        return requester_info(PRTE_PROC_MY_NAME->nspace, NULL, false, prte_process_info.euid,
                              prte_process_info.egid, info, ninfo);
    }
    /* the user a file the job names is read as */
    prte_pmix_server_job_user((prte_job_t *) jdata, &uid, &gid);
    have = top_requester(jdata, NULL, &top);
    return requester_info(jdata->nspace, &top, have, uid, gid, info, ninfo);
}

int prte_requester_info_proc(const pmix_proc_t *requester, uid_t uid, gid_t gid,
                             pmix_info_t **info, size_t *ninfo)
{
    pmix_proc_t top;
    bool have = false;

    if (NULL != requester) {
        have = top_requester(NULL, requester, &top);
    }
    return requester_info((NULL == requester) ? PRTE_PROC_MY_NAME->nspace : requester->nspace,
                          &top, have, uid, gid, info, ninfo);
}

void prte_requester_ids(const pmix_info_t *req, size_t nreq, uid_t *uid, gid_t *gid)
{
    size_t n;

    *uid = prte_process_info.euid;
    *gid = prte_process_info.egid;
    for (n = 0; NULL != req && n < nreq; n++) {
        /* checked, not assumed: a value of another type is no user */
        if (PMIX_CHECK_KEY(&req[n], PMIX_USERID) && PMIX_UINT32 == req[n].value.type) {
            *uid = (uid_t) req[n].value.data.uint32;
        } else if (PMIX_CHECK_KEY(&req[n], PMIX_GRPID) && PMIX_UINT32 == req[n].value.type) {
            *gid = (gid_t) req[n].value.data.uint32;
        }
    }
}

void prte_show_help_recv(int status, pmix_proc_t *sender,
                         pmix_data_buffer_t *buffer,
                         prte_rml_tag_t tag, void *cbdata)
{
    char *filename = NULL, *topic = NULL, *output = NULL;
    pmix_nspace_t nspace;
    int32_t cnt;
    pmix_status_t rc;
    uint8_t about;
    pmix_proc_t *who = NULL;
    pmix_info_t *dirs = NULL, *rdirs = NULL;
    size_t nwho = 0, ndirs = 0, nrdirs = 0;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    /* see the packer in relay_to_hnp: this wire carries no format
     * marker, so the two change together */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nspace, &cnt, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &filename, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &topic, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &output, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &about, &cnt, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nwho, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* checked against what is left to read before anything is sized
     * from it */
    if (0 < nwho) {
        if (!prte_pmix_count_fits(buffer, nwho)) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            nwho = 0;
            goto cleanup;
        }
        who = (pmix_proc_t *) calloc(nwho, sizeof(pmix_proc_t));
        if (NULL == who) {
            nwho = 0;
            goto cleanup;
        }
        cnt = (int32_t) nwho;
        rc = PMIx_Data_unpack(NULL, buffer, who, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* the sender put at most the two delivery directives there */
    if (2 < ndirs) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        ndirs = 0;
        goto cleanup;
    }
    if (0 < ndirs) {
        PMIX_INFO_CREATE(dirs, ndirs);
        if (NULL == dirs) {
            ndirs = 0;
            goto cleanup;
        }
        cnt = (int32_t) ndirs;
        rc = PMIx_Data_unpack(NULL, buffer, dirs, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    if (SH_PROC < about) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        goto cleanup;
    }

    pmix_output_verbose(5, prte_debug_output,
                        "%s showing help relayed by %s [%s:%s]",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender),
                        (NULL == filename) ? "NULL" : filename,
                        (NULL == topic) ? "NULL" : topic);

    /* the text was rendered on the sending daemon - all that is left is to
     * deliver it, and to let PMIx suppress it if an identical message from
     * another node has already been shown */
    if (NULL != output) {
        /* a daemon's message, but ours to show, and under the same rule as
         * one of our own: quiet once a tool is there to receive it, direct
         * while we are still starting and the terminal is all there is - a
         * daemon that fails to launch during DVM startup lands here */
        resolve_target((sh_about_t) about, nspace, NULL, who, nwho, dirs, ndirs, &rdirs, &nrdirs);
        deliver_locally(nspace, filename, topic, output,
                        prte_persistent && !prte_dvm_started, rdirs, nrdirs);
    }

cleanup:
    if (NULL != who) {
        free(who);
    }
    if (NULL != dirs) {
        PMIX_INFO_FREE(dirs, ndirs);
    }
    if (NULL != rdirs) {
        PMIX_INFO_FREE(rdirs, nrdirs);
    }
    if (NULL != filename) {
        free(filename);
    }
    if (NULL != topic) {
        free(topic);
    }
    if (NULL != output) {
        free(output);
    }
}
