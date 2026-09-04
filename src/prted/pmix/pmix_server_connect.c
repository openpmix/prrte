/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/*
 * The registry of "connected" assemblages.
 *
 * PMIx_Connect asks the host to treat a set of processes as one application
 * for fault and event-notification purposes.  The obligation that creates is
 * narrow and specific: should any member terminate - or call PMIx_Finalize -
 * without first calling PMIx_Disconnect, the host must generate a
 * PMIX_ERR_PROC_TERM_WO_SYNC event to the assemblage.  A connect that is
 * never followed by a disconnect is therefore not an omission to be tidied
 * away; it is the case the event exists for.
 *
 * The membership is what makes that promise keepable, and it is held here,
 * on the DVM master alone.  The master is the one process that learns of
 * every proc termination in the DVM, including the procs of a node whose
 * daemon has died - which is exactly when an assemblage most wants telling,
 * and exactly when a record held on the dead daemon would have gone with it.
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"

#include "src/grpcomm/grpcomm.h"
#include "src/mca/plm/plm.h"
#include "src/mca/state/state.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/prte_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void cncon(prte_pmix_server_connection_t *p)
{
    p->members = NULL;
    p->nmembers = 0;
    p->terminating = false;
}
static void cndes(prte_pmix_server_connection_t *p)
{
    if (NULL != p->members) {
        PMIX_PROC_FREE(p->members, p->nmembers);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_server_connection_t,
                    pmix_list_item_t,
                    cncon, cndes);

/* The registry exists on the DVM master, and only while its PMIx server is
 * up: every one of these entry points is also reachable from a unit test, and
 * from teardown, where the list has never been constructed. */
static bool registry_live(void)
{
    return (PRTE_PROC_IS_MASTER && prte_pmix_server_globals.initialized);
}

/* Do these two namespaces name the same job?
 *
 * NOT PMIX_CHECK_NSPACE, which answers "true" the moment either side is
 * empty - wildcard semantics that are right for matching a request and wrong
 * for deciding who is in an assemblage, where an empty nspace would put a
 * proc in every one of them.  PMIX_CHECK_NSPACE_STRICT is that question,
 * and it also declines to call two *unset* namespaces the same one: an
 * assemblage member with no namespace is malformed input, not a participant
 * two requests can agree on. */
#define same_nspace(a, b) PMIX_CHECK_NSPACE_STRICT((a), (b))

/* Does this member entry cover this proc?  An entry naming a wildcard rank
 * stands for every proc of that namespace, which is how a connect between
 * two jobs is almost always expressed. */
static bool member_covers(const pmix_proc_t *member, const pmix_proc_t *proc)
{
    if (!same_nspace(member->nspace, proc->nspace)) {
        return false;
    }
    return (PMIX_RANK_WILDCARD == member->rank || member->rank == proc->rank);
}

/* Two assemblages are the same one if they name the same participants.  The
 * order carries no meaning - the PMIx definition says so explicitly - so this
 * compares them as sets.
 *
 * The comparison of a single entry is literal, deliberately: PMIX_CHECK_PROCID
 * would call "nspace/0" and "nspace/WILDCARD" a match, and the two are
 * different participant lists.  PMIx itself will not match a connect
 * expressed one way with a connect expressed the other, so neither may we -
 * a disconnect naming a set we never recorded must fail to find it rather
 * than drop somebody else's. */
static bool same_proc(const pmix_proc_t *a, const pmix_proc_t *b)
{
    return (a->rank == b->rank && same_nspace(a->nspace, b->nspace));
}

