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
#include "src/mca/state/state.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void cncon(prte_pmix_server_connection_t *p)
{
    p->members = NULL;
    p->nmembers = 0;
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
 * proc in every one of them. */
static bool same_nspace(const pmix_nspace_t a, const pmix_nspace_t b)
{
    return (0 == strncmp(a, b, PMIX_MAX_NSLEN));
}

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

void prte_pmix_server_connection_drop(const pmix_proc_t *members, size_t nmembers)
{
    prte_pmix_server_connection_t *cptr;

    if (!registry_live() || NULL == members || 0 == nmembers) {
        return;
    }

    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (same_membership(cptr, members, nmembers)) {
            pmix_list_remove_item(&prte_pmix_server_globals.connections, &cptr->super);
            PMIX_RELEASE(cptr);
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s connection dropped across %d participants",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nmembers);
            return;
        }
    }
}

/* Broadcast the event to the assemblage.
 *
 * The targets are the members as named, wildcards and all, which is what the
 * custom range wants: PMIx delivers to whoever registered for the code and
 * falls within it, so a member that has already gone simply matches nothing.
 * The source is the departed proc rather than ourselves - naming ourselves
 * would have our own PMIx server upcall the event straight back to us. */
static void notify_assemblage(prte_pmix_server_connection_t *cptr,
                              prte_proc_t *proc)
{
    pmix_status_t event = PMIX_ERR_PROC_TERM_WO_SYNC;
    pmix_data_range_t range = PMIX_RANGE_CUSTOM;
    pmix_data_array_t darray;
    pmix_info_t *info;
    size_t ninfo;
    pmix_data_buffer_t pbkt;
    pmix_status_t rc;
    int ret;

    ninfo = (0 <= proc->exit_code) ? 3 : 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &proc->name, PMIX_PROC);
    darray.type = PMIX_PROC;
    darray.array = cptr->members;
    darray.size = cptr->nmembers;
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
    if (0 <= proc->exit_code) {
        PMIX_INFO_LOAD(&info[2], PMIX_EXIT_CODE, &proc->exit_code, PMIX_INT);
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
        rc = PMIx_Data_pack(NULL, &pbkt, &proc->name, 1, PMIX_PROC);
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
                        "%s notifying connected assemblage of the loss of %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name));

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

    if (!registry_live() ||
        0 == pmix_list_get_size(&prte_pmix_server_globals.connections)) {
        return;
    }
    /* nobody is left to hear it, and the DVM is on its way out */
    if (prte_finalizing || prte_dvm_abort_ordered || prte_prteds_term_ordered) {
        return;
    }

    PMIX_LIST_FOREACH(cptr, &prte_pmix_server_globals.connections,
                      prte_pmix_server_connection_t) {
        if (connection_covers(cptr, &proc->name)) {
            notify_assemblage(cptr, proc);
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
