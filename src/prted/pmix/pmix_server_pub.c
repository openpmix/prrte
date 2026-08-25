/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

/* Attach to the data server named by prte_data_server_uri.
 *
 * The server is a DVM of its own, so it is reached as a PMIx *tool
 * connection* and not over the RML: the RML addresses a peer by rank within
 * the sender's own namespace and cannot name a process of another DVM at
 * all (see src/rml/AGENTS.md).  What crosses the boundary is therefore an
 * ordinary PMIx publish/lookup/unpublish issued by this daemon acting as a
 * tool of the remote DVM, and the URI this wants is that DVM's PMIx server
 * URI - what `prte --report-uri` writes out.
 *
 * Only the DVM master attaches.  Every other daemon relays to it over the
 * RML exactly as it does for a data server hosted here, so a DVM holds one
 * connection to the external server however many daemons it has.
 */
static int init_server(void);

/* Attach to the external data server if we have not already.
 *
 * This used to happen only inside execute() below - that is, only when a
 * LOCAL client of this daemon published or looked something up.  The master
 * needs the connection for a different reason: every other daemon relays its
 * requests to the master, and the master is the only one that holds the tool
 * connection to the far end.  A master with no publishing client of its own
 * therefore never attached, and the relay failed PMIX_ERR_UNREACH for every
 * request the DVM made.  Nothing noticed, because a job's nspace
 * registration used to publish through execute() and attach as a side
 * effect. */
int prte_pmix_server_init_pubsub(void)
{
    int ret;

    if (prte_pmix_server_globals.pubsub_init) {
        return PRTE_SUCCESS;
    }
    ret = init_server();
    if (PRTE_SUCCESS != ret) {
        prte_show_help("help-prted.txt", "noserver", true,
                       (NULL == prte_data_server_uri) ? "NULL" : prte_data_server_uri);
    }
    return ret;
}

static int init_server(void)
{
    char *server;
    char input[1024], *filename;
    FILE *fp;
    pmix_status_t ret;
    pmix_info_t info[2];

    /* only do this once */
    prte_pmix_server_globals.pubsub_init = true;

    /* if the universal server wasn't specified, then we use
     * our own HNP for that purpose */
    if (NULL == prte_data_server_uri) {
        prte_pmix_server_globals.server = *PRTE_PROC_MY_HNP;
    } else if (!PRTE_PROC_IS_MASTER) {
        /* our master holds the connection - everything we cannot serve
         * ourselves goes to it */
        prte_pmix_server_globals.server = *PRTE_PROC_MY_HNP;
    } else {
        if (0 == strncmp(prte_data_server_uri, "file", strlen("file")) ||
            0 == strncmp(prte_data_server_uri, "FILE", strlen("FILE"))) {
            /* it is a file - get the filename */
            filename = strchr(prte_data_server_uri, ':');
            if (NULL == filename) {
                /* filename is not correctly formatted */
                prte_show_help("help-prun.txt", "prun:ompi-server-filename-bad", true,
                               prte_tool_basename, prte_data_server_uri);
                return PRTE_ERR_BAD_PARAM;
            }
            ++filename; /* space past the : */

            if (0 >= strlen(filename)) {
                /* they forgot to give us the name! */
                prte_show_help("help-prun.txt", "prun:ompi-server-filename-missing", true,
                               prte_tool_basename, prte_data_server_uri);
                return PRTE_ERR_BAD_PARAM;
            }

            /* open the file and extract the uri */
            fp = fopen(filename, "r");
            if (NULL == fp) { /* can't find or read file! */
                prte_show_help("help-prun.txt", "prun:ompi-server-filename-access", true,
                               prte_tool_basename, prte_data_server_uri);
                return PRTE_ERR_BAD_PARAM;
            }
            if (NULL == fgets(input, 1024, fp)) {
                /* something malformed about file */
                fclose(fp);
                prte_show_help("help-prun.txt", "prun:ompi-server-file-bad", true,
                               prte_tool_basename, prte_data_server_uri, prte_tool_basename);
                return PRTE_ERR_BAD_PARAM;
            }
            fclose(fp);
            input[strlen(input) - 1] = '\0'; /* remove newline */
            server = strdup(input);
        } else {
            server = strdup(prte_data_server_uri);
        }
        /* check if we are to wait for the server to start - resolves
         * a race condition that can occur when the server is run
         * as a background job - e.g., in scripts
         */
        if (prte_pmix_server_globals.wait_for_server) {
            /* just hang loose */
            struct timespec timeout = {prte_pmix_server_globals.timeout, 0};
            nanosleep(&timeout, NULL);
        }

        /* attach to it.  PMIX_WAIT_FOR_CONNECTION is deliberately not set:
         * the wait above is what the user asked for, and a server that is
         * not there is an error we want reported now rather than a hang. */
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, server, PMIX_STRING);
        PMIX_INFO_LOAD(&info[1], PMIX_PRIMARY_SERVER, NULL, PMIX_BOOL);
        ret = PMIx_tool_attach_to_server(NULL, &prte_pmix_server_globals.server,
                                         info, 2);
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        free(server);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            return prte_pmix_convert_status(ret);
        }
        /* the attach asked for it to be made primary, so the library has
         * already done what prte_pmix_set_primary_server would - record
         * that, or the tracker's next comparison is against nothing */
        PMIX_XFER_PROCID(&prte_pmix_server_globals.primary_server,
                         &prte_pmix_server_globals.server);
        prte_pmix_server_globals.primary_server_set = true;

        pmix_output_verbose(1, prte_pmix_server_globals.output,
                            "%s attached to external data server %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_NAME_PRINT(&prte_pmix_server_globals.server));
    }

    return PRTE_SUCCESS;
}

