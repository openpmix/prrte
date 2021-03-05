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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/class/prte_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"

#include "src/mca/grpcomm/base/base.h"
#include "grpcomm_direct.h"


/* Static API's */
static int init(void);
static void finalize(void);
static int xcast(pmix_rank_t *vpids,
                 size_t nprocs,
                 pmix_data_buffer_t *buf);
static int allgather(prte_grpcomm_coll_t *coll,
                     pmix_data_buffer_t *buf, int mode);

/* Module def */
prte_grpcomm_base_module_t prte_grpcomm_direct_module = {
    .init = init,
    .finalize = finalize,
    .xcast = xcast,
    .allgather = allgather,
    .rbcast = NULL,
    .register_cb = NULL,
    .unregister_cb = NULL
};

/* internal functions */
static void xcast_recv(int status, pmix_proc_t* sender,
                       pmix_data_buffer_t* buffer, prte_rml_tag_t tag,
                       void* cbdata);
static void allgather_recv(int status, pmix_proc_t* sender,
                           pmix_data_buffer_t* buffer, prte_rml_tag_t tag,
                           void* cbdata);
static void barrier_release(int status, pmix_proc_t* sender,
                            pmix_data_buffer_t* buffer, prte_rml_tag_t tag,
                            void* cbdata);

/* internal variables */
static prte_list_t tracker;

/**
 * Initialize the module
 */
static int init(void)
{
    PRTE_CONSTRUCT(&tracker, prte_list_t);

    /* post the receives */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_XCAST,
                            PRTE_RML_PERSISTENT,
                            xcast_recv, NULL);
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_ALLGATHER_DIRECT,
                            PRTE_RML_PERSISTENT,
                            allgather_recv, NULL);
    /* setup recv for barrier release */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_COLL_RELEASE,
                            PRTE_RML_PERSISTENT,
                            barrier_release, NULL);

    return PRTE_SUCCESS;
}

/**
 * Finalize the module
 */
static void finalize(void)
{
    PRTE_LIST_DESTRUCT(&tracker);
    return;
}

static int xcast(pmix_rank_t *vpids,
                 size_t nprocs,
                 pmix_data_buffer_t *buf)
{
    int rc;

    /* send it to the HNP (could be myself) for relay */
    if (0 > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_XCAST,
                                          prte_rml_send_callback, NULL))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    return PRTE_SUCCESS;
}

static int allgather(prte_grpcomm_coll_t *coll,
                     pmix_data_buffer_t *buf, int mode)
{
    int rc;
    pmix_data_buffer_t *relay;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: allgather",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* the base functions pushed us into the event library
     * before calling us, so we can safely access global data
     * at this point */

    PMIX_DATA_BUFFER_CREATE(relay);
    /* pack the signature */
    rc = PMIx_Data_pack(NULL, relay, &coll->sig->sz, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return rc;
    }
    rc = PMIx_Data_pack(NULL, relay, coll->sig->signature, coll->sig->sz, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return rc;
    }

    /* pack the mode */
    rc = PMIx_Data_pack(NULL, relay, &mode, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return rc;
    }

    /* pass along the payload */
    rc = PMIx_Data_copy_payload(relay, buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return rc;
    }

    /* send this to ourselves for processing */
    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:allgather sending to ourself",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* send the info to ourselves for tracking */
    rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_NAME, relay,
                                 PRTE_RML_TAG_ALLGATHER_DIRECT,
                                 prte_rml_send_callback, NULL);
    return rc;
}

