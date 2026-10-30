/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
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

#include <limits.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
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
    p->collapsed = NULL;
    p->ncollapsed = 0;
    p->refs = 0;
    p->terminating = false;
}
static void cndes(prte_pmix_server_connection_t *p)
{
    if (NULL != p->members) {
        PMIX_PROC_FREE(p->members, p->nmembers);
    }
    if (NULL != p->collapsed) {
        PMIX_PROC_FREE(p->collapsed, p->ncollapsed);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_server_connection_t,
                    pmix_list_item_t,
                    cncon, cndes);

static void dpcon(prte_pmix_server_departure_t *p)
{
    PMIX_LOAD_NSPACE(p->nspace, NULL);
    p->targets = NULL;
    p->ntargets = 0;
    p->procs = NULL;
    p->nprocs = 0;
    p->cap = 0;
    p->exit_code = 0;
    p->exit_uniform = true;
    p->held = false;
}
static void dpdes(prte_pmix_server_departure_t *p)
{
    if (NULL != p->targets) {
        free(p->targets);
    }
    if (NULL != p->procs) {
        free(p->procs);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_server_departure_t,
                    pmix_list_item_t,
                    dpcon, dpdes);

/* The broadcast, as a pointer so that the unit test can read what a departure
 * puts on the wire without a DVM to carry it. */
int (*prte_pmix_server_connection_xcast)(prte_rml_tag_t tag,
                                         pmix_data_buffer_t *msg) = prte_grpcomm_xcast;

/* the pass-end flush of the departures - see schedule_flush() */
static prte_event_t flush_ev;
static bool flush_queued = false;

void prte_pmix_server_connection_init(void)
{
    PMIX_CONSTRUCT(&prte_pmix_server_globals.connections, pmix_list_t);
    PMIX_CONSTRUCT(&prte_pmix_server_globals.departures, pmix_list_t);
    prte_pmix_server_globals.teardowns = NULL;
    flush_queued = false;
}

void prte_pmix_server_connection_finalize(void)
{
    if (flush_queued) {
        prte_event_del(&flush_ev);
        flush_queued = false;
    }
    PMIX_LIST_DESTRUCT(&prte_pmix_server_globals.departures);
    PMIX_LIST_DESTRUCT(&prte_pmix_server_globals.connections);
    PMIx_Argv_free(prte_pmix_server_globals.teardowns);
    prte_pmix_server_globals.teardowns = NULL;
}

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
 * The comparison of a single entry wildcards on neither half, which is what
 * PMIX_CHECK_PROCID_STRICT is for: PMIX_CHECK_PROCID would call "nspace/0"
 * and "nspace/WILDCARD" a match, and the two are different participant
 * lists.  PMIx itself will not match a connect expressed one way with a
 * connect expressed the other, so neither may we - a disconnect naming a set
 * we never recorded must fail to find it rather than drop somebody else's. */
#define same_proc(a, b) PMIX_CHECK_PROCID_STRICT((a), (b))

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

/* Is rank r of this job a proc whose termination has not been recorded?
 * Not its state: the errmgr writes a failed proc's state before its
 * termination is recorded and its departure reported. */
static bool live_proc(prte_job_t *jdata, int r)
{
    prte_proc_t *proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, r);

    return (NULL != proc && !PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_RECORDED));
}

/* Does this list name every live proc of this job?
 *
 * PRRTE grows an allocation, never a job: every spawn is a new namespace,
 * and a job's table is complete once it is mapped, before anything has been
 * launched to connect.  A terminated proc is never relaunched, so the procs
 * whose termination is not yet recorded are all the job's wildcard can still
 * reach, and naming them is naming the job - which lets the survivors of a
 * job that some procs have left collapse too.  Two kinds of namespace do
 * gain ranks later, and are never collapsed: a tool's, as further tool
 * processes attach, and the daemons', on a grow. */
