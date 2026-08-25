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


// globals
prte_data_store_t prte_data_store = {
    .store = PMIX_POINTER_ARRAY_STATIC_INIT,
    .pending = PMIX_LIST_STATIC_INIT,
    .output = -1,
    .verbosity = 0
};


/* locals */
static bool initialized = false;

int prte_data_server_init(void)
{
    pmix_status_t rc;

    if (initialized) {
        return PRTE_SUCCESS;
    }
    initialized = true;

    /* register a verbosity */
    prte_data_store.verbosity = -1;
    (void) pmix_mca_base_var_register("prte", "prte", "data", "server_verbose",
                                      "Debug verbosity for PRTE data server",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &prte_data_store.verbosity);
    if (0 <= prte_data_store.verbosity) {
        prte_data_store.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_data_store.output, prte_data_store.verbosity);
    }

    PMIX_CONSTRUCT(&prte_data_store.store, pmix_pointer_array_t);
    if (PMIX_SUCCESS != (rc = pmix_pointer_array_init(&prte_data_store.store, 1, INT_MAX, 1))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    PMIX_CONSTRUCT(&prte_data_store.pending, pmix_list_t);

    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DATA_SERVER,
                  PRTE_RML_PERSISTENT, prte_data_server, NULL);

    return PRTE_SUCCESS;
}

void prte_data_server_finalize(void)
{
    int32_t i;
    prte_data_object_t *data;

    if (!initialized) {
        return;
    }
    initialized = false;

    for (i = 0; i < prte_data_store.store.size; i++) {
        data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, i);
        if (NULL != data) {
            PMIX_RELEASE(data);
        }
    }
    PMIX_DESTRUCT(&prte_data_store.store);
    PMIX_LIST_DESTRUCT(&prte_data_store.pending);
}

void prte_data_server(int status, pmix_proc_t *sender,
                      pmix_data_buffer_t *buffer,
                      prte_rml_tag_t tag, void *cbdata)
{
    uint8_t command;
    int32_t count;
    pmix_data_buffer_t *answer;
    pmix_status_t rc;
    int room_number;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server got message from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the room number of the caller's request */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &room_number, &count, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the command */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &command, &count, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    PMIX_DATA_BUFFER_CREATE(answer);
    /* pack the room number as this must lead any response */
    rc = PMIx_Data_pack(NULL, answer, &room_number, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return;
    }
    /* and the command */
    rc = PMIx_Data_pack(NULL, answer, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return;
    }

    /* When an external data server is configured, this DVM stores nothing:
     * the request is reissued to that server over our PMIx tool connection
     * to it, and the relay answers this sender when it replies.  Only the
     * master holds that connection, which is why every daemon addressed
     * its request here (pmix_server_pub.c, execute).
     *
     * A relay that could NOT take the request on has to be reported like any
     * other failure.  This used to log the error and return, answering
     * nobody - so the daemon that asked stayed parked on its room number and
     * the process behind it hung for good.  An unreachable data server is
     * something a caller can be told about; a hang is not. */
    if (NULL != prte_data_server_uri) {
        rc = prte_ds_relay(sender, room_number, command, buffer);
        if (PMIX_SUCCESS == rc) {
            /* the relay owns the request now and answers from a reply of
             * its own, so ours is surplus */
            PMIX_DATA_BUFFER_RELEASE(answer);
            return;
        }
        goto report;
    }

    /* From here on the handlers own "answer": each of them either sends it
     * or releases it and returns PMIX_SUCCESS. Anything else comes back to
     * us still holding the buffer, and we turn it into an error reply. */
    switch (command) {
        case PRTE_PMIX_PUBLISH_CMD:
            rc = prte_ds_publish(sender, buffer, answer);
            break;

        case PRTE_PMIX_LOOKUP_CMD:
            pmix_output_verbose(1, prte_data_store.output,
                                "%s data server: lookup data from %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PRTE_NAME_PRINT(sender));
            rc = prte_ds_lookup(sender, room_number,
                                buffer, answer);
            break;

        case PRTE_PMIX_UNPUBLISH_CMD:
            rc = prte_ds_unpublish(sender, buffer, answer);
            break;

        case PRTE_PMIX_PURGE_PROC_CMD:
            prte_ds_purge(sender, buffer, answer);
            return;

        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            rc = PRTE_ERR_BAD_PARAM;
            break;
    }

report:
    if (PMIX_SUCCESS != rc) {
        pmix_status_t ret;

        /* rc is a pmix_status_t here, so it takes the PMIx formatter -
         * PRTE_ERROR_NAME knows only PRRTE's codes and rendered every
         * status this reports as "Unknown error" */
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: sending error %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIx_Error_string(rc));
        /* pack the error code. Keep the pack status in its own variable:
         * overwriting rc with it lost the very code we were reporting, and
         * a failed pack then went out as a "successful" empty answer. */
        ret = PMIx_Data_pack(NULL, answer, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(answer);
            return;
        }
        PRTE_RML_RELIABLE_SEND(ret, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(answer);
        }
    }

}