static void execute(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;
    int rc;
    pmix_data_buffer_t *xfer;
    pmix_proc_t *target;
    /* the DVM's store unless the range says otherwise */
    prte_rml_tag_t dstag = PRTE_RML_TAG_DATA_SERVER;
    bool stored = false;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(req);

    /* we need our connection to the server */
    if (PRTE_SUCCESS != (rc = prte_pmix_server_init_pubsub())) {
        goto callback;
    }

    /* add this request to our tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
    stored = true;

    /* setup the xfer */
    PMIX_DATA_BUFFER_CREATE(xfer);

    /* pack the room number */
    rc = PMIx_Data_pack(NULL, xfer, &req->local_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(xfer);
        goto callback;
    }
    rc = PMIx_Data_copy_payload(xfer, &req->msg);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(xfer);
        goto callback;
    }

    /* if the range is SESSION, then set the target to the global server.
     * When that server is an external DVM it is not addressable over the
     * RML at all, and only the master holds the connection to it - so the
     * request goes to the master either way, and the master relays it from
     * prte_data_server().  At the master itself that is a send to self,
     * which the RML posts straight back for receipt. */
    if (PMIX_RANGE_SESSION == req->range) {
        pmix_output_verbose(1, prte_pmix_server_globals.output,
                            "%s orted:pmix:server range SESSION",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        target = (NULL == prte_data_server_uri) ? &prte_pmix_server_globals.server
                                                : PRTE_PROC_MY_HNP;
    } else if (PMIX_RANGE_LOCAL == req->range) {
        /* if the range is local, send it to myself - and say so with the
         * tag, so that a DVM pointed at an external data server serves this
         * out of its own store instead of relaying it away */
        pmix_output_verbose(1, prte_pmix_server_globals.output, "%s orted:pmix:server range LOCAL",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        target = PRTE_PROC_MY_NAME;
        dstag = PRTE_RML_TAG_DATA_SERVER_LOCAL;
    } else {
        pmix_output_verbose(1, prte_pmix_server_globals.output, "%s orted:pmix:server range GLOBAL",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        target = PRTE_PROC_MY_HNP;
    }

    /* send the request to the target */
    PRTE_RML_RELIABLE_SEND(rc, target->rank, xfer, dstag);
    if (PRTE_SUCCESS == rc) {
        return;
    }
    PRTE_ERROR_LOG(rc);
    rc = prte_pmix_convert_rc(rc);

callback:
    /* execute the callback to avoid having the client hang */
    if (NULL != req->opcbfunc) {
        req->opcbfunc(rc, req->cbdata);
    } else if (NULL != req->lkcbfunc) {
        req->lkcbfunc(rc, NULL, 0, req->cbdata);
    }
    if (stored) {
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    }
    PMIX_RELEASE(req);
}

pmix_status_t pmix_server_publish_fn(const pmix_proc_t *proc, const pmix_info_t info[],
                                     size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;
    pmix_status_t rc;
    int ret;
    uint8_t cmd = PRTE_PMIX_PUBLISH_CMD;
    size_t n;

    pmix_output_verbose(1, prte_pmix_server_globals.output, "%s orted:pmix:server PUBLISH",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* create the caddy */
    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "PUBLISH: %s:%d", __FILE__, __LINE__);
    req->opcbfunc = cbfunc;
    req->cbdata = cbdata;

    /* load the command */
    ret = PMIx_Data_pack(NULL, &req->msg, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(req);
        return PMIX_ERR_PACK_FAILURE;
    }

    /* no help for it - need to search for range/persistence */
    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
            req->range = info[n].value.data.range;
        } else if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
            req->timeout = info[n].value.data.integer;
        }
    }

    /* pack the name of the publisher */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_proc_t *) proc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    /* pack the number of infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    /* pack the infos */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_info_t *) info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    /* thread-shift so we can store the tracker */
    prte_event_set(prte_event_base, &(req->ev), -1, PRTE_EV_WRITE, execute, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

pmix_status_t pmix_server_lookup_fn(const pmix_proc_t *proc, char **keys, const pmix_info_t info[],
                                    size_t ninfo, pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;
    int ret;
    uint8_t cmd = PRTE_PMIX_LOOKUP_CMD;
    size_t m, n;
    pmix_status_t rc;

    if (NULL == keys || 0 == PMIx_Argv_count(keys)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create the caddy */
    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "LOOKUP: %s:%d", __FILE__, __LINE__);
    req->lkcbfunc = cbfunc;
    req->cbdata = cbdata;

    /* load the command */
    if (PRTE_SUCCESS != (ret = PMIx_Data_pack(NULL, &req->msg, &cmd, 1, PMIX_UINT8))) {
        PRTE_ERROR_LOG(ret);
        PMIX_RELEASE(req);
        return PMIX_ERR_PACK_FAILURE;
    }

    /* no help for it - need to search for range and timeout */
    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
            req->range = info[n].value.data.range;
        } else if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
            req->timeout = info[n].value.data.integer;
        }
    }

    /* pack the name of the requestor */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_proc_t *) proc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    /* pack the number of keys */
    n = PMIx_Argv_count(keys);
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &n, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }
    /* pack the keys */
    for (m = 0; NULL != keys[m]; m++) {
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &keys[m], 1, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
    }

    /* pack the number of infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    if (0 < ninfo) {
        /* pack the infos */
        if (PMIX_SUCCESS
            != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_info_t *) info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
    }

    /* thread-shift so we can store the tracker */
    prte_event_set(prte_event_base, &(req->ev), -1, PRTE_EV_WRITE, execute, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

pmix_status_t pmix_server_unpublish_fn(const pmix_proc_t *proc, char **keys,
                                       const pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;
    int ret;
    uint8_t cmd;
    size_t m, n;
    pmix_status_t rc;

    // check for a "purge" command
    if (NULL == keys) {
        /* create the caddy */
        req = PMIX_NEW(prte_pmix_server_req_t);
        pmix_asprintf(&req->operation, "PURGE: %s:%d", __FILE__, __LINE__);
        req->opcbfunc = cbfunc;
        req->cbdata = cbdata;

        /* load the command */
        cmd = PRTE_PMIX_PURGE_PROC_CMD;
        if (PRTE_SUCCESS != (ret = PMIx_Data_pack(NULL, &req->msg, &cmd, 1, PMIX_UINT8))) {
            PRTE_ERROR_LOG(ret);
            PMIX_RELEASE(req);
            return PMIX_ERR_PACK_FAILURE;
        }

        /* pack the name of the requestor */
        if (PMIX_SUCCESS
            != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_proc_t *) proc, 1, PMIX_PROC))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }

        /* pack the directives.  A purge takes none of its own, but a
         * relayed one carries PMIX_REQUESTOR naming the process whose data
         * is actually to go - without it the data server would purge
         * everything owned by the relaying tool. */
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
        if (0 < ninfo) {
            if (PMIX_SUCCESS
                != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_info_t *) info, ninfo,
                                        PMIX_INFO))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(req);
                return rc;
            }
        }

        /* thread-shift so we can store the tracker */
        prte_event_set(prte_event_base, &(req->ev), -1, PRTE_EV_WRITE, execute, req);
        PMIX_POST_OBJECT(req);
        prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

        return PRTE_SUCCESS;
    }


    /* create the caddy */
    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "UNPUBLISH: %s:%d", __FILE__, __LINE__);
    req->opcbfunc = cbfunc;
    req->cbdata = cbdata;

    /* load the command */
    cmd = PRTE_PMIX_UNPUBLISH_CMD;
    if (PRTE_SUCCESS != (ret = PMIx_Data_pack(NULL, &req->msg, &cmd, 1, PMIX_UINT8))) {
        PRTE_ERROR_LOG(ret);
        PMIX_RELEASE(req);
        return PMIX_ERR_PACK_FAILURE;
    }

    /* no help for it - need to search for range and timeout */
    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
            req->range = info[n].value.data.range;
        } else if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
            req->timeout = info[n].value.data.integer;
        }
    }

    /* pack the name of the requestor */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_proc_t *) proc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    /* pack the number of keys */
    n = PMIx_Argv_count(keys);
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &n, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }
    /* pack the keys */
    for (m = 0; m < n; m++) {
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &keys[m], 1, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
    }

    /* pack the number of infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &req->msg, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }

    if (0 < ninfo) {
        /* pack the infos */
        if (PMIX_SUCCESS
            != (rc = PMIx_Data_pack(NULL, &req->msg, (pmix_info_t *) info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
    }

    /* thread-shift so we can store the tracker */
    prte_event_set(prte_event_base, &(req->ev), -1, PRTE_EV_WRITE, execute, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

void pmix_server_keyval_client(int status, pmix_proc_t *sender,
                               pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata)
{
    uint8_t command;
    int rc, room_num = -1;
    int32_t cnt;
    prte_pmix_server_req_t *req = NULL;
    pmix_byte_object_t bo;
    pmix_data_buffer_t pbkt;
    pmix_status_t ret = PMIX_SUCCESS;
    pmix_info_t info;
    pmix_pdata_t *pdata = NULL;
    size_t n, npdata = 0;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    pmix_output_verbose(1, prte_pmix_server_globals.output,
                        "%s recvd data server return",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* unpack the room number of the request tracker */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &room_num, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        ret = PMIX_ERR_UNPACK_FAILURE;
        goto release;
    }

    /* unpack the command */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &command, &cnt, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto release;
    }

    /* these answers carry no payload: nothing was found, nothing could be
     * shown to this requestor, the wait ran out, or the command is one that
     * returns only a status */
    if (PMIX_ERR_NOT_FOUND == ret ||
        PMIX_ERR_NO_PERMISSIONS == ret ||
        PMIX_ERR_TIMEOUT == ret ||
        PRTE_PMIX_UNPUBLISH_CMD == command ||
        PRTE_PMIX_PUBLISH_CMD == command) {
        goto release;
    }

    /* unpack the byte object payload */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &bo, &cnt, PMIX_BYTE_OBJECT);
    /* there may not be anything returned here - e.g., a publish
     * command will not return any data if no matching pending
     * requests were found */
    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            // not necessarily an error, so don't log it
            goto release;
        }
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto release;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    rc = PMIx_Data_load(&pbkt, &bo);
    bo.bytes = NULL;
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto release;
    }

    /* unpack the number of data items */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &pbkt, &npdata, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        ret = rc;
        goto release;
    }

    if (0 < npdata) {
        PMIX_PDATA_CREATE(pdata, npdata);
        for (n = 0; n < npdata; n++) {
            PMIX_INFO_CONSTRUCT(&info);
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &pbkt, &pdata[n].proc, &cnt, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_DESTRUCT(&info);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                ret = rc;
                goto release;
            }
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &pbkt, &info, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_DESTRUCT(&info);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                ret = rc;
                goto release;
            }
            PMIX_LOAD_KEY(pdata[n].key, info.key);
            PMIX_VALUE_XFER_DIRECT(rc, &pdata[n].value, &info.value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_DESTRUCT(&info);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                ret = rc;
                goto release;
            }
            PMIX_INFO_DESTRUCT(&info);
        }
    }
    /* the payload was loaded into this buffer, which owns it from that point
     * on - every error arm above lets it go and the way out did not, so a
     * whole lookup response leaked on each one that worked */
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

release:
    if (0 <= room_num) {
        req = (prte_pmix_server_req_t*)pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, room_num);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, room_num, NULL);
    }

    if (NULL != req) {
        /* pass down the response */
        if (NULL != req->opcbfunc) {
            req->opcbfunc(ret, req->cbdata);
        } else if (NULL != req->lkcbfunc) {
            req->lkcbfunc(ret, pdata, npdata, req->cbdata);
        } else {
            /* should not happen */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
        }

        /* cleanup */
        PMIX_RELEASE(req);
    }
    if (NULL != pdata) {
        PMIX_PDATA_FREE(pdata, npdata);
    }
}