static bool same_membership(prte_pmix_server_connection_t *cptr,
                            const pmix_proc_t *members, size_t nmembers)
{
    size_t i, j;
    bool found;

    if (cptr->nmembers != nmembers) {
        return false;
    }
    for (i = 0; i < nmembers; i++) {
        found = false;
        for (j = 0; j < cptr->nmembers; j++) {
            if (same_proc(&members[i], &cptr->members[j])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

void prte_pmix_server_connection_record(const pmix_proc_t *members, size_t nmembers)
{
    prte_pmix_server_connection_t *cptr;

    if (!registry_live() || NULL == members || 0 == nmembers) {
        return;
    }

    /* a repeat is not an error - it just means the same set connected again
     * without having disconnected, which changes nothing */
    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (same_membership(cptr, members, nmembers)) {
            return;
        }
    }

    cptr = PMIX_NEW(prte_pmix_server_connection_t);
    cptr->nmembers = nmembers;
    PMIX_PROC_CREATE(cptr->members, cptr->nmembers);
    memcpy(cptr->members, members, nmembers * sizeof(pmix_proc_t));
    pmix_list_append(&prte_pmix_server_globals.connections, &cptr->super);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s connection recorded across %d participants",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nmembers);
}

/* Does this disconnect request name every member of this assemblage?
 *
 * Dissolving is deliberately more generous than recording.  Recording
 * compares sets exactly, because that is how PMIx itself decides whether two
 * connects are the same operation; but an assemblage exists to be told about
 * departures, and once every process in it has said it is leaving there is
 * nobody left the record serves.  The generosity is one-directional and
 * cannot dissolve something by accident: a request only covers a member it
 * names, and a wildcard member is covered only by a wildcard.
 *
 * The case that needs it is the one PRRTE creates itself.  A spawn connects
 * the child to the parent *process*, while an application that then wants
 * out disconnects the two *jobs* - the shape MPI_Comm_disconnect has - and
 * under an exact-set rule that request would match nothing and the implicit
 * assemblage could never be left. */
static bool membership_covered(prte_pmix_server_connection_t *cptr,
                               const pmix_proc_t *members, size_t nmembers)
{
    size_t i, j;
    bool found;

    for (i = 0; i < cptr->nmembers; i++) {
        found = false;
        for (j = 0; j < nmembers; j++) {
            if (member_covers(&members[j], &cptr->members[i])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

void prte_pmix_server_connection_drop(const pmix_proc_t *members, size_t nmembers)
{
    prte_pmix_server_connection_t *cptr, *next;

    if (!registry_live() || NULL == members || 0 == nmembers) {
        return;
    }

    PMIX_LIST_FOREACH_SAFE(cptr, next, &prte_pmix_server_globals.connections,
                           prte_pmix_server_connection_t) {
        if (membership_covered(cptr, members, nmembers)) {
            pmix_list_remove_item(&prte_pmix_server_globals.connections, &cptr->super);
            PMIX_RELEASE(cptr);
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s connection dropped across %d participants",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nmembers);
        }
    }
}

/* Broadcast an event to the assemblage.
 *
 * The targets are the members as named, wildcards and all, which is what the
 * custom range wants: PMIx delivers to whoever registered for the code and
 * falls within it, so a member that has already gone simply matches nothing.
 * The source is the affected proc rather than ourselves - naming ourselves
 * would have our own PMIx server upcall the event straight back to us.
 *
 * "affected" is a proc for a departure and <nspace>/WILDCARD for a job that
 * was terminated because of one; exit_code is passed as -1 where there is
 * none to report. */
static void notify_assemblage(const pmix_proc_t *members, size_t nmembers,
                              pmix_status_t event,
                              const pmix_proc_t *affected,
                              int exit_code)
{
    pmix_data_range_t range = PMIX_RANGE_CUSTOM;
    pmix_data_array_t darray;
    pmix_info_t *info;
    size_t ninfo;
    pmix_data_buffer_t pbkt;
    pmix_status_t rc;
    int ret;

    ninfo = (0 <= exit_code) ? 3 : 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, (pmix_proc_t *) affected, PMIX_PROC);
    darray.type = PMIX_PROC;
    darray.array = (pmix_proc_t *) members;
    darray.size = nmembers;
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
    if (0 <= exit_code) {
        PMIX_INFO_LOAD(&info[2], PMIX_EXIT_CODE, &exit_code, PMIX_INT);
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* the originator field: an invalid rank says "this did not come from a
     * daemon", so every daemon - ourselves included - hands it to its own
     * PMIx server rather than assuming it has already been delivered */
    rc = PMIx_Data_pack(NULL, &pbkt, &PRTE_NAME_INVALID->rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &pbkt, &event, 1, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &pbkt, (pmix_proc_t *) affected, 1, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO);
    }
    PMIX_INFO_FREE(info, ninfo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return;
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s notifying connected assemblage of %s affecting %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(event),
                        PRTE_NAME_PRINT(affected));

    ret = prte_grpcomm_xcast(PRTE_RML_TAG_NOTIFICATION, &pbkt);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
}

static bool connection_covers(prte_pmix_server_connection_t *cptr,
                              const pmix_proc_t *proc)
{
    size_t n;

    for (n = 0; n < cptr->nmembers; n++) {
        if (member_covers(&cptr->members[n], proc)) {
            return true;
        }
    }
    return false;
}

bool prte_pmix_server_is_connected(const pmix_proc_t *proc)
{
    prte_pmix_server_connection_t *cptr;

    if (!registry_live() || NULL == proc) {
        return false;
    }
    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (connection_covers(cptr, proc)) {
            return true;
        }
    }
    return false;
}

void prte_pmix_server_connection_terminated(prte_proc_t *proc)
{
    prte_pmix_server_connection_t *cptr;
    pmix_proc_t *targets = NULL;
    size_t ntargets = 0, cap = 0, i, j;
    bool have;

    if (!registry_live() ||
        0 == pmix_list_get_size(&prte_pmix_server_globals.connections)) {
        return;
    }
    /* nobody is left to hear it, and the DVM is on its way out */
    if (prte_finalizing || prte_dvm_abort_ordered || prte_prteds_term_ordered) {
        return;
    }

    /* A proc can belong to more than one assemblage - a spawned job is
     * connected to its parent by the spawn itself, and the two may then
     * connect explicitly as well - and its departure is one event, not one
     * per assemblage.  Address it to the union of the memberships, so
     * everyone who was promised the news gets it exactly once. */
    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (!connection_covers(cptr, &proc->name)) {
            continue;
        }
        if (ntargets + cptr->nmembers > cap) {
            pmix_proc_t *tmp;
            cap = ntargets + cptr->nmembers;
            tmp = (pmix_proc_t *) realloc(targets, cap * sizeof(pmix_proc_t));
            if (NULL == tmp) {
                /* assigning through the same pointer would lose the block we
                 * already have, and the loop below would then index a NULL */
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                break;
            }
            targets = tmp;
        }
        for (i = 0; i < cptr->nmembers; i++) {
            have = false;
            for (j = 0; j < ntargets; j++) {
                if (same_proc(&targets[j], &cptr->members[i])) {
                    have = true;
                    break;
                }
            }
            if (!have) {
                memcpy(&targets[ntargets], &cptr->members[i], sizeof(pmix_proc_t));
                ++ntargets;
            }
        }
    }

    if (0 < ntargets) {
        notify_assemblage(targets, ntargets, PMIX_ERR_PROC_TERM_WO_SYNC,
                          &proc->name, proc->exit_code);
    }
    if (NULL != targets) {
        free(targets);
    }
}

/* A job has launched successfully.  If it was spawned by an application
 * process, the PMIx definition connects the two by default - "the parent
 * process is given a copy of the new job's job-level information ... and both
 * the parent and the members of the child job receive notification of errors
 * arising anywhere in their combined assemblage".
 *
 * Whether there is a parent process at all is the whole question here, and
 * three launches that are not a dynamic spawn arrive by the same route.
 *
 * jdata->originator does NOT answer it.  By the time the job reaches us it
 * names the daemon that relayed the request - plm_base_receive overwrites it
 * with the sender, because that is who the response has to go back to.  What
 * survives from the requestor's own daemon is PRTE_JOB_LAUNCH_PROXY, which
 * is set from the originator before the request is packed and is therefore
 * the process that actually asked.
 *
 * That still has to be screened.  A prterun-style launch records the daemon
 * itself as the proxy, and a "prun ./app" records prun's own tool procID - a
 * tool is not a member of a job, has no processes to connect to or to
 * terminate, and is not what "the parent process" means.  So a proxy in our
 * own namespace is rejected, as is one whose job object carries
 * PRTE_JOB_FLAG_TOOL.
 *
 * PMIX_SPAWN_CHILD_SEP is honored as the opt-out: a spawn that asked for the
 * child to be independent of its parent has said in as many words that the
 * two jobs' fates are not to be linked. */
void prte_pmix_server_connection_spawned(prte_job_t *jdata)
{
    prte_job_t *parent;
    pmix_proc_t *proxy = NULL, members[2];
    bool sep = false, *sepptr = &sep;

    if (!registry_live() || NULL == jdata) {
        return;
    }
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY,
                            (void **) &proxy, PMIX_PROC) || NULL == proxy) {
        return;
    }
    if (PMIX_NSPACE_INVALID(proxy->nspace) || PMIX_RANK_INVALID == proxy->rank ||
        same_nspace(proxy->nspace, PRTE_PROC_MY_NAME->nspace)) {
        /* no requestor, or one of our own daemons - either way there is no
         * parent process to connect to */
        goto done;
    }
    parent = prte_get_job_data_object(proxy->nspace);
    if (NULL == parent || PRTE_FLAG_TEST(parent, PRTE_JOB_FLAG_TOOL)) {
        goto done;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CHILD_SEP,
                           (void **) &sepptr, PMIX_BOOL) && sep) {
        goto done;
    }

    PMIX_XFER_PROCID(&members[0], proxy);
    PMIX_LOAD_PROCID(&members[1], jdata->nspace, PMIX_RANK_WILDCARD);
    prte_pmix_server_connection_record(members, 2);

done:
    PMIX_PROC_RELEASE(proxy);
}

/* Does this assemblage name this job at all? */
static bool connection_names_job(prte_pmix_server_connection_t *cptr,
                                 const pmix_nspace_t nspace)
{
    size_t n;

    for (n = 0; n < cptr->nmembers; n++) {
        if (same_nspace(cptr->members[n].nspace, nspace)) {
            return true;
        }
    }
    return false;
}

void prte_pmix_server_connection_job_failed(const pmix_nspace_t nspace)
{
    prte_pmix_server_connection_t *cptr;
    prte_job_t *jptr;
    pmix_proc_t target;
    pmix_pointer_array_t procs;
    prte_proc_t *pobj;
    size_t n, m;
    bool done;
    int i;

    if (!registry_live() || !prte_pmix_server_globals.terminate_connected) {
        return;
    }
    if (prte_finalizing || prte_dvm_abort_ordered || prte_prteds_term_ordered) {
        /* everything is going down anyway */
        return;
    }

    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (cptr->terminating || !connection_names_job(cptr, nspace)) {
            continue;
        }
        /* the rest of this assemblage is coming down with it, and each of
         * those jobs will report its own procs failing as it goes - none of
         * which should drive this again */
        cptr->terminating = true;

        for (n = 0; n < cptr->nmembers; n++) {
            if (same_nspace(cptr->members[n].nspace, nspace)) {
                continue;
            }
            /* a namespace can appear more than once in a membership - one
             * entry per rank - and it is to be killed once */
            done = false;
            for (m = 0; m < n; m++) {
                if (same_nspace(cptr->members[m].nspace, cptr->members[n].nspace)) {
                    done = true;
                    break;
                }
            }
            if (done) {
                continue;
            }
            jptr = prte_get_job_data_object(cptr->members[n].nspace);
            if (NULL == jptr || PRTE_FLAG_TEST(jptr, PRTE_JOB_FLAG_ABORTED) ||
                PRTE_JOB_STATE_TERMINATED <= jptr->state) {
                /* already gone, or already on its way */
                continue;
            }

            /* the user is about to lose a job they did not ask about, so say
             * why - "connected" is not a property they can see in ps */
            prte_show_help("help-prted.txt", "connected-term", true,
                           nspace, jptr->nspace);

            PMIX_LOAD_PROCID(&target, jptr->nspace, PMIX_RANK_WILDCARD);
            notify_assemblage(cptr->members, cptr->nmembers,
                              PMIX_ERR_JOB_TERM_WO_SYNC, &target, -1);

            PMIX_CONSTRUCT(&procs, pmix_pointer_array_t);
            pmix_pointer_array_init(&procs, 1, 1, 1);
            pobj = PMIX_NEW(prte_proc_t);
            PMIX_XFER_PROCID(&pobj->name, &target);
            pmix_pointer_array_add(&procs, pobj);
            prte_plm.terminate_procs(&procs);
            for (i = 0; i < procs.size; i++) {
                pobj = (prte_proc_t *) pmix_pointer_array_get_item(&procs, i);
                if (NULL != pobj) {
                    PMIX_RELEASE(pobj);
                }
            }
            PMIX_DESTRUCT(&procs);
        }
    }
}