static void allgather_recv(int status, pmix_proc_t* sender,
                           pmix_data_buffer_t* buffer, prte_rml_tag_t tag,
                           void* cbdata)
{
    int32_t cnt;
    int rc, ret, mode;
    prte_grpcomm_signature_t sig;
    pmix_data_buffer_t *reply;
    prte_grpcomm_coll_t *coll;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct allgather recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* unpack the signature */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &sig.sz, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    PMIX_PROC_CREATE(sig.signature, sig.sz);
    cnt = sig.sz;
    rc = PMIx_Data_unpack(NULL, buffer, sig.signature, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = prte_grpcomm_base_get_tracker(&sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_PROC_FREE(sig.signature, sig.sz);
        return;
    }

    /* unpack the mode */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &mode, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* increment nprocs reported for collective */
    coll->nreported++;
    /* capture any provided content */
    rc = PMIx_Data_copy_payload(&coll->bucket, buffer);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct allgather recv nexpected %d nrep %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int)coll->nexpected, (int)coll->nreported));

    /* see if everyone has reported */
    if (coll->nreported == coll->nexpected) {
        if (PRTE_PROC_IS_MASTER) {
            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather HNP reports complete",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* the allgather is complete - send the xcast */
            PMIX_DATA_BUFFER_CREATE(reply);
            /* pack the signature */
            rc = PMIx_Data_pack(NULL, reply, &sig.sz, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            rc = PMIx_Data_pack(NULL, reply, sig.signature, sig.sz, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* pack the status - success since the allgather completed. This
             * would be an error if we timeout instead */
            ret = PRTE_SUCCESS;
            rc = PMIx_Data_pack(NULL, reply, &ret, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* pack the mode */
            rc = PMIx_Data_pack(NULL, reply, &mode, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* if we were asked to provide a context id, do so */
            if (1 == mode) {
                size_t sz;
                sz = prte_grpcomm_base.context_id;
                ++prte_grpcomm_base.context_id;
                rc = PMIx_Data_pack(NULL, reply, &sz, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_PROC_FREE(sig.signature, sig.sz);
                    return;
                }
            }
            /* transfer the collected bucket */
            rc = PMIx_Data_copy_payload(reply, &coll->bucket);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* send the release via xcast */
            (void)prte_grpcomm.xcast(&sig, PRTE_RML_TAG_COLL_RELEASE, reply);
            PMIX_DATA_BUFFER_RELEASE(reply);
        } else {
            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather rollup complete - sending to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));
            PMIX_DATA_BUFFER_CREATE(reply);
            /* pack the signature */
            rc = PMIx_Data_pack(NULL, reply, &sig.sz, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            rc = PMIx_Data_pack(NULL, reply, sig.signature, sig.sz, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* pack the mode */
            rc = PMIx_Data_pack(NULL, reply, &mode, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* transfer the collected bucket */
            rc = PMIx_Data_copy_payload(reply, &coll->bucket);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(sig.signature, sig.sz);
                return;
            }
            /* send the info to our parent */
            rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_PARENT, reply,
                                         PRTE_RML_TAG_ALLGATHER_DIRECT,
                                         prte_rml_send_callback, NULL);
        }
    }
    PMIX_PROC_FREE(sig.signature, sig.sz);
}

static void xcast_recv(int status, pmix_proc_t* sender,
                       pmix_data_buffer_t* buffer, prte_rml_tag_t tg,
                       void* cbdata)
{
    prte_list_item_t *item;
    prte_namelist_t *nm;
    int ret, cnt;
    pmix_data_buffer_t *relay=NULL, *rly, *rlycopy;
    pmix_data_buffer_t datbuf, *data;
    bool compressed;
    prte_job_t *jdata, *daemons;
    prte_proc_t *rec;
    prte_list_t coll;
    prte_grpcomm_signature_t sig;
    prte_rml_tag_t tag;
    pmix_byte_object_t bo, pbo;
    pmix_value_t val;
    pmix_proc_t dmn;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int)buffer->bytes_used));

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
    PRTE_CONSTRUCT(&coll, prte_list_t);

    /* unpack the flag to see if this payload is compressed */
    cnt=1;
    ret = PMIx_Data_unpack(NULL, buffer, &compressed, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    /* unpack the data blob */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PRTE_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    if (compressed) {
        /* decompress the data */
        if (PMIx_Data_decompress((uint8_t**)&bo.bytes, &bo.size,
                                (uint8_t*)pbo.bytes, pbo.size)) {
            /* the data has been uncompressed */
            ret = PMIx_Data_load(&datbuf, &bo);
            if (PMIX_SUCCESS != ret) {
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                PRTE_DESTRUCT(&coll);
                PMIX_DATA_BUFFER_RELEASE(rly);
                return;
            }
        } else {
            PMIX_ERROR_LOG(PMIX_ERROR);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            return;
        }
    } else {
        ret = PMIx_Data_load(&datbuf, &pbo);
        if (PMIX_SUCCESS != ret) {
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            return;
        }
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    data = &datbuf;

    /* get the signature that we do not need */
    cnt=1;
    ret = PMIx_Data_unpack(NULL, data, &sig.sz, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    PMIX_PROC_CREATE(sig.signature, sig.sz);
    cnt = sig.sz;
    ret = PMIx_Data_unpack(NULL, data, sig.signature, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        PMIX_PROC_FREE(sig.signature, sig.sz);
        return;
    }
    PMIX_PROC_FREE(sig.signature, sig.sz);

    /* get the target tag */
    cnt=1;
    ret = PMIx_Data_unpack(NULL, data, &tag, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
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
        PRTE_DESTRUCT(&coll);
        PMIX_DATA_BUFFER_RELEASE(rly);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return;
    }

    if (PRTE_RML_TAG_WIREUP == tag && !PRTE_PROC_IS_MASTER) {
        if (PRTE_SUCCESS != (ret = prte_util_decode_nidmap(data))) {
            PRTE_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            PMIX_DATA_BUFFER_RELEASE(relay);
            return;
        }
        if (PRTE_SUCCESS != (ret = prte_util_parse_node_info(data))) {
            PRTE_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PMIX_DATA_BUFFER_RELEASE(rly);
            PMIX_DATA_BUFFER_RELEASE(relay);
            return;
        }
        /* unpack the wireup info */
        cnt=1;
        while (PMIX_SUCCESS == (ret = PMIx_Data_unpack(NULL, data, &dmn, &cnt, PMIX_PROC))) {
            PMIX_VALUE_CONSTRUCT(&val);
            val.type = PMIX_STRING;
            cnt = 1;
            ret = PMIx_Data_unpack(NULL, data, &val.data.string, &cnt, PMIX_STRING);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                PRTE_DESTRUCT(&coll);
                PMIX_DATA_BUFFER_RELEASE(rly);
                PMIX_DATA_BUFFER_RELEASE(relay);
                return;
            }

            /* store it locally */
            ret = PMIx_Store_internal(&dmn, PMIX_PROC_URI, &val);
            PMIX_VALUE_DESTRUCT(&val);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                PRTE_DESTRUCT(&coll);
                PMIX_DATA_BUFFER_RELEASE(rly);
                PMIX_DATA_BUFFER_RELEASE(relay);
                return;
            }
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
            PMIX_ERROR_LOG(ret);
        }
        /* update the routing plan */
        prte_routed.update_routing_plan();
    }

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        /* get the list of next recipients from the routed module */
        prte_routed.get_routing_list(&coll);

        /* if list is empty, no relay is required */
        if (prte_list_is_empty(&coll)) {
            PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay - recipient list is empty!",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            goto CLEANUP;
        }

        /* send the message to each recipient on list, deconstructing it as we go */
        while (NULL != (item = prte_list_remove_first(&coll))) {
            nm = (prte_namelist_t*)item;

            PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay sending relay msg of %d bytes to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int)rly->bytes_used,
                                 PRTE_NAME_PRINT(&nm->name)));
            /* check the state of the recipient - no point
             * sending to someone not alive
             */
            jdata = prte_get_job_data_object(nm->name.nspace);
            if (NULL == (rec = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, nm->name.rank))) {
                if (!prte_abnormal_term_ordered && !prte_prteds_term_ordered) {
                    prte_output(0, "%s grpcomm:direct:send_relay proc %s not found - cannot relay",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name));
                }
                PRTE_RELEASE(item);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                continue;
            }
            if ((PRTE_PROC_STATE_RUNNING < rec->state &&
                PRTE_PROC_STATE_CALLED_ABORT != rec->state) ||
                !PRTE_FLAG_TEST(rec, PRTE_PROC_FLAG_ALIVE)) {
                if (!prte_abnormal_term_ordered && !prte_prteds_term_ordered) {
                    prte_output(0, "%s grpcomm:direct:send_relay proc %s not running - cannot relay: %s ",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name),
                                PRTE_FLAG_TEST(rec, PRTE_PROC_FLAG_ALIVE) ? prte_proc_state_to_str(rec->state) : "NOT ALIVE");
                }
                PRTE_RELEASE(item);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                continue;
            }
            /* copy the buffer for send */
            PMIX_DATA_BUFFER_CREATE(rlycopy);
            ret = PMIx_Data_copy_payload(rlycopy, rly);
            if (PMIX_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_RELEASE(rlycopy);
                PRTE_RELEASE(item);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                continue;
            }
            if (PRTE_SUCCESS != (ret = prte_rml.send_buffer_nb(&nm->name, rlycopy, PRTE_RML_TAG_XCAST,
                                                               prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_RELEASE(rlycopy);
                PRTE_RELEASE(item);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                continue;
            }
            PRTE_RELEASE(item);
        }
    }

 CLEANUP:
    /* cleanup */
    PRTE_LIST_DESTRUCT(&coll);
    PMIX_DATA_BUFFER_RELEASE(rly);  // retain accounting

    /* now pass the relay buffer to myself for processing IFF it
     * wasn't just a wireup message - don't
     * inject it into the RML system via send as that will compete
     * with the relay messages down in the OOB. Instead, pass it
     * directly to the RML message processor */
    if (PRTE_RML_TAG_WIREUP != tag) {
        PRTE_RML_POST_MESSAGE(PRTE_PROC_MY_NAME, tag, 1,
                              relay->base_ptr, relay->bytes_used);
        relay->base_ptr = NULL;
        relay->bytes_used = 0;
    }
    if (NULL != relay) {
        PMIX_DATA_BUFFER_RELEASE(relay);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
}

static void barrier_release(int status, pmix_proc_t* sender,
                            pmix_data_buffer_t* buffer, prte_rml_tag_t tag,
                            void* cbdata)
{
    int32_t cnt;
    int rc, ret, mode;
    prte_grpcomm_signature_t sig;
    prte_grpcomm_coll_t *coll;

    PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: barrier release called with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int)buffer->bytes_used));

    /* unpack the signature */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &sig.sz, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    PMIX_PROC_CREATE(sig.signature, sig.sz);
    cnt = sig.sz;
    rc = PMIx_Data_unpack(NULL, buffer, sig.signature, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the mode */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &mode, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker - it is not an error if not
     * found as that just means we wre not involved
     * in the collective */
    if (NULL == (coll = prte_grpcomm_base_get_tracker(&sig, false))) {
        PMIX_PROC_FREE(sig.signature, sig.sz);
        return;
    }

    /* execute the callback */
    if (NULL != coll->cbfunc) {
        coll->cbfunc(ret, buffer, coll->cbdata);
    }
    prte_list_remove_item(&prte_grpcomm_base.ongoing, &coll->super);
    PRTE_RELEASE(coll);
    PMIX_PROC_FREE(sig.signature, sig.sz);
}
