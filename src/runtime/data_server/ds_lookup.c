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
#include "src/util/name_fns.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/data_server/ds.h"

/* Pack what a lookup found into the byte object its answer carries: the
 * count, then each value's owner and the value itself.  This is somewhat
 * less efficient than packing an info array, but avoids another malloc
 * operation just to assemble all the return values into a contiguous
 * array. */
static pmix_status_t pack_answers(pmix_list_t *answers, pmix_byte_object_t *pbo)
{
    pmix_data_buffer_t pbkt;
    prte_ds_info_t *rinfo;
    size_t n = pmix_list_get_size(answers);
    pmix_status_t rc;

    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    rc = PMIx_Data_pack(NULL, &pbkt, &n, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return rc;
    }
    PMIX_LIST_FOREACH(rinfo, answers, prte_ds_info_t) {
        rc = PMIx_Data_pack(NULL, &pbkt, &rinfo->source, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return rc;
        }
        rc = PMIx_Data_pack(NULL, &pbkt, &rinfo->info, 1, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return rc;
        }
    }
    rc = PMIx_Data_unload(&pbkt, pbo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
    return rc;
}

/* see ds.h */
void prte_ds_reply_parked(prte_data_req_t *req, pmix_status_t status,
                          pmix_byte_object_t *pbo)
{
    pmix_data_buffer_t *reply;
    uint8_t command = PRTE_PMIX_LOOKUP_CMD;
    pmix_status_t rc;
    int ret;

    PMIX_DATA_BUFFER_CREATE(reply);
    rc = PMIx_Data_pack(NULL, reply, &req->room_number, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    rc = PMIx_Data_pack(NULL, reply, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    rc = PMIx_Data_pack(NULL, reply, &status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    if (NULL != pbo) {
        rc = PMIx_Data_pack(NULL, reply, pbo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
    }
    PRTE_RML_RELIABLE_SEND(ret, req->proxy.rank, reply, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(reply);
    }
}

/* A parked lookup that was given a PMIX_TIMEOUT gets an event that fires if
 * nothing satisfies it first.  Without one, a PMIX_WAIT lookup for a key
 * nobody ever publishes waits forever: the timeout the caller gave reached
 * the daemon's caddy and went no further, and the request left the pending
 * list only when a publish matched it or its requestor died. */
static void lookup_timeout(int sd, short args, void *cbdata)
{
    prte_data_req_t *req = (prte_data_req_t *) cbdata;

    PRTE_HIDE_UNUSED_PARAMS(sd, args);
    PMIX_ACQUIRE_OBJECT(req);

    /* the event has fired, so there is nothing left to disarm */
    req->timer_active = false;

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: parked lookup from %s timed out",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_NAME_PRINT(&req->requestor));

    /* it is waiting for nothing now */
    pmix_list_remove_item(&prte_data_store.pending, &req->super);

    /* A timeout carries no payload, and the daemon-side receiver knows not
     * to look for one.  Nothing was taken out of the store on this
     * request's behalf while it waited, so there is nothing to give back. */
    prte_ds_reply_parked(req, PMIX_ERR_TIMEOUT, NULL);
    PMIX_RELEASE(req);
}

size_t prte_ds_collect(prte_data_req_t *req, char **keys, pmix_list_t *answers,
                       bool *denied)
{
    prte_data_object_t *data;
    prte_info_item_t *ds1, *ds2;
    prte_ds_info_t *rinfo;
    size_t nfound = 0;
    bool found;
    int i, k;

    for (i = 0; NULL != keys[i]; i++) {
        pmix_output_verbose(10, prte_data_store.output,
                            "%s data server: looking for %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), keys[i]);
        found = false;
        /* cycle across the stored data, looking for a match */
        for (k = 0; k < prte_data_store.store.size && !found; k++) {
            data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
            if (NULL == data) {
                continue;
            }
            /* Access is decided in two steps, in this order: the
             * requestor must first satisfy the publisher's access
             * permissions, and then meet its range constraint. */
            if (PMIX_SUCCESS != prte_data_server_check_access(req, data)) {
                /* if this item holds the key, the answer is "you may not
                 * have it" rather than "there is no such thing", and the
                 * retrieval rules ask us to say so */
                if (NULL != denied) {
                    PMIX_LIST_FOREACH(ds1, &data->info, prte_info_item_t) {
                        if (PMIx_Check_key(ds1->info.key, keys[i])) {
                            *denied = true;
                            break;
                        }
                    }
                }
                continue;
            }
            if (PMIX_SUCCESS != prte_data_server_check_range(req, data)) {
                continue;
            }
            /* ...and the requestor's own range constrains which publishers
             * it asked us to search at all */
            if (PMIX_SUCCESS != prte_data_server_check_search_range(req, data)) {
                continue;
            }
            /* see if we have this key */
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                pmix_output_verbose(10, prte_data_store.output,
                                    "%s COMPARING %s %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), keys[i],
                                    ds1->info.key);
                if (!PMIx_Check_key(ds1->info.key, keys[i])) {
                    continue;
                }
                // can only find it once - keys are required to be globally unique
                // within a given range, and we checked the range above
                found = true;
                if (NULL == answers) {
                    /* only counting */
                    break;
                }
                rinfo = PMIX_NEW(prte_ds_info_t);
                memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                PMIX_INFO_XFER(&rinfo->info, &ds1->info);
                pmix_list_append(answers, &rinfo->super);
                pmix_output_verbose(1, prte_data_store.output,
                                    "%s data server: adding %s to data from %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ds1->info.key,
                                    PRTE_NAME_PRINT(&data->owner));
                /* it was of use to somebody, so the retention timeout
                 * starts again from here */
                data->last_access = time(NULL);
                if (PMIX_PERSIST_FIRST_READ == data->persistence) {
                    pmix_list_remove_item(&data->info, &ds1->super);
                    PMIX_RELEASE(ds1);
                    /* An item that has given up its last key is no longer
                     * holding anything, and has to leave the store rather
                     * than sit in it empty - matching nothing, answering
                     * nothing, and removed only by a purge.
                     *
                     * "data" is the loop variable of the enclosing scan over
                     * the store, so nothing may touch it after this; the
                     * break below, and "found" ending the scan, are what
                     * make that safe. */
                    if (0 == pmix_list_get_size(&data->info)) {
                        prte_ds_drop(data);
                        data = NULL;
                    } else {
                        /* it shrank: recharge what is left of it */
                        prte_ds_charge(data);
                    }
                }
                break;
            }
        } // loop over stored data
        if (found) {
            nfound++;
        }
    } // loop over keys

    return nfound;
}

/* How many found keys does a PMIX_WAIT lookup need before it is answered? */
static size_t wait_target(prte_data_req_t *req, size_t nkeys)
{
    if (0 == req->nwait || req->nwait > nkeys) {
        return nkeys;
    }
    return req->nwait;
}

bool prte_ds_answer_parked(prte_data_req_t *req)
{
    size_t nkeys = (size_t) PMIx_Argv_count(req->keys);
    size_t nfound;
    pmix_list_t answers;
    pmix_byte_object_t pbo;
    pmix_status_t rc;

    /* Decide before taking anything.  A FIRST_READ value handed to a
     * request that then goes on waiting is a value nobody will ever
     * receive: the daemon that asked frees its room on the FIRST reply it
     * gets, so there is exactly one answer to put it in, and it is not
     * this one. */
    if (prte_ds_collect(req, req->keys, NULL, NULL) < wait_target(req, nkeys)) {
        return false;
    }

    PMIX_CONSTRUCT(&answers, pmix_list_t);
    nfound = prte_ds_collect(req, req->keys, &answers, NULL);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: answering parked lookup from %s with %lu of %lu keys",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_NAME_PRINT(&req->requestor),
                        (unsigned long) nfound, (unsigned long) nkeys);

    rc = pack_answers(&answers, &pbo);
    PMIX_LIST_DESTRUCT(&answers);
    if (PMIX_SUCCESS != rc) {
        /* tell it, rather than leave it parked on a room nobody answers */
        prte_ds_reply_parked(req, rc, NULL);
        return true;
    }
    prte_ds_reply_parked(req, (nfound < nkeys) ? PMIX_ERR_PARTIAL_SUCCESS : PMIX_SUCCESS, &pbo);
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    return true;
}

pmix_status_t prte_ds_lookup(pmix_proc_t *sender, int room_number,
                             pmix_data_buffer_t *buffer,
                             pmix_data_buffer_t *answer)
{
    int32_t count;
    size_t nfound, nkeys;
    pmix_status_t rc, ret;
    pmix_proc_t requestor;
    size_t n, ninfo;
    char **keys = NULL;
    char *str;
    pmix_info_t *info;
    uint32_t uid = UINT32_MAX;
    uint32_t gid = UINT32_MAX;
    int timeout = 0;
    size_t nwait = 0;
    uint8_t u8;
    bool wait = false;
    bool denied = false;
    /* the default range for a lookup is SESSION - see the PMIx
     * retrieval rules for published data */
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_list_t answers;
    prte_data_req_t *req, rq;
    pmix_byte_object_t pbo;
    struct timeval tv;

    /* unpack the requestor */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* unpack the number of keys */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nkeys, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 == nkeys) {
        /* they forgot to send us the keys?? */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* unpack the keys */
    for (n = 0; n < nkeys; n++) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &str, &count, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Argv_free(keys);
            return rc;
        }
        PMIx_Argv_append_nosize(&keys, str);
        free(str);
    }

    /* unpack the number of directives, if any */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIx_Argv_free(keys);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        count = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            PMIx_Argv_free(keys);
            return rc;
        }
        /* scan the directives for things we care about */
        for (n = 0; n < ninfo; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
                uid = info[n].value.data.uint32;
            } else if (PMIx_Check_key(info[n].key, PMIX_GRPID)) {
                gid = info[n].value.data.uint32;
            } else if (PMIx_Check_key(info[n].key, PMIX_TIMEOUT)) {
                if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &timeout, PMIX_INT)) {
                    timeout = 0;
                }
            } else if (PMIx_Check_key(info[n].key, PMIX_WAIT)) {
                /* flag that we wait until the data is present.  The value
                 * is how many of the keys to wait for, 0 meaning all of
                 * them; a value that is not a count - a bool is common -
                 * asks for the default */
                wait = true;
                if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &nwait, PMIX_SIZE)) {
                    nwait = 0;
                }
            } else if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
                if (PMIX_SUCCESS != prte_ds_get_named_uint8(&info[n].value, PMIX_DATA_RANGE, &u8)) {
                    /* the search would be over a set of processes we cannot
                     * name - refuse it, rather than search everything */
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    PMIX_INFO_FREE(info, ninfo);
                    PMIx_Argv_free(keys);
                    return PMIX_ERR_BAD_PARAM;
                }
                range = u8;
            }
        }
        /* a relay looking up on behalf of a process in its own DVM.  After
         * the scan: the uid PMIx appended is the RELAY's, and the access
         * rules have to be answered about the process actually asking. */
        prte_ds_check_requestor(&requestor, &uid, &gid, info, ninfo);
        /* ignore anything else for now */
        PMIX_INFO_FREE(info, ninfo);
    }

    PMIX_CONSTRUCT(&rq, prte_data_req_t);
    memcpy(&rq.requestor, &requestor, sizeof(pmix_proc_t));
    memcpy(&rq.proxy, sender, sizeof(pmix_proc_t));
    /* the range the requestor gave constrains the search, so it has to be
     * on the request we hand to the range checks - it used to be unpacked
     * and then only ever reach a PARKED request, which meant an immediate
     * lookup searched everything the publishers would let it see */
    rq.range = range;
    rq.uid = uid;
    rq.gid = gid;
    rq.nwait = nwait;

    /* A lookup that is prepared to wait, and cannot yet be given what it is
     * waiting for, is parked with ALL of its keys and takes nothing from the
     * store.  It used to take what it could find and then park the rest:
     * the values found were dropped on the floor - a FIRST_READ value among
     * them was gone from the store too - and the eventual answer carried
     * only the keys a later publish supplied, reported as a complete
     * success. */
    if (wait && prte_ds_collect(&rq, keys, NULL, NULL) < wait_target(&rq, nkeys)) {
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server:lookup: parking a lookup for %lu keys from %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned long) nkeys,
                            PMIX_NAME_PRINT(&requestor));

        req = PMIX_NEW(prte_data_req_t);
        req->room_number = room_number;
        req->proxy = *sender;
        memcpy(&req->requestor, &requestor, sizeof(pmix_proc_t));
        req->uid = uid;
        req->gid = gid;
        req->range = range;
        req->nwait = nwait;
        req->keys = keys;
        keys = NULL;
        pmix_list_append(&prte_data_store.pending, &req->super);
        if (0 < timeout) {
            /* the caller said how long it is prepared to wait */
            tv.tv_sec = timeout;
            tv.tv_usec = 0;
            prte_event_evtimer_set(prte_event_base, &req->ev,
                                   lookup_timeout, req);
            req->timer_active = true;
            PMIX_POST_OBJECT(req);
            prte_event_evtimer_add(&req->ev, &tv);
        }
        PMIX_DESTRUCT(&rq);
        /* No answer goes back now - the request waits until a publish
         * satisfies it. Our caller's contract is "PMIX_SUCCESS means the
         * handler has disposed of the answer buffer", so dispose of it:
         * the publish path builds a fresh reply of its own, and leaving
         * this one behind leaked a buffer per waiting lookup. */
        PMIX_DATA_BUFFER_RELEASE(answer);
        return PMIX_SUCCESS; // do not return an answer
    }

    PMIX_CONSTRUCT(&answers, pmix_list_t);
    nfound = prte_ds_collect(&rq, keys, &answers, &denied);
    PMIx_Argv_free(keys);
    PMIX_DESTRUCT(&rq);

    if (0 == nfound) {
        /* nothing was found - indicate that situation.  If the data was
         * there and we refused it, say which: the retrieval rules reserve
         * PMIX_ERR_NO_PERMISSIONS for exactly this case */
        PMIX_LIST_DESTRUCT(&answers);
        return denied ? PMIX_ERR_NO_PERMISSIONS : PMIX_ERR_NOT_FOUND;
    }

    /* We get here having found at least something, so the answer carries
     * data whether the status is SUCCESS or PARTIAL_SUCCESS - a partial
     * result used to fall straight through to "return rc", which handed the
     * caller an error to relay and quietly dropped both the values we found
     * and the buffer holding them. */
    ret = (nfound < nkeys) ? PMIX_ERR_PARTIAL_SUCCESS : PMIX_SUCCESS;

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server:lookup: data found - status %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIx_Error_string(ret));

    rc = pack_answers(&answers, &pbo);
    PMIX_LIST_DESTRUCT(&answers);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* pack the status */
    rc = PMIx_Data_pack(NULL, answer, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        return rc;
    }
    /* pack the values into our reply */
    rc = PMIx_Data_pack(NULL, answer, &pbo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PRTE_RML_RELIABLE_SEND(rc, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != rc) {
        /* The send refused the buffer - the requesting daemon is gone - so
         * it is still ours, and releasing it disposes of it.  That is
         * PMIX_SUCCESS by the answer-buffer contract: returning the send's
         * error handed our caller a buffer we had just freed, which it
         * packed the error into, sent, and released a second time. */
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
    }
    /* the answer has been disposed of - tell our caller not to send another */
    return PMIX_SUCCESS;
}
