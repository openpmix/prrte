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
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
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

/* The persistences and ranges this store knows how to honor.  Spelled out
 * rather than bounded by the highest value: neither family is a ladder,
 * and a value PMIx adds later is one this store cannot honor until it is
 * taught what it means. */
static bool known_persistence(pmix_persistence_t persist)
{
    switch (persist) {
    case PMIX_PERSIST_INDEF:
    case PMIX_PERSIST_FIRST_READ:
    case PMIX_PERSIST_PROC:
    case PMIX_PERSIST_APP:
    case PMIX_PERSIST_SESSION:
    case PMIX_PERSIST_NSPACE:
        return true;
    default:
        return false;
    }
}

static bool known_range(pmix_data_range_t range)
{
    switch (range) {
    case PMIX_RANGE_UNDEF:
    case PMIX_RANGE_RM:
    case PMIX_RANGE_LOCAL:
    case PMIX_RANGE_NAMESPACE:
    case PMIX_RANGE_SESSION:
    case PMIX_RANGE_GLOBAL:
    case PMIX_RANGE_CUSTOM:
    case PMIX_RANGE_PROC_LOCAL:
        return true;
    default:
        return false;
    }
}

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

/* Does this publication collide with what is already stored?
 *
 * Counts the colliding keys and reports whether any of them belongs to a
 * DIFFERENT publisher, which is what decides between "you may replace your
 * own" and "that name is taken".  Nothing is modified here: the decision
 * has to be complete before anything is removed, so that a publish which
 * ends up refused leaves the store exactly as it found it. */
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
        if (!prte_data_server_same_range(rq, dptr, data->range)) {
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
        if (!prte_data_server_same_range(rq, dptr, data->range)) {
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

/* Could this newly stored item advance a parked lookup?  It must hold one of
 * the keys the request is waiting for, and pass the three tests the lookup
 * applies.  A cheap filter ahead of prte_ds_answer_parked(), which scans the
 * whole store: only a publish can make a parked request answerable, and
 * only by being an item that request can see. */
static bool answers_request(prte_data_req_t *req, prte_data_object_t *data)
{
    prte_info_item_t *item;
    int i;

    if (PMIX_SUCCESS != prte_data_server_check_access(req, data) ||
        PMIX_SUCCESS != prte_data_server_check_range(req, data) ||
        PMIX_SUCCESS != prte_data_server_check_search_range(req, data)) {
        return false;
    }
    for (i = 0; NULL != req->keys[i]; i++) {
        PMIX_LIST_FOREACH(item, &data->info, prte_info_item_t) {
            if (PMIx_Check_key(item->info.key, req->keys[i])) {
                return true;
            }
        }
    }
    return false;
}

pmix_status_t prte_ds_publish(pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer,
                              pmix_data_buffer_t *answer)
{
    int32_t count;
    prte_data_object_t *data;
    int rc;
    size_t ninfo;
    prte_data_req_t *req, *rqnext;
    pmix_status_t ret, st = PMIX_SUCCESS;
    prte_info_item_t *ds1;
    size_t n, ndups;
    pmix_info_t *info;
    prte_data_req_t rq;
    uint8_t u8;
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
        /* the status goes back to a PMIx client, so it is a PMIx one */
        return ret;
    }

    /* create the space */
    PMIX_INFO_CREATE(info, ninfo);

    /* unpack into it */
    count = ninfo;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO))) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        PMIX_INFO_FREE(info, ninfo);
        return ret;
    }

    /* check for directives */
    ret = PMIX_SUCCESS;
    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
            /* a range or a persistence we cannot read is refused along with
             * the publish, for the same reason an access restriction is */
            ret = prte_ds_get_named_uint8(&info[n].value, PMIX_DATA_RANGE, &u8);
            if (PMIX_SUCCESS == ret) {
                data->range = u8;
            }
        } else if (PMIx_Check_key(info[n].key, PMIX_PERSISTENCE)) {
            ret = prte_ds_get_named_uint8(&info[n].value, PMIX_PERSIST, &u8);
            if (PMIX_SUCCESS == ret) {
                data->persistence = u8;
            }
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
        } else if (PMIx_Check_key(info[n].key, PMIX_TIMEOUT)) {
            /* A directive the Standard defines for PMIx_Publish, and one
             * the daemon has already acted on (pmix_server_pub.c) - not
             * data.  It used to fall through to the store as a published
             * key named "pmix.timeout", so the same user's next publish
             * that carried a timeout collided with it and was refused as a
             * duplicate, whatever it was actually publishing. */
            continue;
        } else {
            /* add it to the list of data */
            ds1 = PMIX_NEW(prte_info_item_t);
            PMIX_INFO_XFER(&ds1->info, &info[n]);
            pmix_list_append(&data->info, &ds1->super);
        }
        if (PMIX_SUCCESS != ret) {
            /* a restriction we could not read - an access list, a range, or
             * a persistence.  Storing the data anyway would store it with a
             * restriction nobody asked for, so refuse the publish */
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

    /* A publisher that named no persistence, or named PMIX_PERSIST_INVALID,
     * gets the default the object was constructed with. */
    if (PMIX_PERSIST_INVALID == data->persistence) {
        data->persistence = PMIX_PERSIST_NSPACE;
    }
    /* Any other value we do not know is refused, as is a range we do not
     * know.  A persistence nothing here recognizes is one no purge horizon
     * takes and the retention sweep does not touch, so the item used to be
     * kept for the life of the DVM whatever the publisher meant; a range
     * nothing recognizes admits nobody, so the item was stored where no
     * lookup could reach it.  Both reported success. */
    if (!known_persistence(data->persistence) || !known_range(data->range)) {
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: refusing publish from %s - unknown %s %u",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIX_NAME_PRINT(&data->owner),
                            known_range(data->range) ? "persistence" : "range",
                            (unsigned) (known_range(data->range) ? data->persistence
                                                                 : data->range));
        PMIX_INFO_FREE(info, ninfo);
        PMIX_RELEASE(data);
        return PMIX_ERR_BAD_PARAM;
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
    if (0 > data->index) {
        /* not stored, so neither charged nor answerable */
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        PMIX_RELEASE(data);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    prte_ds_charge(data);

    /* an INDEF or unread FIRST_READ item is the only thing the retention
     * timeout applies to, so the sweep runs only while the store holds one */
    prte_ds_arm_sweep();

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: checking for pending requests",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* Answer any parked lookup this publish lets us complete.
     *
     * A parked request gets exactly one reply: the daemon that asked frees
     * its room on the first answer it receives.  This loop used to answer
     * whatever this one item resolved, as PMIX_ERR_PARTIAL_SUCCESS when
     * that was not everything, and leave the request parked for the rest -
     * so the requestor was handed a partial answer it had asked to wait
     * past, a FIRST_READ value it resolved was consumed on its behalf, and
     * the NEXT matching publish replied to a room already freed, or already
     * handed to some other request.  prte_ds_answer_parked() now decides
     * against the whole store and answers once.
     *
     * Satisfying a request can consume this item's FIRST_READ keys and drop
     * it from the store, so hold a reference of our own for as long as the
     * loop reads it.  A dropped item holds no keys, so it matches nothing
     * further. */
    PMIX_RETAIN(data);
    PMIX_LIST_FOREACH_SAFE(req, rqnext, &prte_data_store.pending, prte_data_req_t)
    {
        /* only a request this item could advance needs the full scan */
        if (!answers_request(req, data)) {
            continue;
        }
        if (prte_ds_answer_parked(req)) {
            pmix_list_remove_item(&prte_data_store.pending, &req->super);
            PMIX_RELEASE(req);
        }
    }
    PMIX_RELEASE(data);

    /* The data is stored whatever became of the parked requests, so the
     * publisher is told it succeeded.  A pending reply that could not be
     * delivered used to leave its error in rc, which suppressed this
     * answer and reported the error to the publisher instead - a publish
     * that had in fact taken effect, and that a retry would then find
     * refused as a duplicate. */
    rc = PMIx_Data_pack(NULL, answer, &st, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PRTE_RML_RELIABLE_SEND(rc, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != rc) {
        /* The send refused the buffer - the publisher's daemon is gone - so
         * it is still ours, and releasing it disposes of it.  That is
         * PMIX_SUCCESS by the answer-buffer contract: returning the send's
         * error handed our caller a buffer we had just freed. */
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
    }
    return PMIX_SUCCESS;
}
