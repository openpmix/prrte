/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Access to a job, or a session, by user and group - see PMIx's
 * docs/security-plan.rst.
 *
 * A job has an owner and may name further users and groups allowed to act
 * on it; root and the user this DVM runs as may act on any job. The rule is
 * PMIx's pmix_server_access_check(), and we apply exactly it by keeping our
 * own copies of what it needs - PMIx keeps its own, on its own thread:
 *
 *  - each job's owner and access list, a pmix_access_t on the job object
 *    (jdata->access), loaded with pmix_server_access_load() from the very
 *    info we register the job with;
 *  - each user, a pmix_user_t on prte_pmix_server_globals.users, its groups
 *    looked up when we first meet it - a job it owns, a tool it runs, or a
 *    request it makes - and registered with PMIx then
 *    (PMIx_server_register_resources with PMIX_USERID).
 *
 * When no job or tool of a user is left, we drop its record and deregister
 * it from PMIx, so the two stay in step. Root and our own user never get a
 * record: they pass before anything is looked up, which keeps a DVM of one
 * user - and prterun - exactly as it was.
 *
 * Everything here runs on the PRRTE progress thread. */

#include "prte_config.h"

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"
#include "src/util/pmix_argv.h"
#include "src/util/proc_info.h"
#if PRTE_PMIX_HAVE_ACCESS_CHECK
#    include "src/server/pmix_server_ops.h"
#    include "src/util/pmix_idname.h"
#endif

#include "src/prted/pmix/pmix_server_internal.h"

bool prte_pmix_server_requester_uid(const pmix_info_t *info, size_t ninfo, uid_t *uid)
{
    uint32_t id;
    size_t n;

    for (n = 0; NULL != info && n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &id, PMIX_UINT32)) {
                return false;
            }
            *uid = (uid_t) id;
            return true;
        }
    }
    return false;
}

bool prte_pmix_server_privileged(uid_t uid)
{
    return (0 == uid || prte_process_info.euid == uid);
}

uid_t prte_pmix_server_job_owner(prte_job_t *jdata)
{
    uint32_t id, *idptr = &id;

    /* the owner every daemon is told of, then the HNP's own record (a
     * tool's job) - and with none known, our own user stands in, as it
     * does in PMIx */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OWNER_UID, (void **) &idptr,
                           PMIX_UINT32)) {
        return (uid_t) id;
    }
    if (PRTE_INVALID_UID != jdata->uid) {
        return jdata->uid;
    }
    return prte_process_info.euid;
}

#if PRTE_PMIX_HAVE_ACCESS_CHECK
typedef struct {
    pmix_info_t *info;
    size_t ninfo;
} user_msg_t;

