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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prte_data_server.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/rml_contact.h"

#include "src/prted/pmix/pmix_server_internal.h"

static int init_server(void)
{
    char *server;
    pmix_proc_t proc;
    pmix_value_t val;
    char input[1024], *filename;
    FILE *fp;
    int rc;
    pmix_status_t ret;

    /* only do this once */
    prte_pmix_server_globals.pubsub_init = true;

    /* if the universal server wasn't specified, then we use
     * our own HNP for that purpose */
    if (NULL == prte_data_server_uri) {
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
                               prte_tool_basename, prte_data_server_uri,
                               prte_tool_basename);
                return PRTE_ERR_BAD_PARAM;
            }
            fclose(fp);
            input[strlen(input)-1] = '\0';  /* remove newline */
            server = strdup(input);
        } else {
            server = strdup(prte_data_server_uri);
        }
        /* parse the URI to get the server's name */
        if (PRTE_SUCCESS != (rc = prte_rml_base_parse_uris(server, &prte_pmix_server_globals.server, NULL))) {
            PRTE_ERROR_LOG(rc);
            free(server);
            return rc;
        }
        /* setup our route to the server */
        PRTE_PMIX_CONVERT_NAME(rc, &proc, &prte_pmix_server_globals.server);
        PMIX_VALUE_LOAD(&val, server, PMIX_STRING);
        if (PMIX_SUCCESS != (ret = PMIx_Store_internal(&proc, PMIX_PROC_URI, &val))) {
            PMIX_ERROR_LOG(ret);
            PMIX_VALUE_DESTRUCT(&val);
            return rc;
        }
        PMIX_VALUE_DESTRUCT(&val);

        /* check if we are to wait for the server to start - resolves
         * a race condition that can occur when the server is run
         * as a background job - e.g., in scripts
         */
        if (prte_pmix_server_globals.wait_for_server) {
            /* ping the server */
            struct timeval timeout;
            timeout.tv_sec = prte_pmix_server_globals.timeout;
            timeout.tv_usec = 0;
            if (PRTE_SUCCESS != (rc = prte_rml.ping(server, &timeout))) {
                /* try it one more time */
                if (PRTE_SUCCESS != (rc = prte_rml.ping(server, &timeout))) {
                    /* okay give up */
                    prte_show_help("help-prun.txt", "prun:server-not-found", true,
                                   prte_tool_basename, server,
                                   (long)prte_pmix_server_globals.timeout,
                                   PRTE_ERROR_NAME(rc));
                    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                    return rc;
                }
            }
        }
    }

    return PRTE_SUCCESS;
}