void prte_ds_check_requestor(pmix_proc_t *owner, const pmix_info_t *info)
{
    prte_job_t *jdata;

    if (PMIX_PROC != info->value.type || NULL == info->value.data.proc) {
        return;
    }

    /* Acting for another process is what a RELAY does, and only a tool
     * relays: a daemon of another DVM attaches to us as a tool and
     * reissues the operation its own client asked for.  Anything else
     * claiming it is a process trying to publish - or unpublish - under
     * somebody else's name, so the claim is simply dropped and the
     * operation proceeds under the caller's own identity. */
    jdata = prte_get_job_data_object(owner->nspace);
    if (NULL == jdata || !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: %s is not a tool - ignoring its "
                            "claim to act for %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_NAME_PRINT(owner),
                            PMIX_NAME_PRINT(info->value.data.proc));
        return;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: %s is acting for %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(owner),
                        PMIX_NAME_PRINT(info->value.data.proc));
    PMIX_XFER_PROCID(owner, info->value.data.proc);
}

/* One range rule, applied in both directions.
 *
 * The PMIx retrieval rules for published data impose the range test
 * twice: the publisher's range says who may see the item, and the
 * requester's range says whose items it is willing to see.  Both ask the
 * same question - does "subject" fall within "range" as seen from
 * "anchor" - so both are answered here, with the caller deciding which
 * process plays which role.
 *
 * This is an ACCESS rule: it says who may read an item.  It is therefore
 * the wrong test for who may REMOVE one, which is a question of
 * ownership - see ds_unpublish.c. */
static pmix_status_t range_admits(pmix_data_range_t range,
                                  const pmix_proc_t *anchor,
                                  const pmix_proc_t *anchor_proxy,
                                  const pmix_proc_t *subject,
                                  const pmix_proc_t *subject_proxy)
{
    bool match;

    switch (range) {
    case PMIX_RANGE_UNDEF:
    case PMIX_RANGE_SESSION:
    case PMIX_RANGE_GLOBAL:
        // open to everyone
        match = true;
        break;

    case PMIX_RANGE_NAMESPACE:
        match = PMIX_CHECK_NSPACE(anchor->nspace, subject->nspace);
        break;

    case PMIX_RANGE_LOCAL:
        // the two must sit behind the same daemon
        match = PMIX_CHECK_PROCID(anchor_proxy, subject_proxy);
        break;

    case PMIX_RANGE_PROC_LOCAL:
        match = PMIX_CHECK_PROCID(anchor, subject);
        break;

    case PMIX_RANGE_RM:
        /* the subject must be the host environment - which means its
         * nspace must match that of the host's server, which is my own */
        match = PMIX_CHECK_NSPACE(subject->nspace, PRTE_PROC_MY_NAME->nspace);
        break;

    case PMIX_RANGE_CUSTOM:
        /* a CUSTOM range is the publisher's accessor list, and only a
         * publisher has one - see prte_data_server_check_range(), which
         * answers this case before we are reached.  In the other
         * direction, where the anchor is a requestor asking us to search,
         * there is no list to consult and nothing it could mean */
        match = false;
        break;

    default:
        match = false;
        break;
    }

    pmix_output_verbose(10, prte_data_store.output,
                        "%s\tRANGE %s ANCHOR %s SUBJECT %s: %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIx_Data_range_string(range),
                        PMIX_NAME_PRINT(anchor),
                        PMIX_NAME_PRINT(subject),
                        match ? "ADMIT" : "DENY");

    return match ? PMIX_SUCCESS : PMIX_ERROR;
}

/* the publisher's range: may this requestor see this item? */
pmix_status_t prte_data_server_check_range(prte_data_req_t *req,
                                           prte_data_object_t *data)
{
    if (PMIX_RANGE_CUSTOM == data->range) {
        /* for CUSTOM the accessor list IS the range - "available only to
         * processes as specified in the pmix_info_t associated with this
         * call".  So admit whoever the list admits, which
         * prte_data_server_check_access() decides, and refuse everyone
         * when the publisher named nobody: there is no other reading of a
         * custom range with no custom in it. */
        if (0 == data->nauids && 0 == data->nagids) {
            return PMIX_ERROR;
        }
        return PMIX_SUCCESS;
    }
    return range_admits(data->range, &data->owner, &data->proxy,
                        &req->requestor, &req->proxy);
}

/* the publisher's access permissions: may this requestor's uid and gid see
 * this item?  See the header for the rule; note that a list the publisher
 * gave is a REQUIREMENT, so a publisher whose own uid is not on its own
 * PMIX_ACCESS_USERIDS list cannot look its own data up either.  Removing it
 * is a separate question, answered by ownership - see ds_unpublish.c. */