void prte_pmix_server_connection_purge(const pmix_nspace_t nspace)
{
    prte_pmix_server_connection_t *cptr, *next;
    size_t n;
    bool live;

    if (!registry_live()) {
        return;
    }

    /* An assemblage outlives the departure of one member's job: the members
     * that remain are still connected to each other, and still owed an event
     * apiece when they go.  It is only once nothing named in it exists any
     * more that the record has no one left to serve. */
    PMIX_LIST_FOREACH_SAFE(cptr, next, &prte_pmix_server_globals.connections,
                           prte_pmix_server_connection_t) {
        live = false;
        for (n = 0; n < cptr->nmembers; n++) {
            if (same_nspace(cptr->members[n].nspace, nspace)) {
                continue;
            }
            if (NULL != prte_get_job_data_object(cptr->members[n].nspace)) {
                live = true;
                break;
            }
        }
        if (!live) {
            pmix_list_remove_item(&prte_pmix_server_globals.connections, &cptr->super);
            PMIX_RELEASE(cptr);
        }
    }
}

/* Tell the DVM master what just connected (or disconnected).
 *
 * Every daemon holding a participant runs this, and the master takes the
 * first report and ignores the rest - deliberately, rather than electing one
 * daemon to speak for the others.  There is no cheap election here that does
 * not depend on some particular daemon still being alive, and the reports are
 * a few dozen bytes each on an operation that has just paid for a DVM-wide
 * collective.  Reporting from the completion of that collective, rather than
 * from its start, is what keeps a connect that failed from being recorded.
 */
