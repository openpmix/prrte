/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"


static int pack_xcast(pmix_data_buffer_t *buffer,
                      pmix_data_buffer_t *message, prte_rml_tag_t tag);

int prte_grpcomm_direct_xcast(prte_rml_tag_t tag,
                              pmix_data_buffer_t *msg)
{
    int rc;
    pmix_data_buffer_t *buf;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) msg->bytes_used));

    /* this function does not access any framework-global data, and
     * so it does not require us to push it into the event library */

    /* prep the output buffer */
    PMIX_DATA_BUFFER_CREATE(buf);

    /* setup the payload */
    if (PRTE_SUCCESS != (rc = pack_xcast(buf, msg, tag))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* send it to the HNP (could be myself) for relay */
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_XCAST);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    return PRTE_SUCCESS;
}

void prte_grpcomm_direct_xcast_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tg, void *cbdata)
{
    prte_routed_tree_t *nm;
    int ret, cnt;
    pmix_data_buffer_t *relay = NULL, *rly, *rlycopy;
    pmix_data_buffer_t datbuf, *data;
    bool compressed;
    prte_job_t *daemons;
    pmix_list_t coll;
    prte_rml_tag_t tag;
    pmix_byte_object_t bo, pbo;
    pmix_value_t val;
    pmix_proc_t dmn;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) buffer->bytes_used));

    /* we need a passthru buffer to send to our children - we leave it
     * as compressed data */
    PMIX_DATA_BUFFER_CREATE(rly);
    ret = PMIx_Data_copy_payload(rly, buffer);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&datbuf);
    /* setup the relay list */
    PMIX_CONSTRUCT(&coll, pmix_list_t);

    /* unpack the flag to see if this payload is compressed */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &compressed, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PMIX_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    /* unpack the data blob */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    if (compressed) {
        /* decompress the data */
        if (PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size,
                                 (uint8_t **) &bo.bytes, &bo.size)) {
            /* the data has been uncompressed */
            ret = PMIx_Data_load(&datbuf, &bo);
            if (PMIX_SUCCESS != ret) {
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                PMIX_DESTRUCT(&coll);
                PMIX_DATA_BUFFER_RELEASE(rly);
                return;
            }
        } else {
            pmix_show_help("help-prte-runtime.txt", "failed-to-uncompress",
                           true, prte_process_info.nodename);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PMIX_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            return;
        }
    } else {
        ret = PMIx_Data_load(&datbuf, &pbo);
        if (PMIX_SUCCESS != ret) {
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PMIX_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            return;
        }
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    data = &datbuf;

    /* get the target tag */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, data, &tag, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PMIX_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }

    /* copy the msg for relay to ourselves */
    PMIX_DATA_BUFFER_CREATE(relay);
    ret = PMIx_Data_copy_payload(relay, data);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PMIX_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return;
    }

    if (PRTE_RML_TAG_WIREUP == tag && !PRTE_PROC_IS_MASTER) {
        if (PRTE_SUCCESS != (ret = prte_util_decode_nidmap(data))) {
            PRTE_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PMIX_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            PMIX_DATA_BUFFER_RELEASE(relay);
            return;
        }
        /* unpack the wireup info */
        cnt = 1;
        while (PMIX_SUCCESS == (ret = PMIx_Data_unpack(NULL, data, &dmn, &cnt, PMIX_PROC))) {
            PMIX_VALUE_CONSTRUCT(&val);
            val.type = PMIX_STRING;
            cnt = 1;
            ret = PMIx_Data_unpack(NULL, data, &val.data.string, &cnt, PMIX_STRING);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                PMIX_DESTRUCT(&coll);
                PMIX_DATA_BUFFER_RELEASE(rly);
                PMIX_DATA_BUFFER_RELEASE(relay);
                return;
            }

            if (!PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_HNP) &&
                !PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_NAME) &&
                !PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_PARENT)) {
                /* store it locally */
                ret = PMIx_Store_internal(&dmn, PMIX_PROC_URI, &val);
                PMIX_VALUE_DESTRUCT(&val);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                    PMIX_DESTRUCT(&coll);
                    PMIX_DATA_BUFFER_RELEASE(rly);
                    PMIX_DATA_BUFFER_RELEASE(relay);
                    return;
                }
            }
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
            PMIX_ERROR_LOG(ret);
        }
    }

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        /* send the message to each of our children */
        PMIX_LIST_FOREACH(nm, &prte_rml_base.children, prte_routed_tree_t)
        {
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay sending relay msg of %d bytes to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) rly->bytes_used,
                                 PRTE_VPID_PRINT(nm->rank)));
            /* copy the buffer for send */
            PMIX_DATA_BUFFER_CREATE(rlycopy);
            ret = PMIx_Data_copy_payload(rlycopy, rly);
            if (PMIX_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_RELEASE(rlycopy);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                continue;
            }
            PRTE_RML_SEND(ret, nm->rank, rlycopy, PRTE_RML_TAG_XCAST);
            if (PRTE_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_RELEASE(rlycopy);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                continue;
            }
        }
    }

    /* cleanup */
    PMIX_LIST_DESTRUCT(&coll);
    PMIX_DATA_BUFFER_RELEASE(rly); // retain accounting

    /* now pass the relay buffer to myself for processing IFF it
     * wasn't just a wireup message - don't
     * inject it into the RML system via send as that will compete
     * with the relay messages down in the OOB. Instead, pass it
     * directly to the RML message processor */
    if (PRTE_RML_TAG_WIREUP != tag) {
        PRTE_RML_POST_MESSAGE(PRTE_PROC_MY_NAME, tag, 1, relay->base_ptr, relay->bytes_used);
        relay->base_ptr = NULL;
        relay->bytes_used = 0;
    }
    if (NULL != relay) {
        PMIX_DATA_BUFFER_RELEASE(relay);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
}

static int pack_xcast(pmix_data_buffer_t *buffer,
                      pmix_data_buffer_t *message, prte_rml_tag_t tag)
{
    int rc;
    pmix_data_buffer_t data;
    bool compressed;
    pmix_byte_object_t bo;
    size_t sz;

    /* setup an intermediate buffer */
    PMIX_DATA_BUFFER_CONSTRUCT(&data);

    /* pass the final tag */
    rc = PMIx_Data_pack(NULL, &data, &tag, 1, PRTE_RML_TAG);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&data);
        return rc;
    }

    /* copy the payload into the new buffer - this is non-destructive, so our
     * caller is still responsible for releasing any memory in the buffer they
     * gave to us
     */
    rc = PMIx_Data_copy_payload(&data, message);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&data);
        return rc;
    }

    /* see if we want to compress this message */
    if (PMIx_Data_compress((uint8_t *) data.base_ptr, data.bytes_used,
                           (uint8_t **) &bo.bytes, &sz)) {
        /* the data was compressed - mark that we compressed it */
        compressed = true;
        bo.size = sz;
    } else {
        /* mark that it was not compressed */
        compressed = false;
        bo.bytes = data.base_ptr;
        bo.size = data.bytes_used;
        data.base_ptr = NULL;
        data.bytes_used = 0;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&data);
    rc = PMIx_Data_pack(NULL, buffer, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        return rc;
    }
    rc = PMIx_Data_pack(NULL, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        return rc;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);

    return PRTE_SUCCESS;
}