pmix_status_t prte_data_server_check_access(prte_data_req_t *req,
                                            prte_data_object_t *data)
{
    size_t n;
    bool found;

    if (0 == data->nauids && 0 == data->nagids) {
        /* no accessors named: the data belongs to whoever published it */
        if (req->uid == data->uid && req->gid == data->gid) {
            return PMIX_SUCCESS;
        }
        pmix_output_verbose(10, prte_data_store.output,
                            "%s\tACCESS DENY owner %u/%u requestor %u/%u",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (unsigned) data->uid, (unsigned) data->gid,
                            (unsigned) req->uid, (unsigned) req->gid);
        return PMIX_ERR_NO_PERMISSIONS;
    }

    if (0 < data->nauids) {
        found = false;
        for (n = 0; n < data->nauids; n++) {
            if (data->auids[n] == req->uid) {
                found = true;
                break;
            }
        }
        if (!found) {
            pmix_output_verbose(10, prte_data_store.output,
                                "%s\tACCESS DENY uid %u not permitted",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                (unsigned) req->uid);
            return PMIX_ERR_NO_PERMISSIONS;
        }
    }

    if (0 < data->nagids) {
        found = false;
        for (n = 0; n < data->nagids; n++) {
            if (data->agids[n] == req->gid) {
                found = true;
                break;
            }
        }
        if (!found) {
            pmix_output_verbose(10, prte_data_store.output,
                                "%s\tACCESS DENY gid %u not permitted",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                (unsigned) req->gid);
            return PMIX_ERR_NO_PERMISSIONS;
        }
    }

    return PMIX_SUCCESS;
}

/* the requester's range: is this publisher one it asked to search? */
pmix_status_t prte_data_server_check_search_range(prte_data_req_t *req,
                                                  prte_data_object_t *data)
{
    return range_admits(req->range, &req->requestor, &req->proxy,
                        &data->owner, &data->proxy);
}

// CLASS INSTANCE
static void construct(prte_data_object_t *ptr)
{
    ptr->index = -1;
    /* proxy has to be initialized here as well as owner: it is what the
     * PMIX_RANGE_LOCAL check compares against, and PMIX_NEW does not zero
     * the allocation, so an object whose publisher never filled it in was
     * matching requestors against uninitialized memory */
    PMIX_PROC_CONSTRUCT(&ptr->proxy);
    PMIX_PROC_CONSTRUCT(&ptr->owner);
    ptr->uid = UINT32_MAX;
    ptr->gid = UINT32_MAX;
    ptr->auids = NULL;
    ptr->nauids = 0;
    ptr->agids = NULL;
    ptr->nagids = 0;
    ptr->range = PMIX_RANGE_SESSION;
    /* the Standard's default is PMIX_PERSIST_APP - "retain until the
     * application terminates".  PMIx adds no default of its own before
     * handing a publish to the host, so this is the one that governs */
    ptr->persistence = PMIX_PERSIST_APP;
    PMIX_CONSTRUCT(&ptr->info, pmix_list_t);
}

static void destruct(prte_data_object_t *ptr)
{
    if (NULL != ptr->auids) {
        free(ptr->auids);
    }
    if (NULL != ptr->agids) {
        free(ptr->agids);
    }
    PMIX_LIST_DESTRUCT(&ptr->info);
}

PMIX_CLASS_INSTANCE(prte_data_object_t,
                    pmix_object_t,
                    construct, destruct);


static void rqcon(prte_data_req_t *p)
{
    PMIX_PROC_CONSTRUCT(&p->proxy);
    PMIX_PROC_CONSTRUCT(&p->requestor);
    p->room_number = -1;
    p->keys = NULL;
    p->uid = UINT32_MAX;
    p->gid = UINT32_MAX;
    p->timer_active = false;
    /* the default range for a lookup or an unpublish is SESSION - the
     * same default the publish side carries */
    p->range = PMIX_RANGE_SESSION;
}
static void rqdes(prte_data_req_t *p)
{
    /* a parked request that is answered - or purged - before its timeout
     * fires still owns an armed event, and libevent must not be left
     * holding a pointer into freed storage */
    if (p->timer_active) {
        prte_event_evtimer_del(&p->ev);
        p->timer_active = false;
    }
    PMIx_Argv_free(p->keys);
}
PMIX_CLASS_INSTANCE(prte_data_req_t,
                    pmix_list_item_t,
                    rqcon, rqdes);


PMIX_CLASS_INSTANCE(prte_data_cleanup_t,
                    pmix_list_item_t,
                    NULL, NULL);


static void dsicon(prte_ds_info_t *p)
{
    PMIX_PROC_CONSTRUCT(&p->source);
    PMIX_INFO_CONSTRUCT(&p->info);
}
static void dsides(prte_ds_info_t *p)
{
    PMIX_INFO_DESTRUCT(&p->info);
}
PMIX_CLASS_INSTANCE(prte_ds_info_t,
                    pmix_list_item_t,
                    dsicon, dsides);