static void report_connection(const pmix_proc_t *members, size_t nmembers, bool connected)
{
    pmix_data_buffer_t *buf;
    pmix_status_t rc;
    uint8_t flag;
    int ret;

    if (NULL == members || 0 == nmembers) {
        return;
    }

    if (PRTE_PROC_IS_MASTER) {
        /* no need to send it to ourselves */
        if (connected) {
            prte_pmix_server_connection_record(members, nmembers);
        } else {
            prte_pmix_server_connection_drop(members, nmembers);
        }
        return;
    }

    PMIX_DATA_BUFFER_CREATE(buf);
    flag = connected ? 1 : 0;
    rc = PMIx_Data_pack(NULL, buf, &flag, 1, PMIX_UINT8);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, &nmembers, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, (pmix_proc_t *) members, nmembers, PMIX_PROC);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    PRTE_RML_SEND(ret, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_CONNECTED);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

void prte_pmix_server_connection_report(const pmix_proc_t *members, size_t nmembers)
{
    report_connection(members, nmembers, true);
}

void prte_pmix_server_connection_report_drop(const pmix_proc_t *members, size_t nmembers)
{
    report_connection(members, nmembers, false);
}

/* Receive a report from a daemon.  Runs on the DVM master, on the PRRTE
 * progress thread. */
void prte_pmix_server_connection_recv(int status, pmix_proc_t *sender,
                                      pmix_data_buffer_t *buffer,
                                      prte_rml_tag_t tag, void *cbdata)
{
    pmix_proc_t *members = NULL;
    size_t nmembers;
    uint8_t flag;
    int32_t cnt;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &flag, &cnt, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nmembers, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 == nmembers) {
        return;
    }
    PMIX_PROC_CREATE(members, nmembers);
    if (NULL == members) {
        /* nmembers came off the wire, so this is reachable by a peer asking
         * for more than we can hold rather than only by genuine exhaustion */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    cnt = nmembers;
    rc = PMIx_Data_unpack(NULL, buffer, members, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_PROC_FREE(members, nmembers);
        return;
    }

    if (0 != flag) {
        prte_pmix_server_connection_record(members, nmembers);
    } else {
        prte_pmix_server_connection_drop(members, nmembers);
    }
    PMIX_PROC_FREE(members, nmembers);
}
