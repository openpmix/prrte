/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_UTIL_SHOW_HELP_H
#define PRTE_UTIL_SHOW_HELP_H

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/rml/rml_types.h"

BEGIN_C_DECLS

/*
 * Render a help message and get it in front of the user, from anywhere in
 * a DVM.
 *
 * This is a drop-in replacement for pmix_show_help() with the same
 * signature and the same rendering, and it exists because
 * pmix_show_help() does not work on a prted.
 *
 * Why: pmix_show_help() hands the rendered text to PMIx's plog framework,
 * and plog/stdfd writes its own stderr only when the caller is a PMIx
 * *client or tool*.  A prted is a PMIx *server*, so it takes the other
 * branch, which passes the text to PMIx_server_IOF_deliver() tagged with
 * the daemon's own identity - and nothing has an IOF sink for a daemon's
 * own output, so it is dropped.  The message is rendered perfectly and
 * then thrown away, on every node but the head one.  (It appears to work
 * on the head node only because there the daemon's PMIx server is the one
 * holding the tool connection.)
 *
 * So: on the HNP, on a tool, or in an application, deliver locally exactly
 * as pmix_show_help() would.  On any other daemon, ship the rendered text
 * to the HNP over the RML and let it do the delivering.  Aggregation and
 * duplicate suppression still happen once, on the HNP - which is better
 * than per-node suppression anyway.
 *
 * The nspace names the job the message is ABOUT, and it is what scopes
 * that suppression.  It is not optional and it is not decoration: a DVM
 * runs many jobs, in parallel and one after another for as long as it
 * lives, and suppression keyed on the message alone told only the first
 * job to trip a diagnostic about it - every later one got silence, for
 * days.  Pass the job the message concerns; where the message is about
 * the DVM itself, a tool, or the command line rather than about any
 * job - a parse error, a startup failure - pass
 * PRTE_PROC_MY_NAME->nspace, which is that job.
 *
 * Use this, not pmix_show_help(), for any message that a daemon can
 * produce.  It is safe (and identical) everywhere else.
 */
PRTE_EXPORT int prte_show_help(const pmix_nspace_t nspace,
                               const char *filename, const char *topic,
                               int want_error_header, ...);

/*
 * Who a message is for.
 *
 * On the master, prte_show_help() is logged as the PMIx server's own
 * output, which goes to every tool connected to the DVM. A message caused
 * by a request - a hostfile that would not parse, a mapping that could not
 * be satisfied - is for the tool that made the request and nobody else, so
 * these three say which request it was, and the message is delivered with
 * PMIX_LOG_TARGET naming the tool that made it.
 *
 * prte_show_help() itself does this for the job its nspace names: the
 * target is the job's PRTE_JOB_LAUNCH_PROXY, followed up through any job
 * that spawned it to the tool at the top, and a tool's own namespace is
 * that tool. On a prted the master resolves it, from its own job data. Our
 * own namespace - the DVM's, and so the daemons' - is everyone. These say
 * which request it was when the nspace cannot:
 *
 *  - prte_show_help_job(): prte_show_help() for jdata, or for the DVM when
 *    jdata is NULL.
 *  - prte_show_help_proc(): a request made by requester, with no job - an
 *    allocation or a session request. The message is scoped to the
 *    requester's job for duplicate suppression.
 *  - prte_show_help_procs(): a collective among procs - a fence, a group -
 *    which may span jobs, and so tools: each of their requesters is told.
 *  - prte_show_help_info(): a request described by a requester info array
 *    (prte_requester_info_job()/_proc()), for the code - a file parser,
 *    say - that is handed the requester rather than the request. NULL, 0
 *    means the DVM itself, as does a job or proc with no requester.
 *
 * Each scopes duplicate suppression to the job the request is about, or
 * the requester's own: two tools that make the same mistake are each told.
 *
 * A requester this DVM has no connection for - the scheduler made the
 * request on the requester's behalf - is reached through the scheduler,
 * with PMIX_REQUESTOR naming it. A requester that has gone is reached
 * nowhere but the master's own stderr: the message is never handed to
 * anybody else instead.
 *
 * Messages about the DVM itself - a daemon lost, a startup failure, the
 * command line of a tool - stay with prte_show_help() and go to everyone.
 */
struct prte_job_t;
PRTE_EXPORT int prte_show_help_job(const struct prte_job_t *jdata, const char *filename,
                                   const char *topic, int want_error_header, ...);
PRTE_EXPORT int prte_show_help_proc(const pmix_proc_t *requester, const char *filename,
                                    const char *topic, int want_error_header, ...);
PRTE_EXPORT int prte_show_help_procs(const pmix_proc_t *procs, size_t nprocs,
                                     const char *filename, const char *topic,
                                     int want_error_header, ...);
PRTE_EXPORT int prte_show_help_info(const pmix_info_t *req, size_t nreq, const char *filename,
                                    const char *topic, int want_error_header, ...);

/*
 * The requester of a request, as an info array: PMIX_USERID and PMIX_GRPID
 * - whose permissions a file it names is opened with - PMIX_NSPACE, the job
 * the request is about, and the PMIX_LOG_TARGET (and any PMIX_REQUESTOR)
 * that its messages go to. For the
 * code that acts for a request without holding it. The caller releases the
 * array with PMIX_INFO_FREE. _job() takes the user and group from the job;
 * _proc() is given them. Either returns PRTE_SUCCESS with *ninfo possibly
 * 0 for the DVM itself, or PRTE_ERR_OUT_OF_RESOURCE.
 */
PRTE_EXPORT int prte_requester_info_job(const struct prte_job_t *jdata, pmix_info_t **info,
                                        size_t *ninfo);
PRTE_EXPORT int prte_requester_info_proc(const pmix_proc_t *requester, uid_t uid, gid_t gid,
                                         pmix_info_t **info, size_t *ninfo);
/* the user and group such an array names; our own where it names none */
PRTE_EXPORT void prte_requester_ids(const pmix_info_t *req, size_t nreq, uid_t *uid, gid_t *gid);

/* HNP-side receiver for the relayed text. Registered by the PMIx server
 * setup alongside the other HNP-only recvs. */
PRTE_EXPORT void prte_show_help_recv(int status, pmix_proc_t *sender,
                                     pmix_data_buffer_t *buffer,
                                     prte_rml_tag_t tag, void *cbdata);

END_C_DECLS

#endif /* PRTE_UTIL_SHOW_HELP_H */