static bool names_whole_job(const pmix_proc_t *members, size_t nmembers,
                            const pmix_nspace_t nspace)
{
    prte_job_t *jdata;
    bool *seen;
    int nprocs = 0, nseen = 0, size, r;
    size_t i;

    jdata = prte_get_job_data_object(nspace);
    if (NULL == jdata ||
        PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL) ||
        same_nspace(nspace, PRTE_PROC_MY_NAME->nspace)) {
        return false;
    }
    size = jdata->procs->size;
    for (r = 0; r < size; r++) {
        if (live_proc(jdata, r)) {
            ++nprocs;
        }
    }
    if (0 == nprocs) {
        return false;
    }
    seen = (bool *) calloc(size, sizeof(bool));
    if (NULL == seen) {
        return false;
    }
    for (i = 0; i < nmembers; i++) {
        if (!same_nspace(members[i].nspace, nspace) ||
            (pmix_rank_t) size <= members[i].rank) {
            continue;
        }
        r = (int) members[i].rank;
        if (!seen[r] && live_proc(jdata, r)) {
            seen[r] = true;
            ++nseen;
        }
    }
    free(seen);
    return (nseen == nprocs);
}

static bool nspace_listed(char **list, const pmix_nspace_t nspace)
{
    int i;

    for (i = 0; NULL != list && NULL != list[i]; i++) {
        if (same_nspace(list[i], nspace)) {
            return true;
        }
    }
    return false;
}

/* Replace the ranks of any job the list names in full with that job's
 * wildcard.
 *
 * Both forms cover the same procs, and names_whole_job() collapses only a
 * job that cannot gain ranks, so the result covers exactly what the list
 * did.  A departure reads this form of a record: every departure
 * merges and broadcasts the memberships covering it, and a client may name a
 * job by listing its ranks (Open MPI does, from any communicator but
 * MPI_COMM_WORLD).  Returns a list the caller frees, or NULL if out of
 * memory. */
static pmix_proc_t *collapse_whole_jobs(const pmix_proc_t *members, size_t nmembers,
                                        size_t *nout)
{
    pmix_proc_t *out, wild;
    char **whole = NULL, **partial = NULL;
    size_t i, j, n = 0;
    bool have;

    PMIX_PROC_CREATE(out, nmembers);
    if (NULL == out) {
        return NULL;
    }
    for (i = 0; i < nmembers; i++) {
        if (nspace_listed(whole, members[i].nspace)) {
            /* already stands as one wildcard */
            continue;
        }
        if (PMIX_RANK_VALID < members[i].rank ||
            nspace_listed(partial, members[i].nspace)) {
            memcpy(&out[n++], &members[i], sizeof(pmix_proc_t));
            continue;
        }
        if (!names_whole_job(members, nmembers, members[i].nspace)) {
            PMIx_Argv_append_nosize(&partial, members[i].nspace);
            memcpy(&out[n++], &members[i], sizeof(pmix_proc_t));
            continue;
        }
        PMIx_Argv_append_nosize(&whole, members[i].nspace);
        /* the list may have named the job by wildcard as well */
        PMIX_LOAD_PROCID(&wild, members[i].nspace, PMIX_RANK_WILDCARD);
        have = false;
        for (j = 0; j < n; j++) {
            if (same_proc(&out[j], &wild)) {
                have = true;
                break;
            }
        }
        if (!have) {
            memcpy(&out[n++], &wild, sizeof(pmix_proc_t));
        }
    }
    PMIx_Argv_free(whole);
    PMIx_Argv_free(partial);
    *nout = n;
    return out;
}