static void user_reg_cbfunc(pmix_status_t status, void *cbdata)
{
    user_msg_t *msg = (user_msg_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    PMIX_INFO_FREE(msg->info, msg->ninfo);
    free(msg);
}

/* Tell PMIx of a user we have made a record of - and the groups we found
 * it in, so PMIx need not look them up again - or, with a NULL user, that
 * we are done with uid */
static void user_tell_pmix(uid_t uid, const pmix_user_t *user)
{
    user_msg_t *msg;
    pmix_data_array_t da;
    uint32_t id = (uint32_t) uid, *gids = NULL;
    uint16_t n;
    pmix_status_t rc;

    msg = (user_msg_t *) calloc(1, sizeof(user_msg_t));
    if (NULL == msg) {
        return;
    }
    /* no groups found makes no array to send (PMIx will not copy an empty
     * one) - PMIx then looks for itself, and finds the same nothing */
    msg->ninfo = (NULL != user && 0 < user->ngids) ? 2 : 1;
    PMIX_INFO_CREATE(msg->info, msg->ninfo);
    if (NULL == msg->info) {
        free(msg);
        return;
    }
    PMIX_INFO_LOAD(&msg->info[0], PMIX_USERID, &id, PMIX_UINT32);
    if (NULL != user) {
        if (2 == msg->ninfo) {
            gids = (uint32_t *) malloc(user->ngids * sizeof(uint32_t));
            if (NULL == gids) {
                PMIX_INFO_FREE(msg->info, msg->ninfo);
                free(msg);
                return;
            }
            for (n = 0; n < user->ngids; n++) {
                gids[n] = (uint32_t) user->gids[n];
            }
            da.type = PMIX_UINT32;
            da.size = user->ngids;
            da.array = gids;
            PMIX_INFO_LOAD(&msg->info[1], PMIX_GRPID, &da, PMIX_DATA_ARRAY);
            free(gids);
        }
        rc = PMIx_server_register_resources(msg->info, msg->ninfo, user_reg_cbfunc, msg);
    } else {
        rc = PMIx_server_deregister_resources(msg->info, msg->ninfo, user_reg_cbfunc, msg);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_FREE(msg->info, msg->ninfo);
        free(msg);
    }
}

/* our record of uid - made, its groups looked up and PMIx told, if we have
 * none. Never called for root or our own user */
static pmix_user_t *user_get(uid_t uid)
{
    pmix_user_t *user;

    PMIX_LIST_FOREACH (user, &prte_pmix_server_globals.users, pmix_user_t) {
        if (user->uid == uid) {
            return user;
        }
    }
    user = pmix_server_user_create(uid);
    if (NULL != user) {
        (void) pmix_server_user_refresh(user);
        pmix_list_append(&prte_pmix_server_globals.users, &user->super);
        user_tell_pmix(uid, user);
    }
    return user;
}
#endif

void prte_pmix_server_access_user(uid_t uid)
{
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    if (PRTE_INVALID_UID != uid && !prte_pmix_server_privileged(uid)) {
        (void) user_get(uid);
    }
#else
    PRTE_HIDE_UNUSED_PARAMS(uid);
#endif
}

#if PRTE_PMIX_HAVE_ACCESS_CHECK

/* the rule for uid against an owner (and, when valid, one group) alone */
static bool owner_permitted(uid_t uid, uid_t owner, gid_t group)
{
    pmix_access_t acc;
    pmix_user_t *user;
    uint32_t gid = (uint32_t) group;
    pmix_status_t rc;

    user = user_get(uid);
    if (NULL == user) {
        return false;
    }
    pmix_server_access_construct(&acc);
    acc.registered = true;
    acc.source = PMIX_OWNER_REGISTERED;
    acc.uid = owner;
    if (PRTE_INVALID_GID != group) {
        acc.gids = &gid;
        acc.ngids = 1;
    }
    rc = pmix_server_access_check(user, &acc);
    /* the group list was ours, not the struct's */
    acc.gids = NULL;
    acc.ngids = 0;
    pmix_server_access_destruct(&acc);
    return (PMIX_SUCCESS == rc);
}
#endif

bool prte_pmix_server_job_permitted(uid_t uid, prte_job_t *jdata)
{
    uid_t owner;
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    pmix_user_t *user;
#endif

    /* root and our own user, before anything is looked up */
    if (prte_pmix_server_privileged(uid)) {
        return true;
    }
    if (NULL == jdata) {
        /* no job to judge by */
        return false;
    }
    owner = prte_pmix_server_job_owner(jdata);
    if (owner == uid) {
        return true;
    }
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    if (NULL == jdata->access) {
        /* not registered here yet - all we know is its owner */
        return owner_permitted(uid, owner, PRTE_INVALID_GID);
    }
    user = user_get(uid);
    if (NULL == user) {
        return false;
    }
    return (PMIX_SUCCESS == pmix_server_access_check(user, (pmix_access_t *) jdata->access));
#else
    /* a PMIx without the rule: root, our own user and the owner */
    return false;
#endif
}

bool prte_pmix_server_request_permitted(const pmix_proc_t *requestor, const pmix_info_t *info,
                                        size_t ninfo, prte_job_t *jdata)
{
    uid_t uid;

    if (!prte_pmix_server_requester_uid(info, ninfo, &uid)) {
#if PRTE_PMIX_HAVE_REQUESTER_ID
        /* this PMIx names the requester of every request a client or tool
         * makes, replacing any id it supplied - so a request naming none
         * is one we made of our own PMIx server (prterun forwarding its
         * stdin or a signal), and we are not restricted. Anything else
         * naming none is refused */
        return (NULL != requestor && PMIX_CHECK_PROCID(requestor, PRTE_PROC_MY_NAME));
#else
        /* an older PMIx does not say who is asking */
        PRTE_HIDE_UNUSED_PARAMS(requestor);
        return true;
#endif
    }
    return prte_pmix_server_job_permitted(uid, jdata);
}

bool prte_pmix_server_session_permitted(prte_session_t *session, const pmix_proc_t *requestor,
                                        const pmix_info_t *info, size_t ninfo)
{
    uid_t uid, owner;

    if (NULL == session) {
        return false;
    }
    /* the scheduler manages every session - it said so when it connected,
     * and requestor is who PMIx says is asking, not a PMIX_REQUESTOR the
     * request names for itself. Only if there IS one: an empty nspace is
     * PMIX_CHECK_NSPACE's wildcard */
    if (NULL != requestor &&
        !PMIX_NSPACE_INVALID(prte_pmix_server_globals.scheduler.nspace) &&
        PMIX_CHECK_PROCID(&prte_pmix_server_globals.scheduler, requestor)) {
        return true;
    }
    if (!prte_pmix_server_requester_uid(info, ninfo, &uid)) {
#if PRTE_PMIX_HAVE_REQUESTER_ID
        /* a request we made of ourselves - see
         * prte_pmix_server_request_permitted */
        return (NULL != requestor && PMIX_CHECK_PROCID(requestor, PRTE_PROC_MY_NAME));
#else
        /* no requester identity to judge by: the namespace ownership this
         * was decided by before */
        return (NULL != requestor && prte_session_is_owned_by(session, requestor->nspace));
#endif
    }
    if (prte_pmix_server_privileged(uid)) {
        return true;
    }
    /* a session with no recorded owner - the default session among them -
     * is ours */
    owner = (PRTE_INVALID_UID == session->owner_uid) ? prte_process_info.euid
                                                      : session->owner_uid;
    if (owner == uid) {
        return true;
    }
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    /* or a member of the owner's group */
    return owner_permitted(uid, owner, session->owner_gid);
#else
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* a job's access list                                                 */
/* ------------------------------------------------------------------ */

int prte_pmix_server_access_parse(prte_job_t *jdata, bool group, const char *list)
{
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    char **names;
    uint32_t *ids = NULL, *tmp;
    size_t n, nids = 0;
    pmix_data_array_t da, *perms;
    pmix_info_t info;
    pmix_status_t prc;
    int rc = PRTE_SUCCESS;

    if (NULL == list || '\0' == list[0]) {
        return PRTE_ERR_BAD_PARAM;
    }
    /* resolved here, where a name nobody has can still be reported
     * against the command line that gave it */
    names = PMIx_Argv_split(list, ':');
    for (n = 0; NULL != names && NULL != names[n]; n++) {
        tmp = (uint32_t *) realloc(ids, (nids + 1) * sizeof(uint32_t));
        if (NULL == tmp) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            break;
        }
        ids = tmp;
        prc = group ? pmix_util_gid_from_string(names[n], &ids[nids])
                    : pmix_util_uid_from_string(names[n], &ids[nids]);
        if (PMIX_SUCCESS != prc) {
            rc = prte_pmix_convert_status(prc);
            break;
        }
        ++nids;
    }
    PMIx_Argv_free(names);
    if (PRTE_SUCCESS == rc && 0 == nids) {
        rc = PRTE_ERR_BAD_PARAM;
    }
    if (PRTE_SUCCESS != rc) {
        free(ids);
        return rc;
    }
    /* carried with the job's info to every daemon, which registers it with
     * PMIx and keeps its own copy (prte_pmix_server_access_record) */
    PMIX_DATA_ARRAY_CREATE(perms, 1, PMIX_INFO);
    if (NULL == perms) {
        free(ids);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    da.type = PMIX_UINT32;
    da.size = nids;
    da.array = ids;
    PMIX_INFO_LOAD(&((pmix_info_t *) perms->array)[0],
                   group ? PMIX_ACCESS_GRPIDS : PMIX_ACCESS_USERIDS, &da, PMIX_DATA_ARRAY);
    free(ids);
    PMIX_INFO_LOAD(&info, PMIX_ACCESS_PERMISSIONS, perms, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(perms);
    pmix_server_cache_job_info(jdata, &info);
    PMIX_INFO_DESTRUCT(&info);
    return PRTE_SUCCESS;
#else
    PRTE_HIDE_UNUSED_PARAMS(jdata, group, list);
    return PRTE_ERR_NOT_SUPPORTED;
#endif
}

pmix_status_t prte_pmix_server_access_record(prte_job_t *jdata, const pmix_info_t *info,
                                             size_t ninfo)
{
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    pmix_access_t *acc;

    pmix_status_t rc;

    if (NULL == jdata->access) {
        acc = (pmix_access_t *) malloc(sizeof(pmix_access_t));
        if (NULL == acc) {
            return PMIX_ERR_NOMEM;
        }
        pmix_server_access_construct(acc);
        jdata->access = acc;
    }
    rc = pmix_server_access_load((pmix_access_t *) jdata->access, info, ninfo);
    /* the job's owner is a user we now know */
    if (PMIX_SUCCESS == rc && PMIX_OWNER_UNKNOWN != ((pmix_access_t *) jdata->access)->source) {
        prte_pmix_server_access_user(((pmix_access_t *) jdata->access)->uid);
    }
    return rc;
#else
    PRTE_HIDE_UNUSED_PARAMS(jdata, info, ninfo);
    return PMIX_SUCCESS;
#endif
}

void prte_pmix_server_access_release_job(prte_job_t *jdata)
{
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    if (NULL != jdata->access) {
        pmix_server_access_destruct((pmix_access_t *) jdata->access);
        free(jdata->access);
        jdata->access = NULL;
    }
#else
    PRTE_HIDE_UNUSED_PARAMS(jdata);
#endif
}

/* ------------------------------------------------------------------ */
/* users no longer in use                                              */
/* ------------------------------------------------------------------ */

void prte_pmix_server_access_job_done(const pmix_nspace_t nspace)
{
#if PRTE_PMIX_HAVE_ACCESS_CHECK
    prte_job_t *jdata, *other;
    pmix_user_t *user;
    uid_t owner;
    int n;

    jdata = prte_get_job_data_object(nspace);
    if (NULL == jdata) {
        return;
    }
    owner = prte_pmix_server_job_owner(jdata);
    if (prte_pmix_server_privileged(owner)) {
        return;
    }
    /* is anything else of that user's still here? */
    for (n = 0; n < prte_job_data->size; n++) {
        other = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, n);
        if (NULL == other || other == jdata ||
            PMIX_CHECK_NSPACE_STRICT(other->nspace, PRTE_PROC_MY_NAME->nspace)) {
            continue;
        }
        if (prte_pmix_server_job_owner(other) == owner) {
            return;
        }
    }
    /* no - drop our record of the user, and have PMIx drop its own */
    PMIX_LIST_FOREACH (user, &prte_pmix_server_globals.users, pmix_user_t) {
        if (user->uid == owner) {
            pmix_list_remove_item(&prte_pmix_server_globals.users, &user->super);
            PMIX_RELEASE(user);
            break;
        }
    }
    user_tell_pmix(owner, NULL);
#else
    PRTE_HIDE_UNUSED_PARAMS(nspace);
#endif
}

/* ------------------------------------------------------------------ */
/* relaying a request                                                  */
/* ------------------------------------------------------------------ */

void prte_pmix_server_mark_relayed(pmix_info_t *info, size_t ninfo)
{
#if PRTE_PMIX_HAVE_INFO_RELAYED
    size_t n;

    for (n = 0; NULL != info && n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID) || PMIX_CHECK_KEY(&info[n], PMIX_GRPID)) {
            PMIx_Info_relayed(&info[n]);
        }
    }
#else
    PRTE_HIDE_UNUSED_PARAMS(info, ninfo);
#endif
}
