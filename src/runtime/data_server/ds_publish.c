/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2016 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2025      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/attr.h"
#include "src/util/name_fns.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/data_server/ds.h"

/* Load a uid/gid list out of an access-permission directive.  The Standard
 * types these as a pmix_data_array_t of the ids; a single id given as a
 * plain PMIX_UINT32 is accepted too, since refusing it would only push
 * publishers into building a one-element array.  Anything else is a
 * restriction we cannot read, and a restriction we cannot read has to fail
 * the publish - never be silently dropped, which would store the data with
 * no restriction at all. */
static pmix_status_t load_ids(const pmix_value_t *val, uint32_t **ids, size_t *nids)
{
    pmix_data_array_t *array;
    uint32_t *dst;

    if (PMIX_UINT32 == val->type) {
        dst = (uint32_t *) malloc(sizeof(uint32_t));
        if (NULL == dst) {
            return PMIX_ERR_NOMEM;
        }
        dst[0] = val->data.uint32;
        if (NULL != *ids) {
            free(*ids);
        }
        *ids = dst;
        *nids = 1;
        return PMIX_SUCCESS;
    }

    if (PMIX_DATA_ARRAY != val->type) {
        return PMIX_ERR_BAD_PARAM;
    }
    array = val->data.darray;
    if (NULL == array || NULL == array->array || 0 == array->size ||
        PMIX_UINT32 != array->type) {
        return PMIX_ERR_BAD_PARAM;
    }
    dst = (uint32_t *) malloc(array->size * sizeof(uint32_t));
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(dst, array->array, array->size * sizeof(uint32_t));
    if (NULL != *ids) {
        free(*ids);
    }
    *ids = dst;
    *nids = array->size;
    return PMIX_SUCCESS;
}

/* Unpack a PMIX_ACCESS_PERMISSIONS directive - an array of pmix_info_t
 * naming the permissions - onto the data object. */