void prte_pmix_server_connection_record(const pmix_proc_t *members, size_t nmembers)
{
    prte_pmix_server_connection_t *cptr;

    if (!registry_live() || NULL == members || 0 == nmembers) {
        return;
    }

    /* A repeat is the same set connecting again without having disconnected -
     * two MPI_Comm_connect/accept pairs between the same two jobs, say, each
     * an intercommunicator the application will disconnect on its own.  Each
     * is a connection of its own, and the first disconnect must not end the
     * second, so a repeat is counted rather than ignored.  This is called once
     * per connect: the master records where the connect's fence converges. */
    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (same_membership(cptr, members, nmembers)) {
            cptr->refs++;
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s connection across %d participants made again (%u)",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nmembers,
                                cptr->refs);
            return;
        }
    }

    cptr = PMIX_NEW(prte_pmix_server_connection_t);
    cptr->refs = 1;
    cptr->nmembers = nmembers;
    PMIX_PROC_CREATE(cptr->members, cptr->nmembers);
    memcpy(cptr->members, members, nmembers * sizeof(pmix_proc_t));
    cptr->collapsed = collapse_whole_jobs(members, nmembers, &cptr->ncollapsed);
    if (NULL == cptr->collapsed) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        PMIX_RELEASE(cptr);
        return;
    }
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
            /* one disconnect ends one of the connects that made it */
            if (1 < cptr->refs) {
                cptr->refs--;
                continue;
            }
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
 * The targets are a record's collapsed members, wildcards and all, which is
 * what the custom range wants: PMIx delivers to whoever registered for the
 * code and falls within it, so a member that has already gone simply matches
 * nothing.
 *
 * "affected" is what the event is about.  One entry goes as
 * PMIX_EVENT_AFFECTED_PROC, as it always has: a proc for a departure, or
 * <nspace>/WILDCARD for a whole job - one that was terminated because of a
 * departure, or one whose every proc has now gone.  Several entries are
 * departures from one job announced together, and go as the
 * PMIX_EVENT_AFFECTED_PROCS array PMIx defines for exactly that; repeating
 * PMIX_EVENT_AFFECTED_PROC instead would not do, because PMIx keeps only the
 * last of them.
 *
 * The source is the affected proc, or the job's wildcard where there are
 * several - naming ourselves would have our own PMIx server upcall the event
 * straight back to us.  exit_code is passed as -1 where there is none to
 * report. */
static void notify_assemblage(const pmix_proc_t *members, size_t nmembers,
                              pmix_status_t event,
                              const pmix_proc_t *affected, size_t naffected,
                              int exit_code)
{
    pmix_data_range_t range = PMIX_RANGE_CUSTOM;
    pmix_data_array_t darray, aarray;
    pmix_proc_t source;
    pmix_info_t *info;
    size_t ninfo;
    pmix_data_buffer_t pbkt;
    pmix_status_t rc;
    int ret;

    if (0 == naffected) {
        return;
    }
    ninfo = (0 <= exit_code) ? 3 : 2;
    PMIX_INFO_CREATE(info, ninfo);
    if (1 == naffected) {
        PMIX_XFER_PROCID(&source, &affected[0]);
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &source, PMIX_PROC);
    } else {
        PMIX_LOAD_PROCID(&source, affected[0].nspace, PMIX_RANK_WILDCARD);
        aarray.type = PMIX_PROC;
        aarray.array = (pmix_proc_t *) affected;
        aarray.size = naffected;
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROCS, &aarray, PMIX_DATA_ARRAY);
    }
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
        rc = PMIx_Data_pack(NULL, &pbkt, &source, 1, PMIX_PROC);
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
                        "%s notifying connected assemblage of %s affecting %s (%d procs)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(event),
                        PRTE_NAME_PRINT(&source), (int) naffected);

    ret = prte_pmix_server_connection_xcast(PRTE_RML_TAG_NOTIFICATION, &pbkt);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
}