static void execute(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    int rc;
    prte_buffer_t *xfer;
    prte_process_name_t *target;

    PRTE_ACQUIRE_OBJECT(req);

    if (!prte_pmix_server_globals.pubsub_init) {
        /* we need to initialize our connection to the server */
        if (PRTE_SUCCESS != (rc = init_server())) {
            prte_show_help("help-orted.txt", "noserver", true,
                           (NULL == prte_data_server_uri) ?
                           "NULL" : prte_data_server_uri);
            goto callback;
        }
    }

    /* add this request to our tracker hotel */
    if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
        prte_show_help("help-orted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
        goto callback;
    }

    /* setup the xfer */
    xfer = PRTE_NEW(prte_buffer_t);
    /* pack the room number */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(xfer, &req->room_num, 1, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(xfer);
        goto callback;
    }
    prte_dss.copy_payload(xfer, &req->msg);

    /* if the range is SESSION, then set the target to the global server */
    if (PMIX_RANGE_SESSION == req->range) {
        prte_output_verbose(1, prte_pmix_server_globals.output,
                            "%s orted:pmix:server range SESSION",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        target = &prte_pmix_server_globals.server;
    } else if (PMIX_RANGE_LOCAL == req->range) {
        /* if the range is local, send it to myself */
        prte_output_verbose(1, prte_pmix_server_globals.output,
                            "%s orted:pmix:server range LOCAL",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        target = PRTE_PROC_MY_NAME;
    } else {
        prte_output_verbose(1, prte_pmix_server_globals.output,
                            "%s orted:pmix:server range GLOBAL",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        target = PRTE_PROC_MY_HNP;
    }

    /* send the request to the target */
    rc = prte_rml.send_buffer_nb(target, xfer,
                                 PRTE_RML_TAG_DATA_SERVER,
                                 prte_rml_send_callback, NULL);
    if (PRTE_SUCCESS == rc) {
        return;
    }

  callback:
    /* execute the callback to avoid having the client hang */
    if (NULL != req->opcbfunc) {
        req->opcbfunc(rc, req->cbdata);
    } else if (NULL != req->lkcbfunc) {
        req->lkcbfunc(rc, NULL, 0, req->cbdata);
    }
    prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
    PRTE_RELEASE(req);
}

pmix_status_t pmix_server_publish_fn(const pmix_proc_t *proc,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata){
    pmix_server_req_t *req;
    pmix_status_t rc;
    int ret;
    uint8_t cmd = PRTE_PMIX_PUBLISH_CMD;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    prte_byte_object_t bo, *boptr;
    size_t n;

    prte_output_verbose(1, prte_pmix_server_globals.output,
                        "%s orted:pmix:server PUBLISH",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* create the caddy */
    req = PRTE_NEW(pmix_server_req_t);
    prte_asprintf(&req->operation, "PUBLISH: %s:%d", __FILE__, __LINE__);
    req->opcbfunc = cbfunc;
    req->cbdata = cbdata;

    /* load the command */
    if (PRTE_SUCCESS != (ret = prte_dss.pack(&req->msg, &cmd, 1, PRTE_UINT8))) {
        PRTE_ERROR_LOG(ret);
        PRTE_RELEASE(req);
        return PMIX_ERR_PACK_FAILURE;
    }

    /* no help for it - need to search for range/persistence */
    for(n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
            req->range = info[n].value.data.range;
        } else if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
            req->timeout = info[n].value.data.integer;
        }
    }

    /* setup a pmix_data_buffer_t to hold the message */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    /* I will be sending it */

    /* pack the name of the publisher */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_proc_t*)proc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }

    /* pack the number of infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }

    /* pack the infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_info_t*)info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }

    /* unload the pmix buffer */
    PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
    bo.bytes = (uint8_t*)pbo.bytes;
    bo.size = pbo.size;

    /* pack it into our msg */
    boptr = &bo;
    if (PRTE_SUCCESS != (ret = prte_dss.pack(&req->msg, &boptr, 1, PRTE_BYTE_OBJECT))) {
        PRTE_ERROR_LOG(ret);
        free(bo.bytes);
        return PMIX_ERR_PACK_FAILURE;
    }
    free(bo.bytes);

    /* thread-shift so we can store the tracker */
    prte_event_set(prte_event_base, &(req->ev),
                   -1, PRTE_EV_WRITE, execute, req);
    prte_event_set_priority(&(req->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;

}

pmix_status_t pmix_server_lookup_fn(const pmix_proc_t *proc, char **keys,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    pmix_server_req_t *req;
    int ret;
    uint8_t cmd = PRTE_PMIX_LOOKUP_CMD;
    size_t m, n;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    prte_byte_object_t bo, *boptr;
    pmix_status_t rc;

    if (NULL == keys || 0 == prte_argv_count(keys)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create the caddy */
    req = PRTE_NEW(pmix_server_req_t);
    prte_asprintf(&req->operation, "LOOKUP: %s:%d", __FILE__, __LINE__);
    req->lkcbfunc = cbfunc;
    req->cbdata = cbdata;

    /* load the command */
    if (PRTE_SUCCESS != (ret = prte_dss.pack(&req->msg, &cmd, 1, PRTE_UINT8))) {
        PRTE_ERROR_LOG(ret);
        PRTE_RELEASE(req);
        return PMIX_ERR_PACK_FAILURE;
    }

    /* no help for it - need to search for range and timeout */
   for(n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
            req->range = info[n].value.data.range;
        } else if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
            req->timeout = info[n].value.data.integer;
        }
    }

    /* setup a pmix_data_buffer_t to hold the message */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* pack the name of the requestor */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_proc_t*)proc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }

    /* pack the number of keys */
    n = prte_argv_count(keys);
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &n, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }
    /* pack the keys */
    for (m=0; NULL != keys[m]; m++) {
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &keys[m], 1, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            PRTE_RELEASE(req);
            return rc;
        }
    }

    /* pack the number of infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }

    /* pack the infos */
    if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_info_t*)info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(req);
        return rc;
    }

    /* unload the pmix buffer */
    PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
    bo.bytes = (uint8_t*)pbo.bytes;
    bo.size = pbo.size;

    /* pack it into our msg */
    boptr = &bo;
    if (PRTE_SUCCESS != (ret = prte_dss.pack(&req->msg, &boptr, 1, PRTE_BYTE_OBJECT))) {
        PRTE_ERROR_LOG(ret);
        free(bo.bytes);
        return PMIX_ERR_PACK_FAILURE;
    }
    free(bo.bytes);

    /* thread-shift so we can store the tracker */
    prte_event_set(prte_event_base, &(req->ev),
                   -1, PRTE_EV_WRITE, execute, req);
    prte_event_set_priority(&(req->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

pmix_status_t pmix_server_unpublish_fn(const pmix_proc_t *proc, char **keys,
                                       const pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_server_req_t *req;
    int ret;
    uint8_t cmd = PRTE_PMIX_UNPUBLISH_CMD;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    prte_byte_object_t bo, *boptr;
    size_t m, n;
    pmix_status_t rc;

    /* create the caddy */
    req = PRTE_NEW(pmix_server_req_t);
    prte_asprintf(&req->operation, "UNPUBLISH: %s:%d", __FILE__, __LINE__);
    req->opcbfunc = cbfunc;
    req->cbdata = cbdata;

    /* load the command */
    if (PRTE_SUCCESS != (ret = prte_dss.pack(&req->msg, &cmd, 1, PRTE_UINT8))) {
        PRTE_ERROR_LOG(ret);
        PRTE_RELEASE(req);
        return PMIX_ERR_PACK_FAILURE;
    }

     /* no help for it - need to search for range and timeout */
    for(n=0; n < ninfo; n++) {
         if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
             req->range = info[n].value.data.range;
         } else if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
             req->timeout = info[n].value.data.integer;
         }
     }

     /* setup a pmix_data_buffer_t to hold the message */
     PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

     /* pack the name of the requestor */
     if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_proc_t*)proc, 1, PMIX_PROC))) {
         PMIX_ERROR_LOG(rc);
         PRTE_RELEASE(req);
         return rc;
     }

     /* pack the number of keys */
     n = prte_argv_count(keys);
     if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &n, 1, PMIX_SIZE))) {
         PMIX_ERROR_LOG(rc);
         PRTE_RELEASE(req);
         return rc;
     }
     /* pack the keys */
     for (m=0; m < n; m++) {
         if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &keys[m], 1, PMIX_STRING))) {
             PMIX_ERROR_LOG(rc);
             PRTE_RELEASE(req);
             return rc;
         }
     }

     /* pack the number of infos */
     if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, &ninfo, 1, PMIX_SIZE))) {
         PMIX_ERROR_LOG(rc);
         PRTE_RELEASE(req);
         return rc;
     }

     /* pack the infos */
     if (PMIX_SUCCESS != (rc = PMIx_Data_pack(&prte_process_info.myproc, &pbkt, (pmix_info_t*)info, ninfo, PMIX_INFO))) {
         PMIX_ERROR_LOG(rc);
         PRTE_RELEASE(req);
         return rc;
     }

     /* unload the pmix buffer */
     PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
     bo.bytes = (uint8_t*)pbo.bytes;
     bo.size = pbo.size;

     /* pack it into our msg */
     boptr = &bo;
     if (PRTE_SUCCESS != (ret = prte_dss.pack(&req->msg, &boptr, 1, PRTE_BYTE_OBJECT))) {
         PRTE_ERROR_LOG(ret);
         free(bo.bytes);
         return PMIX_ERR_PACK_FAILURE;
     }
     free(bo.bytes);

    /* thread-shift so we can store the tracker */
    prte_event_set(prte_event_base, &(req->ev),
                   -1, PRTE_EV_WRITE, execute, req);
    prte_event_set_priority(&(req->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

void pmix_server_keyval_client(int status, prte_process_name_t* sender,
                               prte_buffer_t *buffer,
                               prte_rml_tag_t tg, void *cbdata)
{
    uint8_t command;
    int rc, room_num = -1;
    int32_t cnt;
    pmix_server_req_t *req=NULL;
    prte_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;
    pmix_status_t ret = PMIX_SUCCESS, rt = PMIX_SUCCESS;
    pmix_info_t info;
    pmix_pdata_t *pdata = NULL;
    size_t n, npdata = 0;

    prte_output_verbose(1, prte_pmix_server_globals.output,
                        "%s recvd lookup data return",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* unpack the room number of the request tracker */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &room_num, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        ret = PMIX_ERR_UNPACK_FAILURE;
        goto release;
    }

    /* unpack the command */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &command, &cnt, PRTE_UINT8))) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &status, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        ret = PMIX_ERR_UNPACK_FAILURE;
        goto release;
    }

    if (PRTE_ERR_NOT_FOUND == status) {
        ret = PMIX_ERR_NOT_FOUND;
        goto release;
    } else if (PRTE_ERR_PARTIAL_SUCCESS == status) {
        rt = PMIX_QUERY_PARTIAL_SUCCESS;
    } else {
        ret = PMIX_SUCCESS;
    }
    if (PRTE_PMIX_UNPUBLISH_CMD == command) {
        /* nothing else will be included */
        goto release;
    }

    /* unpack the byte object payload */
    cnt = 1;
    rc = prte_dss.unpack(buffer, &boptr, &cnt, PRTE_BYTE_OBJECT);
    /* there may not be anything returned here - e.g., a publish
     * command will not return any data if no matching pending
     * requests were found */
    if (PMIX_SUCCESS != rc) {
        if (PMIX_SUCCESS == ret) {
            ret = rt;
        }
        goto release;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    boptr->bytes = NULL;
    free(boptr);

    /* unpack the number of data items */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &npdata, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        goto release;
    }

    if (0 < npdata) {
        PMIX_PDATA_CREATE(pdata, npdata);
        for (n=0; n < npdata; n++) {
            PMIX_INFO_CONSTRUCT(&info);
            cnt = 1;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &pdata[n].proc, &cnt, PMIX_PROC))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                goto release;
            }
            cnt = 1;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, &info, &cnt, PMIX_INFO))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                goto release;
            }
            PMIX_LOAD_KEY(pdata[n].key, info.key);
            pmix_value_xfer(&pdata[n].value, &info.value);
            PMIX_INFO_DESTRUCT(&info);
        }
    }
    if (PMIX_SUCCESS == ret) {
        ret = rt;
    }

  release:
    if (0 <= room_num) {
        /* retrieve the tracker */
        prte_hotel_checkout_and_return_occupant(&prte_pmix_server_globals.reqs, room_num, (void**)&req);
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
        PRTE_RELEASE(req);
    }
    if (NULL != pdata) {
        PMIX_PDATA_FREE(pdata, npdata);
    }
}
