/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Relay a data-server request to an EXTERNAL data server.
 *
 * When prte_data_server_uri names a server, this DVM does not store
 * published data at all - the named DVM does, so that jobs launched by
 * different invocations can find each other's data (what an MPI
 * application reaches through MPI_Publish_name / MPI_Comm_accept).
 *
 * That server is a DVM of its own, and its daemons are in a different
 * namespace, which the RML cannot address: every send entry point takes a
 * rank and stamps this daemon's own namespace on it, so a foreign daemon is
 * simply not nameable (see src/rml/AGENTS.md).  The crossing is therefore a
 * PMIx TOOL connection - this daemon attaches to the remote DVM's PMIx
 * server (pmix_server_pub.c, init_server) and reissues the request as an
 * ordinary PMIx_Publish/Lookup/Unpublish.  The remote DVM's own
 * publish/lookup upcalls then reach its data server exactly as a local
 * client's would.
 *
 * Only the DVM master attaches, so every request arrives here over the RML -
 * from another daemon, or from the master itself as a send to self - and
 * every answer goes back the same way, in the format
 * pmix_server_keyval_client() expects.  Nothing above this file knows
 * whether the data server was local or remote.
 *
 * The requesting process's identity is carried across in PMIX_REQUESTOR,
 * which the Standard defines for exactly this: "used when relaying a
 * request to the PMIx library on behalf of someone else where the API
 * doesn't include a requestor parameter".  Without it every item published
 * through the relay would be owned by this daemon's tool identity, and the
 * ownership rules at the far end - who may unpublish, what
 * PMIX_RANGE_NAMESPACE admits - would all be answered about the wrong
 * process.
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/data_server/ds.h"

/* one relayed request in flight */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    /* the daemon that asked us, and the room number it is waiting on */
    pmix_proc_t sender;
    int room_number;
    uint8_t command;
    /* what we tell it when the far end answers */
    pmix_status_t status;
    /* the directive array handed to PMIx - ours to free */
    pmix_info_t *info;
    size_t ninfo;
    char **keys;
    /* a lookup's results, copied out of the PMIx callback */
    pmix_pdata_t *pdata;
    size_t npdata;
} prte_ds_relay_t;

static void rlcon(prte_ds_relay_t *p)
{
    PMIX_PROC_CONSTRUCT(&p->sender);
    p->room_number = -1;
    p->command = 0;
    p->status = PMIX_SUCCESS;
    p->info = NULL;
    p->ninfo = 0;
    p->keys = NULL;
    p->pdata = NULL;
    p->npdata = 0;
}
static void rldes(prte_ds_relay_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    if (NULL != p->keys) {
        PMIx_Argv_free(p->keys);
    }
    if (NULL != p->pdata) {
        PMIX_PDATA_FREE(p->pdata, p->npdata);
    }
}
static PMIX_CLASS_INSTANCE(prte_ds_relay_t, pmix_object_t, rlcon, rldes);

/* Send the answer the requesting daemon is parked on.
 *
 * The format is the one every handler in this directory produces and
 * pmix_server_keyval_client() unpacks: the room number, the command, the
 * status, and - for a lookup that found something - a byte object holding
 * the count and then a (proc, info) pair per item.
 */