static bool connection_covers(prte_pmix_server_connection_t *cptr,
                              const pmix_proc_t *proc)
{
    size_t n;

    for (n = 0; n < cptr->ncollapsed; n++) {
        if (member_covers(&cptr->collapsed[n], proc)) {
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

/* Does this assemblage name any proc of this job? */
static bool names_job(prte_pmix_server_connection_t *cptr, const pmix_nspace_t nspace)
{
    size_t n;

    for (n = 0; n < cptr->ncollapsed; n++) {
        if (same_nspace(cptr->collapsed[n].nspace, nspace)) {
            return true;
        }
    }
    return false;
}

/* Who is owed the news that this proc has gone - or, with by_job, that its
 * job has.
 *
 * A proc can belong to more than one assemblage - a spawned job is connected
 * to its parent by the spawn itself, and the two may then connect explicitly
 * as well - and its departure is one event, not one per assemblage.  Address
 * it to the union of the memberships, so everyone who was promised the news
 * gets it exactly once.
 *
 * An assemblage already marked terminating is left out.  The sweep in
 * connection_job_failed() is killing every live member of it and has told
 * them why, and each proc coming down with it would otherwise announce itself
 * to an audience that is dying alongside it - one DVM-wide broadcast per
 * proc, to nobody who can use it.
 *
 * Returns the number of targets in *out, which the caller frees. */
static size_t departure_targets(const pmix_proc_t *proc, bool by_job, pmix_proc_t **out)
{
    prte_pmix_server_connection_t *cptr;
    pmix_proc_t *targets = NULL;
    size_t ntargets = 0, cap = 0, i, j;
    bool have;

    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (cptr->terminating) {
            continue;
        }
        if (by_job ? !names_job(cptr, proc->nspace) : !connection_covers(cptr, proc)) {
            continue;
        }
        if (ntargets + cptr->ncollapsed > cap) {
            pmix_proc_t *tmp;
            cap = ntargets + cptr->ncollapsed;
            tmp = (pmix_proc_t *) realloc(targets, cap * sizeof(pmix_proc_t));
            if (NULL == tmp) {
                /* assigning through the same pointer would lose the block we
                 * already have, and the loop below would then index a NULL */
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                break;
            }
            targets = tmp;
        }
        for (i = 0; i < cptr->ncollapsed; i++) {
            have = false;
            for (j = 0; j < ntargets; j++) {
                if (same_proc(&targets[j], &cptr->collapsed[i])) {
                    have = true;
                    break;
                }
            }
            if (!have) {
                memcpy(&targets[ntargets], &cptr->collapsed[i], sizeof(pmix_proc_t));
                ++ntargets;
            }
        }
    }
    *out = targets;
    return ntargets;
}

/* Two target lists address the same members.  Neither holds a repeat - a
 * departure_targets() union never does - so equal sizes and one inclusion are
 * enough. */
static bool same_targets(const pmix_proc_t *a, size_t na, const pmix_proc_t *b, size_t nb)
{
    size_t i, j;
    bool found;

    if (na != nb) {
        return false;
    }
    for (i = 0; i < na; i++) {
        found = false;
        for (j = 0; j < nb; j++) {
            if (same_proc(&a[i], &b[j])) {
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

/* Has the master ordered this job down?  Its departures are then known to be
 * coming, all of them, and are held for the job's end - see
 * prte_pmix_server_connection_teardown(). */
static bool teardown_ordered(const pmix_nspace_t nspace)
{
    int i;

    for (i = 0; NULL != prte_pmix_server_globals.teardowns &&
                NULL != prte_pmix_server_globals.teardowns[i]; i++) {
        if (same_nspace(prte_pmix_server_globals.teardowns[i], nspace)) {
            return true;
        }
    }
    return false;
}

static void teardown_done(const pmix_nspace_t nspace)
{
    int i, argc;

    for (i = 0; NULL != prte_pmix_server_globals.teardowns &&
                NULL != prte_pmix_server_globals.teardowns[i]; i++) {
        if (same_nspace(prte_pmix_server_globals.teardowns[i], nspace)) {
            argc = PMIx_Argv_count(prte_pmix_server_globals.teardowns);
            pmix_argv_delete(&argc, &prte_pmix_server_globals.teardowns, i, 1);
            return;
        }
    }
}

/* Send one set of departures.  A set holding every proc of its job is
 * announced as the job's wildcard, which is what lets a whole job's exit
 * reach its assemblage as one event; the exit code travels only when every
 * proc in the set shares it. */
static void announce(prte_pmix_server_departure_t *dptr)
{
    prte_job_t *jdata;
    pmix_proc_t whole;
    int exit_code = dptr->exit_uniform ? dptr->exit_code : -1;

    jdata = prte_get_job_data_object(dptr->nspace);
    if (1 < dptr->nprocs && NULL != jdata &&
        dptr->nprocs == (size_t) jdata->num_procs) {
        PMIX_LOAD_PROCID(&whole, dptr->nspace, PMIX_RANK_WILDCARD);
        notify_assemblage(dptr->targets, dptr->ntargets, PMIX_ERR_PROC_TERM_WO_SYNC,
                          &whole, 1, exit_code);
    } else {
        notify_assemblage(dptr->targets, dptr->ntargets, PMIX_ERR_PROC_TERM_WO_SYNC,
                          dptr->procs, dptr->nprocs, exit_code);
    }
}

/* Announce the departures that are waiting: with no nspace, every set not
 * held for its job's end; with one, every set of that job, held or not.  Sets
 * go out in the order they were opened. */
static void flush_departures(const char *nspace)
{
    prte_pmix_server_departure_t *dptr, *next;
    bool quiet = (prte_finalizing || prte_dvm_abort_ordered || prte_prteds_term_ordered);

    PMIX_LIST_FOREACH_SAFE(dptr, next, &prte_pmix_server_globals.departures,
                           prte_pmix_server_departure_t) {
        if (NULL == nspace ? dptr->held : !same_nspace(dptr->nspace, nspace)) {
            continue;
        }
        pmix_list_remove_item(&prte_pmix_server_globals.departures, &dptr->super);
        if (!quiet) {
            /* nobody is left to hear it once the DVM is on its way out */
            announce(dptr);
        }
        PMIX_RELEASE(dptr);
    }
}

static void flush_cb(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args, cbdata);

    flush_queued = false;
    if (registry_live()) {
        flush_departures(NULL);
    }
}

/* Announce what has gathered once the current pass of the event loop is done.
 *
 * A daemon reports the normal ends of its procs in one message per job, sent
 * once all of that job's local procs have gone, and the master activates a
 * termination for each proc in it back to back.  An event activated now runs
 * after every one of those, so the whole report reaches the assemblage as one
 * event per audience rather than one per proc - and adds no delay, since it
 * fires as soon as the loop comes round. */
static void schedule_flush(void)
{
    if (flush_queued) {
        return;
    }
    flush_queued = true;
    prte_event_set(prte_event_base, &flush_ev, -1, PRTE_EV_WRITE, flush_cb, NULL);
    prte_event_active(&flush_ev, PRTE_EV_WRITE, 1);
}

void prte_pmix_server_connection_terminated(prte_proc_t *proc)
{
    prte_pmix_server_departure_t *dptr, *match = NULL;
    pmix_proc_t *targets = NULL, *tmp;
    size_t ntargets;
    bool held;

    if (!registry_live() ||
        0 == pmix_list_get_size(&prte_pmix_server_globals.connections)) {
        return;
    }
    /* nobody is left to hear it, and the DVM is on its way out */
    if (prte_finalizing || prte_dvm_abort_ordered || prte_prteds_term_ordered) {
        return;
    }

    ntargets = departure_targets(&proc->name, false, &targets);
    if (0 == ntargets) {
        if (NULL != targets) {
            free(targets);
        }
        return;
    }

    /* Departures are gathered into sets that share both a job and an
     * audience, and each set is announced as one event.  The audience is part
     * of the key, not just the job: two ranks of one job can belong to
     * different assemblages, and an event naming a proc goes to everyone in
     * its range, so folding them together would tell a member about a proc it
     * was never connected to.  A spurious PROC_TERM_WO_SYNC is not harmless -
     * an Open MPI process built without fault tolerance aborts on one.
     *
     * A job the master has ordered down keeps its departures in a set of its
     * own, held until the job has ended. */
    held = teardown_ordered(proc->name.nspace);
    PMIX_LIST_FOREACH(dptr, &prte_pmix_server_globals.departures,
                      prte_pmix_server_departure_t) {
        if (dptr->held == held && same_nspace(dptr->nspace, proc->name.nspace) &&
            same_targets(dptr->targets, dptr->ntargets, targets, ntargets)) {
            match = dptr;
            break;
        }
    }
    if (NULL == match) {
        match = PMIX_NEW(prte_pmix_server_departure_t);
        PMIX_LOAD_NSPACE(match->nspace, proc->name.nspace);
        match->targets = targets;
        match->ntargets = ntargets;
        match->held = held;
        match->exit_code = proc->exit_code;
        targets = NULL;
        pmix_list_append(&prte_pmix_server_globals.departures, &match->super);
    } else {
        free(targets);
    }
    if (match->nprocs == match->cap) {
        tmp = (pmix_proc_t *) realloc(match->procs,
                                      (match->cap + 8) * sizeof(pmix_proc_t));
        if (NULL == tmp) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return;
        }
        match->procs = tmp;
        match->cap += 8;
    }
    PMIX_XFER_PROCID(&match->procs[match->nprocs], &proc->name);
    match->nprocs++;
    if (match->exit_code != proc->exit_code) {
        match->exit_uniform = false;
    }

    if (!held) {
        schedule_flush();
    }
}

void prte_pmix_server_connection_teardown(const pmix_nspace_t nspace)
{
    if (!registry_live() || PMIX_NSPACE_INVALID(nspace) ||
        0 == pmix_list_get_size(&prte_pmix_server_globals.connections) ||
        teardown_ordered(nspace)) {
        return;
    }
    PMIx_Argv_append_nosize(&prte_pmix_server_globals.teardowns, nspace);
}

void prte_pmix_server_connection_job_ended(prte_job_t *jdata)
{
    if (!registry_live() || NULL == jdata) {
        return;
    }
    flush_departures(jdata->nspace);
    teardown_done(jdata->nspace);
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
    /* nor is there one when a tool asked for the job for itself, whatever
     * name it holds - see PRTE_JOB_NO_PARENT_PROC */
    if (PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_NO_PARENT_PROC)) {
        goto done;
    }
    /* PMIX_SPAWN_CHILD_SEP opts the child out of being connected to its
     * parent.  Only an explicit TRUE does so: not having said is not an
     * opt-out, and the PMIx default is that a spawn connects the two. */
    if (PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_CHILD_SEP)) {
        goto done;
    }

    PMIX_XFER_PROCID(&members[0], proxy);
    PMIX_LOAD_PROCID(&members[1], jdata->nspace, PMIX_RANK_WILDCARD);
    prte_pmix_server_connection_record(members, 2);

done:
    PMIX_PROC_RELEASE(proxy);
}

/* The failure that is bringing this job down, and who is owed the news.
 *
 * Every other proc of the job is being killed on the master's orders, and
 * its departure is either held for the job's end or - where the sweep below
 * condemns its assemblages - not announced at all.  So this one cannot wait
 * for its own termination: by then it would be suppressed along with the
 * rest, and it is the only departure naming the job where the trouble began
 * (the sweep's JOB_TERM_WO_SYNC events name the jobs it kills, never this
 * one).
 *
 * The culprit is PRTE_JOB_ABORTED_PROC, which every abort arm of the error
 * manager sets before terminating the job.  One that has already terminated
 * was announced as it went.  A job that failed as a whole has no culprit, and
 * when the sweep will silence its departures it is announced as a job.
 *
 * Who is owed it has to be found before the sweep, which marks every
 * assemblage it condemns as terminating - and departure_targets() passes
 * those by.  It is sent after: see prte_pmix_server_connection_job_failed(). */
typedef struct {
    pmix_status_t event;
    pmix_proc_t affected;
    int exit_code;
    pmix_proc_t *targets;
    size_t ntargets;
} prte_pmix_server_culprit_t;

static void find_culprit(const pmix_nspace_t nspace, prte_pmix_server_culprit_t *culprit)
{
    prte_job_t *jdata;
    prte_proc_t *pptr = NULL;

    culprit->targets = NULL;
    culprit->ntargets = 0;

    jdata = prte_get_job_data_object(nspace);
    if (NULL != jdata &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_ABORTED_PROC, (void **) &pptr,
                           PMIX_POINTER) &&
        NULL != pptr && same_nspace(pptr->name.nspace, nspace)) {
        if (PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_RECORDED)) {
            return;
        }
        culprit->event = PMIX_ERR_PROC_TERM_WO_SYNC;
        PMIX_XFER_PROCID(&culprit->affected, &pptr->name);
        culprit->exit_code = pptr->exit_code;
        culprit->ntargets = departure_targets(&pptr->name, false, &culprit->targets);
    } else if (prte_pmix_server_globals.terminate_connected) {
        culprit->event = PMIX_ERR_JOB_TERM_WO_SYNC;
        PMIX_LOAD_PROCID(&culprit->affected, nspace, PMIX_RANK_WILDCARD);
        culprit->exit_code = -1;
        culprit->ntargets = departure_targets(&culprit->affected, true, &culprit->targets);
    }
}