static pmix_status_t load_permissions(const pmix_value_t *val,
                                      prte_data_object_t *data)
{
    pmix_data_array_t *array;
    pmix_info_t *iptr;
    pmix_status_t rc;
    size_t n;

    if (PMIX_DATA_ARRAY != val->type) {
        return PMIX_ERR_BAD_PARAM;
    }
    array = val->data.darray;
    if (NULL == array || NULL == array->array || 0 == array->size ||
        PMIX_INFO != array->type) {
        return PMIX_ERR_BAD_PARAM;
    }
    iptr = (pmix_info_t *) array->array;
    for (n = 0; n < array->size; n++) {
        if (PMIx_Check_key(iptr[n].key, PMIX_ACCESS_USERIDS)) {
            rc = load_ids(&iptr[n].value, &data->auids, &data->nauids);
        } else if (PMIx_Check_key(iptr[n].key, PMIX_ACCESS_GRPIDS)) {
            rc = load_ids(&iptr[n].value, &data->agids, &data->nagids);
        } else {
            /* a permission we do not know how to enforce */
            rc = PMIX_ERR_BAD_PARAM;
        }
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* Is a stored item on the SAME DATA RANGE as this publication?
 *
 * The Standard permits duplicate keys on different ranges and requires
 * PMIX_ERR_DUPLICATE_KEY for a duplicate on the same one.  A range is a
 * SET OF PROCESSES, and the pmix_data_range_t is only that set's name as
 * seen from the publisher: PMIX_RANGE_NAMESPACE published by two processes
 * of different namespaces names two disjoint sets, not one, and refusing
 * the second of those would refuse a publish the Standard permits.
 *
 * So "same data range" is the range word matching AND the stored item being
 * one this publisher could itself have looked up.  For NAMESPACE, LOCAL and
 * PROC_LOCAL that second test is exactly set equality; for SESSION, GLOBAL
 * and RM it is trivially true, which is what makes those the cases that do
 * collide.  Bringing the access check in with it separates two users'
 * identically-keyed items for the same reason: neither can see the other's,
 * so neither can shadow it.
 *
 * The req is the PUBLISHER cast as a requestor - the publisher's range is
 * carried in it too, but only so a CUSTOM publication is asked the right
 * question; the range word is compared before either check runs. */
static bool same_data_range(prte_data_req_t *rq, prte_data_object_t *data,
                            pmix_data_range_t range)
{
    if (range != data->range) {
        return false;
    }
    if (PMIX_SUCCESS != prte_data_server_check_access(rq, data)) {
        return false;
    }
    return (PMIX_SUCCESS == prte_data_server_check_range(rq, data));
}

/* Does this publication collide with what is already stored?
 *
 * Counts the colliding keys and reports whether any of them belongs to a
 * DIFFERENT publisher, which is what decides between "you may replace your
 * own" and "that name is taken".  Nothing is modified here: the decision
 * has to be complete before anything is removed, so that a publish which
 * ends up refused leaves the store exactly as it found it. */
/* Record which application and session the publisher belongs to.
 *
 * Every process that runs a data server holds the job objects it needs for
 * this: the master holds them all, and a daemon holds the ones whose procs
 * it hosts - which is exactly the set that can publish into its own store.
 * The session id is a job attribute set PRTE_ATTR_GLOBAL where it is set at
 * all, so it reaches the daemons in the launch message. */
static void resolve_publisher(prte_data_object_t *data)
{
    prte_job_t *jdata;
    prte_proc_t *proc;
    uint32_t *ui32ptr;

    jdata = prte_get_job_data_object(data->owner.nspace);
    if (NULL == jdata) {
        return;
    }
    proc = prte_get_proc_object(&data->owner);
    if (NULL != proc) {
        data->app_idx = (uint32_t) proc->app_idx;
    }
    ui32ptr = &data->session_id;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_SESSION_ID,
                            (void **) &ui32ptr, PMIX_UINT32)) {
        /* no allocation of its own: the default session, which ends when
         * the DVM does and therefore never needs a purge of its own */
        data->session_id = UINT32_MAX;
    }
}

static size_t count_duplicates(prte_data_req_t *rq, prte_data_object_t *data,
                               bool *foreign)
{
    prte_data_object_t *dptr;
    prte_info_item_t *mine, *theirs;
    size_t ndups = 0;
    int k;

    *foreign = false;
    for (k = 0; k < prte_data_store.store.size; k++) {
        dptr = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
        if (NULL == dptr) {
            continue;
        }
        if (!same_data_range(rq, dptr, data->range)) {
            continue;
        }
        PMIX_LIST_FOREACH(mine, &data->info, prte_info_item_t) {
            PMIX_LIST_FOREACH(theirs, &dptr->info, prte_info_item_t) {
                if (!PMIx_Check_key(mine->info.key, theirs->info.key)) {
                    continue;
                }
                pmix_output_verbose(1, prte_data_store.output,
                                    "%s data server: %s is already published on %s by %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    mine->info.key,
                                    PMIx_Data_range_string(data->range),
                                    PMIX_NAME_PRINT(&dptr->owner));
                ndups++;
                /* Whose name is this?  Ownership is the publishing USER, so
                 * a later job of the same user is republishing rather than
                 * seizing - which is the point: unpublish-then-publish is
                 * open to it either way, and refusing the one-step form
                 * would only make the same outcome take two calls. */
                if (!prte_data_server_owns(rq->uid, rq->gid, dptr)) {
                    *foreign = true;
                }
            }
        }
    }
    return ndups;
}

/* Take back the publisher's own prior publication of these keys.  Only the
 * republished keys go: an object holding others keeps them, and one left
 * empty leaves the store, exactly as an unpublish of those keys would have
 * done.  Called only once count_duplicates() has established that every
 * collision is this publisher's own. */
