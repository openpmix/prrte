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
#include "src/util/name_fns.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/data_server/ds.h"

/* A parked lookup that was given a PMIX_TIMEOUT gets an event that fires if
 * nothing satisfies it first.  Without one, a PMIX_WAIT lookup for a key
 * nobody ever publishes waits forever: the timeout the caller gave reached
 * the daemon's caddy and went no further, and the request left the pending
 * list only when a publish matched it or its requestor died. */
static void lookup_timeout(int sd, short args, void *cbdata)
{
    prte_data_req_t *req = (prte_data_req_t *) cbdata;
    pmix_data_buffer_t *reply;
    pmix_status_t ret = PMIX_ERR_TIMEOUT;
    uint8_t command = PRTE_PMIX_LOOKUP_CMD;
    int rc;

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

    /* answer it: room number, the command it is an answer to, and the
     * status.  A timeout carries no payload, and the daemon-side receiver
     * knows not to look for one */
    PMIX_DATA_BUFFER_CREATE(reply);
    rc = PMIx_Data_pack(NULL, reply, &req->room_number, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    rc = PMIx_Data_pack(NULL, reply, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    rc = PMIx_Data_pack(NULL, reply, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    PRTE_RML_RELIABLE_SEND(rc, req->proxy.rank, reply, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        goto done;
    }
    PMIX_RELEASE(req);
    return;

done:
    PMIX_DATA_BUFFER_RELEASE(reply);
    PMIX_RELEASE(req);
}

pmix_status_t prte_ds_lookup(pmix_proc_t *sender, int room_number,
                             pmix_data_buffer_t *buffer,
                             pmix_data_buffer_t *answer)
{
    int32_t count;
    int i, k;
    size_t nanswers;
    pmix_status_t rc, ret;
    pmix_proc_t requestor;
    size_t n, ninfo;
    char **keys = NULL, **cache = NULL;
    char *str;
    pmix_info_t *info;
    pmix_data_buffer_t pbkt;
    uint32_t uid = UINT32_MAX;
    uint32_t gid = UINT32_MAX;
    int timeout = 0;
    bool wait = false;
    bool denied = false;
    /* the default range for a lookup is SESSION - see the PMIx
     * retrieval rules for published data */
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    prte_data_object_t *data;
    prte_ds_info_t *rinfo;
    prte_info_item_t *ds1, *ds2;
    pmix_list_t answers;
    bool found;
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
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 == ninfo) {
        /* they forgot to send us the keys?? */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* unpack the keys */
    for (n = 0; n < ninfo; n++) {
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
                timeout = info[n].value.data.integer;
            } else if (PMIx_Check_key(info[n].key, PMIX_WAIT)) {
                /* flag that we wait until the data is present */
                wait = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
                range = info[n].value.data.range;
            }
        }
        /* a relay looking up on behalf of a process in its own DVM.  After
         * the scan: the uid PMIx appended is the RELAY's, and the access
         * rules have to be answered about the process actually asking. */
        prte_ds_check_requestor(&requestor, &uid, &gid, info, ninfo);
        /* ignore anything else for now */
        PMIX_INFO_FREE(info, ninfo);
    }

    /* cycle across the provided keys */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    PMIX_CONSTRUCT(&answers, pmix_list_t);
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
            if (PMIX_SUCCESS != prte_data_server_check_access(&rq, data)) {
                /* if this item holds the key, the answer is "you may not
                 * have it" rather than "there is no such thing", and the
                 * retrieval rules ask us to say so */
                PMIX_LIST_FOREACH(ds1, &data->info, prte_info_item_t) {
                    if (PMIx_Check_key(ds1->info.key, keys[i])) {
                        denied = true;
                        break;
                    }
                }
                continue;
            }
            if (PMIX_SUCCESS != prte_data_server_check_range(&rq, data)) {
                continue;
            }
            /* ...and the requestor's own range constrains which publishers
             * it asked us to search at all */
            if (PMIX_SUCCESS != prte_data_server_check_search_range(&rq, data)) {
                continue;
            }
            /* see if we have this key */
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                pmix_output_verbose(10, prte_data_store.output,
                                    "%s COMPARING %s %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), keys[i],
                                    ds1->info.key);
                if (PMIx_Check_key(ds1->info.key, keys[i])) {
                    rinfo = PMIX_NEW(prte_ds_info_t);
                    memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                    PMIX_INFO_XFER(&rinfo->info, &ds1->info);
                    // check the persistence
                    pmix_output_verbose(1, prte_data_store.output,
                                        "%s data server: adding %s to data from %s",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ds1->info.key,
                                        PRTE_NAME_PRINT(&data->owner));
                    /* it was of use to somebody, so the retention timeout
                     * starts again from here.  Both places that answer a
                     * lookup have to do this - the other is ds_publish.c,
                     * where a publish satisfies a parked request */
                    data->last_access = time(NULL);
                    if (PMIX_PERSIST_FIRST_READ == data->persistence) {
                        pmix_list_remove_item(&data->info, &ds1->super);
                        PMIX_RELEASE(ds1);
                        /* An item that has given up its last key is no
                         * longer holding anything, and has to leave the
                         * store rather than sit in it empty.  The publish
                         * side has always done this where it satisfies a
                         * parked request; here the object was left behind,
                         * so a FIRST_READ item that was read by an ordinary
                         * lookup stayed in the store as an empty shell -
                         * matching nothing, answering nothing, and removed
                         * only by a purge.
                         *
                         * "data" is the loop variable of the enclosing scan
                         * over the store, so nothing may touch it after
                         * this; the break below is what makes that safe. */
                        if (0 == pmix_list_get_size(&data->info)) {
                            prte_ds_drop(data);
                            data = NULL;
                        } else {
                            /* it shrank: recharge what is left of it */
                            prte_ds_charge(data);
                        }
                    }
                    pmix_list_append(&answers, &rinfo->super);
                    // can only find it once - keys are required to be globally unique
                    // within a given range, and we checked the range above
                    found = true;
                    break;
                }
            }
        } // loop over stored data
        if (!found) {
            // cache the key
            PMIx_Argv_append_nosize(&cache, keys[i]);
        }
    }     // loop over keys

    nanswers = pmix_list_get_size(&answers);
    rc = PMIX_ERR_NOT_FOUND;
    if (0 < nanswers) {
        /* pack the number of data items found */
        rc = PMIx_Data_pack(NULL, &pbkt, &nanswers, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_LIST_DESTRUCT(&answers);
            PMIx_Argv_free(keys);
            PMIx_Argv_free(cache);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&rq);
            return rc;
        }
        /* loop thru and pack the individual responses - this is somewhat less
         * efficient than packing an info array, but avoids another malloc
         * operation just to assemble all the return values into a contiguous
         * array */
        PMIX_LIST_FOREACH(rinfo, &answers, prte_ds_info_t)
        {
            /* pack the data owner */
            rc = PMIx_Data_pack(NULL, &pbkt, &rinfo->source, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&answers);
                PMIx_Argv_free(keys);
                PMIx_Argv_free(cache);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_DESTRUCT(&rq);
                return rc;
            }
            rc = PMIx_Data_pack(NULL, &pbkt, &rinfo->info, 1, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&answers);
                PMIx_Argv_free(keys);
                PMIx_Argv_free(cache);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_DESTRUCT(&rq);
                return rc;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&answers);

    i = PMIx_Argv_count(cache);
    if (0 < i) {
        if (wait) {
            pmix_output_verbose(1, prte_data_store.output,
                                "%s data server:lookup: at least some data not found %d vs %d",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nanswers,
                                (int) PMIx_Argv_count(keys));

            req = PMIX_NEW(prte_data_req_t);
            req->room_number = room_number;
            req->proxy = *sender;
            memcpy(&req->requestor, &requestor, sizeof(pmix_proc_t));
            req->uid = uid;
            req->gid = gid;
            req->range = range;
            req->keys = cache;
            cache = NULL;
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
            PMIx_Argv_free(keys);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&rq);
            /* No answer goes back now - the request waits until a publish
             * satisfies it. Our caller's contract is "PMIX_SUCCESS means the
             * handler has disposed of the answer buffer", so dispose of it:
             * the publish path builds a fresh reply of its own, and leaving
             * this one behind leaked a buffer per waiting lookup. */
            PMIX_DATA_BUFFER_RELEASE(answer);
            return PMIX_SUCCESS; // do not return an answer
        } else {
            PMIx_Argv_free(cache);
            cache = NULL;
            if (0 == nanswers) {
                /* nothing was found - indicate that situation.  If the
                 * data was there and we refused it, say which: the
                 * retrieval rules reserve PMIX_ERR_NO_PERMISSIONS for
                 * exactly this case */
                rc = denied ? PMIX_ERR_NO_PERMISSIONS : PMIX_ERR_NOT_FOUND;
                PMIx_Argv_free(keys);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_DESTRUCT(&rq);
                return rc;
            } else {
                rc = PMIX_ERR_PARTIAL_SUCCESS;
            }
        }
    } else {
        rc = PMIX_SUCCESS;
    }
    PMIx_Argv_free(keys);
    PMIX_DESTRUCT(&rq);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server:lookup: data found - status %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIx_Error_string(rc));

    /* We get here having found at least something, so the answer carries
     * data whether the status is SUCCESS or PARTIAL_SUCCESS - a partial
     * result used to fall straight through to "return rc", which handed the
     * caller an error to relay and quietly dropped both the values we found
     * and the buffer holding them. */
    ret = rc;
    /* pack the status */
    rc = PMIx_Data_pack(NULL, answer, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return rc;
    }
    /* unload the packed values */
    rc = PMIx_Data_unload(&pbkt, &pbo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return rc;
    }
    /* pack it into our reply */
    rc = PMIx_Data_pack(NULL, answer, &pbo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PRTE_RML_RELIABLE_SEND(rc, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return rc;
    }
    /* the answer has been sent - tell our caller not to send another */
    return PMIX_SUCCESS;
}