static void announce_culprit(prte_pmix_server_culprit_t *culprit)
{
    if (0 < culprit->ntargets) {
        notify_assemblage(culprit->targets, culprit->ntargets, culprit->event,
                          &culprit->affected, 1, culprit->exit_code);
    }
    free(culprit->targets);
    culprit->targets = NULL;
    culprit->ntargets = 0;
}

void prte_pmix_server_connection_job_failed(const pmix_nspace_t nspace)
{
    prte_pmix_server_connection_t *cptr;
    prte_pmix_server_culprit_t culprit;
    prte_job_t *jptr;
    pmix_proc_t target;
    pmix_pointer_array_t procs, told;
    prte_proc_t *pobj;
    char **condemned = NULL;
    const char *cause;
    size_t n;
    bool swept;
    int i;

    if (!registry_live() ||
        0 == pmix_list_get_size(&prte_pmix_server_globals.connections)) {
        return;
    }
    if (prte_finalizing || prte_dvm_abort_ordered || prte_prteds_term_ordered) {
        /* everything is going down anyway */
        return;
    }

    /* what was already owed goes out ahead of the news of this failure */
    flush_departures(NULL);
    find_culprit(nspace, &culprit);
    /* and the rest of the job is coming down on our orders */
    prte_pmix_server_connection_teardown(nspace);

    if (!prte_pmix_server_globals.terminate_connected) {
        /* the failed job's own kill is already on its way, and nobody else
         * is to be killed - so the news can go now */
        announce_culprit(&culprit);
        return;
    }

    /* the closure starts with the job the failure actually happened in */
    PMIx_Argv_append_nosize(&condemned, nspace);
    /* and the assemblage to tell of each job it then condemns: told[i] is
     * told of condemned[i + 1], once every kill has been ordered */
    PMIX_CONSTRUCT(&told, pmix_pointer_array_t);
    pmix_pointer_array_init(&told, 4, INT_MAX, 4);

    /* Fate sharing has to be transitive.  Killing a job because a job it was
     * connected to failed is itself the loss of that job, so whatever *it*
     * was connected to has lost a member too and comes down in its turn.
     *
     * The case is ordinary rather than exotic, because a spawn connects the
     * child to the parent *process*: a parent that spawns two children sits
     * in the two assemblages {parent, A} and {parent, B}, and neither of them
     * names the other.  Sweeping only the assemblages that name A takes the
     * parent down and stops there - leaving B running with nothing left to
     * talk to, and the DVM waiting on it forever.  That is a hang, not an
     * untidiness: prterun does not exit while a job it launched is alive, so
     * an MPI_Comm_spawn test whose child aborts wedges the whole run.
     *
     * So sweep to a fixed point.  Each pass condemns the assemblages naming a
     * job that is already coming down, and the pass after it sees the jobs
     * that pass just condemned.  It terminates because an assemblage is
     * marked `terminating` as it is swept and is never revisited, so every
     * pass either marks at least one more or is the last one. */
    do {
        swept = false;
        PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                          prte_pmix_server_connection_t) {
            if (cptr->terminating) {
                continue;
            }
            /* an assemblage is condemned by naming a job that is coming down,
             * and the member that does so is the one to report as the cause -
             * naming the original failure instead would point the user at a
             * job this assemblage was never connected to */
            cause = NULL;
            for (n = 0; n < cptr->ncollapsed; n++) {
                if (nspace_listed(condemned, cptr->collapsed[n].nspace)) {
                    cause = cptr->collapsed[n].nspace;
                    break;
                }
            }
            if (NULL == cause) {
                continue;
            }

            /* the rest of this assemblage is coming down with it, and each of
             * those jobs will report its own procs failing as it goes - none of
             * which should drive this again, or be announced to an assemblage
             * that is being told here why it is going (departure_targets()) */
            cptr->terminating = true;
            swept = true;

            for (n = 0; n < cptr->ncollapsed; n++) {
                /* a namespace can appear more than once in a membership - one
                 * entry per rank - and it is to be killed once.  The condemned
                 * list is what keeps it to one, here and on every later pass */
                if (nspace_listed(condemned, cptr->collapsed[n].nspace)) {
                    continue;
                }
                jptr = prte_get_job_data_object(cptr->collapsed[n].nspace);
                if (NULL == jptr || PRTE_FLAG_TEST(jptr, PRTE_JOB_FLAG_ABORTED) ||
                    PRTE_JOB_STATE_TERMINATED <= jptr->state) {
                    /* Already gone, or already on its way - and deliberately
                     * NOT added to the condemned list.  The closure extends
                     * only through jobs this sweep is itself taking down: a
                     * member that ended on its own terms notified its
                     * assemblages when it went and killed nobody, and reading
                     * it now as a failure would reach through a job that is
                     * no longer there to take down peers that survived it. */
                    continue;
                }
                PMIx_Argv_append_nosize(&condemned, cptr->collapsed[n].nspace);

                /* the user is about to lose a job they did not ask about, so say
                 * why - "connected" is not a property they can see in ps */
                prte_show_help(PRTE_JOB_NSPACE(jptr), "help-prted.txt", "connected-term", true,
                               cause, jptr->nspace);

                pmix_pointer_array_add(&told, cptr);

                PMIX_LOAD_PROCID(&target, jptr->nspace, PMIX_RANK_WILDCARD);
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
    } while (swept);

    /* Only now does anybody hear why.  Every process told is one this sweep
     * is killing, and the news travels down the same daemon tree as its
     * kill, so sent first it could arrive first - and a process that learns
     * its peer failed can act on it, run to the end of its own work, and be
     * reported as having finished when the host was to bring it down.
     * Behind the kills, it reaches nobody who has not already been given
     * their orders.  (Nothing here removes an assemblage, so told[] still
     * points into the list.) */
    announce_culprit(&culprit);
    for (i = 0; NULL != condemned[i] && NULL != condemned[i + 1]; i++) {
        cptr = (prte_pmix_server_connection_t *) pmix_pointer_array_get_item(&told, i);
        PMIX_LOAD_PROCID(&target, condemned[i + 1], PMIX_RANK_WILDCARD);
        notify_assemblage(cptr->collapsed, cptr->ncollapsed, PMIX_ERR_JOB_TERM_WO_SYNC,
                          &target, 1, -1);
    }
    PMIX_DESTRUCT(&told);
    PMIx_Argv_free(condemned);
}

/* How many distinct jobs does this assemblage name - and, with only_live,
 * how many of those other than "except" still exist?  Counting stops at two,
 * which is all purge needs to know. */
static int count_jobs(prte_pmix_server_connection_t *cptr, const char *except, bool only_live)
{
    const char *seen = NULL;
    size_t n;

    for (n = 0; n < cptr->ncollapsed; n++) {
        if (NULL != except && same_nspace(cptr->collapsed[n].nspace, except)) {
            continue;
        }
        if (only_live && NULL == prte_get_job_data_object(cptr->collapsed[n].nspace)) {
            continue;
        }
        if (NULL == seen) {
            seen = cptr->collapsed[n].nspace;
        } else if (!same_nspace(seen, cptr->collapsed[n].nspace)) {
            return 2;
        }
    }
    return (NULL == seen) ? 0 : 1;
}

void prte_pmix_server_connection_purge(const pmix_nspace_t nspace)
{
    prte_pmix_server_connection_t *cptr, *next;
    int live;

    if (!registry_live()) {
        return;
    }

    /* anything of this job's still waiting was owed when its procs went */
    flush_departures(nspace);
    teardown_done(nspace);

    /* An assemblage outlives the departure of one member's job while two or
     * more of its jobs remain: those are still connected to each other, and
     * still owed an event apiece when they go.
     *
     * Once a connection between jobs is down to one, it is over.  Keeping it
     * would announce each of that job's procs to the rest of the job - a
     * DVM-wide broadcast per exit, about the job's own procs, to an audience
     * that never asked to be connected to itself.  The commonest case is a
     * spawn: the record {parent, child} would otherwise stay until the
     * parent's job ended, so every parent proc's exit after its children had
     * gone was still announced.
     *
     * An assemblage within a single job never had another job in it, so it
     * lasts until that job has gone. */
    PMIX_LIST_FOREACH_SAFE(cptr, next, &prte_pmix_server_globals.connections,
                           prte_pmix_server_connection_t) {
        live = count_jobs(cptr, nspace, true);
        if (0 == live || (1 == live && 1 < count_jobs(cptr, NULL, false))) {
            pmix_list_remove_item(&prte_pmix_server_globals.connections, &cptr->super);
            PMIX_RELEASE(cptr);
        }
    }
}