static void answer(prte_ds_relay_t *cd)
{
    pmix_data_buffer_t *reply, pbkt;
    pmix_byte_object_t pbo;
    pmix_status_t rc;
    size_t n;
    int ret;

    PMIX_DATA_BUFFER_CREATE(reply);

    rc = PMIx_Data_pack(NULL, reply, &cd->room_number, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    rc = PMIx_Data_pack(NULL, reply, &cd->command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    rc = PMIx_Data_pack(NULL, reply, &cd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }

    if (PRTE_PMIX_LOOKUP_CMD == cd->command && 0 < cd->npdata) {
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
        rc = PMIx_Data_pack(NULL, &pbkt, &cd->npdata, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        for (n = 0; n < cd->npdata; n++) {
            pmix_info_t item;

            rc = PMIx_Data_pack(NULL, &pbkt, &cd->pdata[n].proc, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            /* the wire carries a pmix_info_t per item, not a pdata - the
             * owner was packed above and the key/value is what remains */
            PMIX_INFO_CONSTRUCT(&item);
            PMIX_LOAD_KEY(item.key, cd->pdata[n].key);
            PMIX_VALUE_XFER_DIRECT(rc, &item.value, &cd->pdata[n].value);
            if (PMIX_SUCCESS == rc) {
                rc = PMIx_Data_pack(NULL, &pbkt, &item, 1, PMIX_INFO);
            }
            PMIX_INFO_DESTRUCT(&item);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
        }
        rc = PMIx_Data_unload(&pbkt, &pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        rc = PMIx_Data_pack(NULL, reply, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
    }

    PRTE_RML_RELIABLE_SEND(ret, cd->sender.rank, reply, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(reply);
    }
}

/* completion, on the PRRTE progress thread */
static void relay_complete(int sd, short args, void *cbdata)
{
    prte_ds_relay_t *cd = (prte_ds_relay_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server relay: %s answered %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&prte_pmix_server_globals.server),
                        PMIx_Error_string(cd->status));

    answer(cd);
    PMIX_RELEASE(cd);
}

/* PMIx completion for publish/unpublish/purge - runs on the PMIx progress
 * thread, so it captures the status and shifts */
static void opcb(pmix_status_t status, void *cbdata)
{
    prte_ds_relay_t *cd = (prte_ds_relay_t *) cbdata;

    cd->status = status;
    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, relay_complete);
}

/* PMIx completion for a lookup.  The pdata array belongs to PMIx and is
 * gone once this returns, so it is copied here rather than carried. */
static void lkcb(pmix_status_t status, pmix_pdata_t data[], size_t ndata,
                 void *cbdata)
{
    prte_ds_relay_t *cd = (prte_ds_relay_t *) cbdata;
    size_t n;

    cd->status = status;
    if (0 < ndata && NULL != data) {
        PMIX_PDATA_CREATE(cd->pdata, ndata);
        cd->npdata = ndata;
        for (n = 0; n < ndata; n++) {
            PMIX_PDATA_XFER(&cd->pdata[n], &data[n]);
        }
    }
    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, relay_complete);
}

/* Build the directive array we hand to PMIx: whatever the requester sent,
 * plus the requestor identity the far end needs to attribute the operation
 * correctly.  See the note at the head of this file. */
static pmix_status_t load_directives(prte_ds_relay_t *cd,
                                     pmix_info_t *info, size_t ninfo,
                                     pmix_proc_t *requestor)
{
    size_t n;

    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        cd->ninfo = 0;
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_XFER(&cd->info[n], &info[n]);
    }
    PMIX_INFO_LOAD(&cd->info[ninfo], PMIX_REQUESTOR, requestor, PMIX_PROC);
    return PMIX_SUCCESS;
}

/* unpack the trailing directive array, if the command carries one */
static pmix_status_t unpack_directives(pmix_data_buffer_t *buffer,
                                       pmix_info_t **info, size_t *ninfo)
{
    pmix_status_t rc;
    int32_t count;

    *info = NULL;
    *ninfo = 0;

    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        *ninfo = 0;
        return rc;
    }
    if (0 == *ninfo) {
        return PMIX_SUCCESS;
    }
    PMIX_INFO_CREATE(*info, *ninfo);
    count = (int32_t) *ninfo;
    rc = PMIx_Data_unpack(NULL, buffer, *info, &count, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_FREE(*info, *ninfo);
        *info = NULL;
        *ninfo = 0;
        return rc;
    }
    return PMIX_SUCCESS;
}

/* unpack a key array of the form the data-server commands use: a count
 * followed by that many strings */
static pmix_status_t unpack_keys(pmix_data_buffer_t *buffer, char ***keys)
{
    pmix_status_t rc;
    int32_t count;
    size_t n, nkeys;
    char *str;

    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nkeys, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    for (n = 0; n < nkeys; n++) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &str, &count, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Argv_free(*keys);
            *keys = NULL;
            return rc;
        }
        PMIx_Argv_append_nosize(keys, str);
        free(str);
    }
    return PMIX_SUCCESS;
}

pmix_status_t prte_ds_relay(pmix_proc_t *sender, int room_number,
                            uint8_t command, pmix_data_buffer_t *buffer)
{
    prte_ds_relay_t *cd;
    pmix_proc_t requestor;
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    pmix_status_t rc;
    int32_t count;

    /* the requestor leads every one of these commands */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    cd = PMIX_NEW(prte_ds_relay_t);
    PMIX_XFER_PROCID(&cd->sender, sender);
    cd->room_number = room_number;
    cd->command = command;

    switch (command) {
    case PRTE_PMIX_PUBLISH_CMD:
        rc = unpack_directives(buffer, &info, &ninfo);
        break;

    case PRTE_PMIX_LOOKUP_CMD:
    case PRTE_PMIX_UNPUBLISH_CMD:
        rc = unpack_keys(buffer, &cd->keys);
        if (PMIX_SUCCESS == rc) {
            rc = unpack_directives(buffer, &info, &ninfo);
        }
        break;

    case PRTE_PMIX_PURGE_PROC_CMD:
        /* no keys - a purge names only the process whose data is to go,
         * and the directives that came with it */
        rc = unpack_directives(buffer, &info, &ninfo);
        break;

    default:
        rc = PMIX_ERR_BAD_PARAM;
        break;
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }

    rc = load_directives(cd, info, ninfo, &requestor);
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }

    /* Point our client-side calls at the data server.  We may hold more
     * than one server connection - a scheduler is the other - and PMIx
     * sends a client operation to whichever one is currently primary, so
     * this is done per request and not once at startup. */
    rc = prte_pmix_set_primary_server(&prte_pmix_server_globals.server);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server relay: cmd %u for %s to %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) command,
                        PRTE_NAME_PRINT(&requestor),
                        PRTE_NAME_PRINT(&prte_pmix_server_globals.server));

    switch (command) {
    case PRTE_PMIX_PUBLISH_CMD:
        rc = PMIx_Publish_nb(cd->info, cd->ninfo, opcb, cd);
        break;

    case PRTE_PMIX_LOOKUP_CMD:
        rc = PMIx_Lookup_nb(cd->keys, cd->info, cd->ninfo, lkcb, cd);
        break;

    case PRTE_PMIX_UNPUBLISH_CMD:
        rc = PMIx_Unpublish_nb(cd->keys, cd->info, cd->ninfo, opcb, cd);
        break;

    case PRTE_PMIX_PURGE_PROC_CMD:
        /* a purge is an unpublish of everything the process owns, which
         * PMIx spells as an unpublish naming no keys */
        rc = PMIx_Unpublish_nb(NULL, cd->info, cd->ninfo, opcb, cd);
        break;

    default:
        rc = PMIX_ERR_BAD_PARAM;
        break;
    }

    if (PMIX_SUCCESS != rc) {
        /* the callback will not fire, so answer from here - the requester
         * is parked on this room number and nothing else will release it */
        PMIX_ERROR_LOG(rc);
        cd->status = rc;
        answer(cd);
        PMIX_RELEASE(cd);
        /* the request has been answered - our caller must not answer it
         * again */
        return PMIX_SUCCESS;
    }

    return PMIX_SUCCESS;
}