static void drop_prior(prte_data_req_t *rq, prte_data_object_t *data)
{
    prte_data_object_t *dptr;
    prte_info_item_t *mine, *theirs, *tnext;
    int k;

    for (k = 0; k < prte_data_store.store.size; k++) {
        dptr = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
        if (NULL == dptr) {
            continue;
        }
        if (!prte_data_server_owns(rq->uid, rq->gid, dptr)) {
            continue;
        }
        if (!same_data_range(rq, dptr, data->range)) {
            continue;
        }
        PMIX_LIST_FOREACH(mine, &data->info, prte_info_item_t) {
            PMIX_LIST_FOREACH_SAFE(theirs, tnext, &dptr->info, prte_info_item_t) {
                if (PMIx_Check_key(mine->info.key, theirs->info.key)) {
                    pmix_list_remove_item(&dptr->info, &theirs->super);
                    PMIX_RELEASE(theirs);
                }
            }
        }
        if (0 == pmix_list_get_size(&dptr->info)) {
            prte_ds_drop(dptr);
        } else {
            /* it kept some keys and lost others: recharge the difference */
            prte_ds_charge(dptr);
        }
    }
}

pmix_status_t prte_ds_publish(pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer,
                              pmix_data_buffer_t *answer)
{
    uint8_t command;
    int32_t count;
    prte_data_object_t *data;
    pmix_data_buffer_t *reply;
    int rc;
    size_t ninfo;
    uint32_t i;
    bool complete_resolved, found;
    prte_data_req_t *req, *rqnext;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    pmix_status_t ret;
    prte_info_item_t *ds1, *ds2, *ds3;
    size_t n, ndups;
    pmix_info_t *info;
    char **cache;
    pmix_list_t answers;
    prte_data_req_t rq;
    bool replace = false, foreign;

    data = PMIX_NEW(prte_data_object_t);
    memcpy(&data->proxy, sender, sizeof(pmix_proc_t));

    /* unpack the publisher */
    count = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &data->owner, &count, PMIX_PROC);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        return ret;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: publishing data from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), data->owner.nspace,
                        data->owner.rank);

    /* unpack the number of infos and directives they sent */
    count = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        return ret;
    }

    /* if it isn't at least one, then that's an error */
    if (1 > ninfo) {
        ret = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        rc = PRTE_ERR_UNPACK_FAILURE;
        return rc;
    }

    /* create the space */
    PMIX_INFO_CREATE(info, ninfo);

    /* unpack into it */
    count = ninfo;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO))) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        PMIX_INFO_FREE(info, ninfo);
        rc = PRTE_ERR_UNPACK_FAILURE;
        return rc;
    }

    /* check for directives */
    ret = PMIX_SUCCESS;
    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
            data->range = info[n].value.data.range;
        } else if (PMIx_Check_key(info[n].key, PMIX_PERSISTENCE)) {
            data->persistence = info[n].value.data.persist;
        } else if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
            data->uid = info[n].value.data.uint32;
        } else if (PMIx_Check_key(info[n].key, PMIX_GRPID)) {
            data->gid = info[n].value.data.uint32;
        } else if (PMIx_Check_key(info[n].key, PMIX_ACCESS_PERMISSIONS)) {
            ret = load_permissions(&info[n].value, data);
        } else if (PMIx_Check_key(info[n].key, PMIX_ACCESS_USERIDS)) {
            /* the Standard puts these inside PMIX_ACCESS_PERMISSIONS, but
             * they are self-describing enough to honor at the top level */
            ret = load_ids(&info[n].value, &data->auids, &data->nauids);
        } else if (PMIx_Check_key(info[n].key, PMIX_ACCESS_GRPIDS)) {
            ret = load_ids(&info[n].value, &data->agids, &data->nagids);
        } else if (PMIx_Check_key(info[n].key, PMIX_REQUESTOR) ||
                   PMIx_Check_key(info[n].key, PRTE_PUBLISH_REQ_UID) ||
                   PMIx_Check_key(info[n].key, PRTE_PUBLISH_REQ_GID)) {
            /* a relay publishing on behalf of a process in its own DVM.
             * Applied below, once this scan has finished - see
             * prte_ds_check_requestor().  Skipped here so it is not stored
             * as published data. */
            continue;
        } else if (PMIx_Check_key(info[n].key, PRTE_PUBLISH_REPLACE)) {
            /* the publisher is updating something it published itself */
            replace = PMIX_INFO_TRUE(&info[n]);
        } else {
            /* add it to the list of data */
            ds1 = PMIX_NEW(prte_info_item_t);
            PMIX_INFO_XFER(&ds1->info, &info[n]);
            pmix_list_append(&data->info, &ds1->super);
        }
        if (PMIX_SUCCESS != ret) {
            /* an access restriction we could not read.  Storing the data
             * anyway would store it unrestricted, so refuse the publish */
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_RELEASE(data);
            return ret;
        }
    }
    /* Now let a relay's claimed identity override what PMIx told us about
     * the caller, which for a relayed request is the relaying daemon's own
     * tool identity.  After the scan, so the relay's PMIX_USERID cannot
     * land on top of the claim. */
    prte_ds_check_requestor(&data->owner, &data->uid, &data->gid, info, ninfo);

    /* A publisher that named no persistence, or named an invalid one, gets
     * the default the object was constructed with. */
    if (PMIX_PERSIST_INVALID == data->persistence) {
        data->persistence = PMIX_PERSIST_NSPACE;
    }

    /* Which application, and which session?  Neither is derivable later:
     * the job object does not outlive the job, and the purge that reclaims
     * an APP or SESSION item arrives after the publisher has gone.  Both
     * are therefore resolved now, and both are allowed to fail - a relayed
     * publish from another DVM has no proc object here, and a job with no
     * allocation of its own runs in the default session, which ends with
     * the DVM.  UINT32_MAX matches no purge. */
    resolve_publisher(data);

    /* the clock the retention timeout reads starts now */
    data->last_access = time(NULL);

    /* the values we keep were copied into the data object above, so the
     * unpacked array has done its job - it used to be freed only on the
     * unpack-failure path, which leaked it on every successful publish */
    PMIX_INFO_FREE(info, ninfo);

    /* Refuse a duplicate BEFORE anything is stored.
     *
     * "Duplicate keys being published on the same data range shall return
     * the PMIX_ERR_DUPLICATE_KEY error" - and until this was here the
     * duplicate was stored behind the original instead.  prte_ds_lookup()
     * answers a key from the first match it finds, so the second value was
     * unreachable for the life of the DVM: a write that reported success
     * and did nothing.  Nor was the loser reliably the newcomer, which is
     * what made it silent in both directions - the store is a
     * pmix_pointer_array_t and pmix_pointer_array_add() fills the LOWEST
     * FREE slot, so a duplicate landing in a slot some earlier unpublish
     * freed sits ahead of the original and displaces it instead.
     *
     * Which publisher owns the collision is the whole of the difference
     * between the two outcomes, so it is settled before the store is
     * touched: a publisher may take back its own prior publication if it
     * asked to, and nobody may take somebody else's name.
     *
     * Everything the checks read - the owner (which PMIX_REQUESTOR may have
     * replaced), the range, the uid and gid - is final only now that the
     * directive scan above has run. */
    PMIX_CONSTRUCT(&rq, prte_data_req_t);
    PMIX_XFER_PROCID(&rq.requestor, &data->owner);
    PMIX_XFER_PROCID(&rq.proxy, &data->proxy);
    rq.uid = data->uid;
    rq.gid = data->gid;
    rq.range = data->range;

    ndups = count_duplicates(&rq, data, &foreign);
    if (0 < ndups) {
        if (!replace || foreign) {
            pmix_output_verbose(1, prte_data_store.output,
                                "%s data server: refusing publish from %s - "
                                "%lu key(s) already published on %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PMIX_NAME_PRINT(&data->owner),
                                (unsigned long) ndups,
                                PMIx_Data_range_string(data->range));
            PMIX_DESTRUCT(&rq);
            PMIX_RELEASE(data);
            return PMIX_ERR_DUPLICATE_KEY;
        }
        drop_prior(&rq, data);
    }
    PMIX_DESTRUCT(&rq);

    /* Room for it, within what this publisher's uid may hold - evicting
     * that uid's own oldest items if need be, and refusing outright if the
     * item could not fit in an empty store.  Last of the gates, because it
     * is the only one that MODIFIES the store: a publish that is going to
     * be refused must not have cost anybody their data on the way. */
    if (!prte_ds_make_room(data)) {
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: refusing publish from %s - larger than "
                            "the whole per-uid limit",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIX_NAME_PRINT(&data->owner));
        PMIX_RELEASE(data);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    // add this data to our store
    data->index = pmix_pointer_array_add(&prte_data_store.store, data);
    prte_ds_charge(data);

    /* an INDEF or unread FIRST_READ item is the only thing the retention
     * timeout applies to, so the sweep runs only while the store holds one */
    prte_ds_arm_sweep();

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: checking for pending requests",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* check for pending requests that match this data */
    reply = NULL;
    rc = PRTE_SUCCESS;
    PMIX_LIST_FOREACH_SAFE(req, rqnext, &prte_data_store.pending, prte_data_req_t)
    {
        /* the same three tests an immediate lookup applies, in the same
         * order: the publisher's access permissions, then its range, then
         * the range the requestor asked us to search */
        if (PMIX_SUCCESS != prte_data_server_check_access(req, data)) {
            continue;
        }
        if (PMIX_SUCCESS != prte_data_server_check_range(req, data)) {
            continue;
        }
        if (PMIX_SUCCESS != prte_data_server_check_search_range(req, data)) {
            continue;
        }

        complete_resolved = false;
        cache = NULL;
        PMIX_CONSTRUCT(&answers, pmix_list_t);

        for (i = 0; NULL != req->keys[i]; i++) {
            /* cycle thru the data keys for matches */
            found = false;
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                pmix_output_verbose(10, prte_data_store.output,
                                    "%s\tCHECKING %s TO %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    ds1->info.key, req->keys[i]);

                if (PMIx_Check_key(ds1->info.key, req->keys[i])) {
                    pmix_output_verbose(10, prte_data_store.output,
                                        "%s data server: packaging return",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    /* track this response */
                    pmix_output_verbose(
                        10, prte_data_store.output,
                        "%s data server: adding %s data %s from %s:%d to response",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ds1->info.key,
                        PMIx_Data_type_string(ds1->info.value.type), data->owner.nspace,
                        data->owner.rank);
                    ds3 = PMIX_NEW(prte_info_item_t);
                    PMIX_INFO_XFER(&ds3->info, &ds1->info);
                    pmix_list_append(&answers, &ds3->super);
                    /* it was of use to somebody, so the retention timeout
                     * starts again from here.  Both places that answer a
                     * lookup have to do this - the other is ds_lookup.c */
                    data->last_access = time(NULL);
                    // if the persistence is "first read", then remove this info
                    if (PMIX_PERSIST_FIRST_READ == data->persistence) {
                        pmix_list_remove_item(&data->info, &ds1->super);
                        PMIX_RELEASE(ds1);
                    }
                    found = true;
                    break; // a key can only occur once
                }
            }
            if (!found) {
                PMIx_Argv_append_nosize(&cache, req->keys[i]);
            }
        }
        // update the keys to remove all that have been resolved
        if (0 < PMIx_Argv_count(cache)) {
            PMIx_Argv_free(req->keys);
            req->keys = cache;
        } else {
            // if no keys are in the cache, then all keys were resolved
            complete_resolved = true;
        }

        n = pmix_list_get_size(&answers);
        if (0 == n) {
            PMIX_LIST_DESTRUCT(&answers);
            continue;
        }


        /* send the answers back to the requestor */
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server:publish returning %lu data to %s:%d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (unsigned long)n,
                            req->requestor.nspace,
                            req->requestor.rank);

        PMIX_DATA_BUFFER_CREATE(reply);
        /* start with their room number */
        rc = PMIx_Data_pack(NULL, reply, &req->room_number, 1, PMIX_INT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto reply_failed;
        }
        /* we are responding to a lookup cmd */
        command = PRTE_PMIX_LOOKUP_CMD;
        rc = PMIx_Data_pack(NULL, reply, &command, 1, PMIX_UINT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto reply_failed;
        }
        /* If every key the request was still waiting on has now been
         * resolved, the lookup completed. Do NOT re-derive that from
         * req->keys: the unresolved remainder was swapped into it a few
         * lines above, so comparing our answer count against it reported
         * PARTIAL_SUCCESS for a request we had in fact satisfied in full. */
        ret = complete_resolved ? PMIX_SUCCESS : PMIX_ERR_PARTIAL_SUCCESS;
        /* return the status */
        rc = PMIx_Data_pack(NULL, reply, &ret, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto reply_failed;
        }

        /* pack the rest into a pmix_data_buffer_t */
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

        /* pack the number of returned info's */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &n, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRTE_ERR_PACK_FAILURE;
            goto reply_failed;
        }
        /* loop thru and pack the individual responses - this is somewhat less
         * efficient than packing an info array, but avoids another malloc
         * operation just to assemble all the return values into a contiguous
         * array */
        while (NULL != (ds3 = (prte_info_item_t *) pmix_list_remove_first(&answers))) {
            /* pack the data owner */
            ret = PMIx_Data_pack(NULL, &pbkt, &data->owner, 1, PMIX_PROC);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(ds3);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = PRTE_ERR_PACK_FAILURE;
                goto reply_failed;
            }
            /* pack the data */
            ret = PMIx_Data_pack(NULL, &pbkt, &ds3->info, 1, PMIX_INFO);
            PMIX_RELEASE(ds3);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = PRTE_ERR_PACK_FAILURE;
                goto reply_failed;
            }
        }
        PMIX_LIST_DESTRUCT(&answers);

        /* unload the pmix buffer */
        rc = PMIx_Data_unload(&pbkt, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return rc;
        }

        /* pack it into our reply */
        rc = PMIx_Data_pack(NULL, reply, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            /* Leave the request on the pending list. It used to be released
             * right here while still linked into that list, which left a
             * freed item behind for the next publish to walk into. */
            return rc;
        }
        PRTE_RML_RELIABLE_SEND(rc, req->proxy.rank, reply, PRTE_RML_TAG_DATA_CLIENT);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
        }
        if (0 == pmix_list_get_size(&data->info)) {
            // all the data was removed, so we no longer need this entry
            prte_ds_drop(data);
            data = NULL;
        } else {
            /* it shrank: what its publisher is charged has to follow */
            prte_ds_charge(data);
        }
        if (complete_resolved) {
            // completely resolved this pending request, so remove it
            pmix_list_remove_item(&prte_data_store.pending, &req->super);
            PMIX_RELEASE(req);
        }
        if (NULL == data) {
            break;
        }
        continue;

    reply_failed:
        /* a reply to one waiting requestor could not be assembled. Drop it
         * and let the publish itself report the failure - but not before
         * releasing the partial reply and the answers we had collected,
         * both of which used to leak straight out of the function. */
        PMIX_LIST_DESTRUCT(&answers);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return rc;
    }

    if (PMIX_SUCCESS == rc) {
        pmix_status_t st = PMIX_SUCCESS;

        // send back an answer
        rc = PMIx_Data_pack(NULL, answer, &st, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PRTE_RML_RELIABLE_SEND(rc, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(answer);
        }
    }


    return rc;
}
