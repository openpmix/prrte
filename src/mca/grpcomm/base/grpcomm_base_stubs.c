/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */

/*
 * includes
 */
#include "prte_config.h"


#include "src/dss/dss.h"

#include "src/mca/prtecompress/prtecompress.h"
#include "src/util/proc_info.h"
#include "src/util/error_strings.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/routed/routed.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/base/base.h"

static int pack_xcast(prte_grpcomm_signature_t *sig,
                      prte_buffer_t *buffer,
                      prte_buffer_t *message,
                      prte_rml_tag_t tag);

static int create_dmns(prte_grpcomm_signature_t *sig,
                       prte_vpid_t **dmns, size_t *ndmns);

typedef struct {
    prte_object_t super;
    prte_event_t ev;
    prte_grpcomm_signature_t *sig;
    prte_buffer_t *buf;
    int mode;
    prte_grpcomm_cbfunc_t cbfunc;
    void *cbdata;
} prte_grpcomm_caddy_t;
static void gccon(prte_grpcomm_caddy_t *p)
{
    p->sig = NULL;
    p->buf = NULL;
    p->mode = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void gcdes(prte_grpcomm_caddy_t *p)
{
    if (NULL != p->buf) {
        PRTE_RELEASE(p->buf);
    }
}
static PRTE_CLASS_INSTANCE(prte_grpcomm_caddy_t,
                          prte_object_t,
                          gccon, gcdes);

int prte_grpcomm_API_xcast(prte_grpcomm_signature_t *sig,
                           prte_rml_tag_t tag,
                           prte_buffer_t *msg)
{
    int rc = PRTE_ERROR;
    prte_buffer_t *buf;
    prte_grpcomm_base_active_t *active;
    prte_vpid_t *dmns;
    size_t ndmns;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:xcast sending %u bytes to tag %ld",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == msg) ? 0 : (unsigned int)msg->bytes_used, (long)tag));

    /* this function does not access any framework-global data, and
     * so it does not require us to push it into the event library */

    /* prep the output buffer */
    buf = PRTE_NEW(prte_buffer_t);

    /* create the array of participating daemons */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &dmns, &ndmns))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        return rc;
    }

    /* setup the payload */
    if (PRTE_SUCCESS != (rc = pack_xcast(sig, buf, msg, tag))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        if (NULL != dmns) {
            free(dmns);
        }
        return rc;
    }

    /* cycle thru the actives and see who can send it */
    PRTE_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t) {
        if (NULL != active->module->xcast) {
            if (PRTE_SUCCESS == (rc = active->module->xcast(dmns, ndmns, buf))) {
                break;
            }
        }
    }
    PRTE_RELEASE(buf);  // if the module needs to keep the buf, it should PRTE_RETAIN it
    if (NULL != dmns) {
        free(dmns);
    }
    return rc;
}

int prte_grpcomm_API_rbcast(prte_grpcomm_signature_t *sig,
                           prte_rml_tag_t tag,
                           prte_buffer_t *msg)
{
    int rc = PRTE_ERROR;
    prte_buffer_t *buf;
    prte_grpcomm_base_active_t *active;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:rbcast sending %u bytes to tag %ld",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == msg) ? 0 : (unsigned int)msg->bytes_used, (long)tag));

    /* this function does not access any framework-global data, and
     * so it does not require us to push it into the event library */

    /* prep the output buffer */
    buf = PRTE_NEW(prte_buffer_t);

    /* setup the payload */
    if (PRTE_SUCCESS != (rc = pack_xcast(sig, buf, msg, tag))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        return rc;
    }
    /* cycle thru the actives and see who can send it */
    PRTE_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t) {
        if (NULL != active->module->rbcast) {
            if (PRTE_SUCCESS == (rc = active->module->rbcast(buf))) {
                break;
            }
        }
    }

    return rc;
}

int prte_grpcomm_API_register_cb(prte_grpcomm_rbcast_cb_t callback)
{
    int rc = PRTE_ERROR;
    prte_grpcomm_base_active_t *active;

    PRTE_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t) {
        if (NULL != active->module->register_cb) {
            if (PRTE_ERROR != (rc = active->module->register_cb(callback))) {
                break;
            }
        }
    }
    return rc;
}

static void allgather_stub(int fd, short args, void *cbdata)
{
    prte_grpcomm_caddy_t *cd = (prte_grpcomm_caddy_t*)cbdata;
    int ret = PRTE_SUCCESS;
    prte_grpcomm_base_active_t *active;
    prte_grpcomm_coll_t *coll;
    uint32_t *seq_number;

    PRTE_ACQUIRE_OBJECT(cd);

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:allgather stub",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* retrieve an existing tracker, create it if not
     * already found. The allgather module is responsible
     * for releasing it upon completion of the collective */
    ret = prte_hash_table_get_value_ptr(&prte_grpcomm_base.sig_table, (void *)cd->sig->signature, cd->sig->sz * sizeof(prte_process_name_t), (void **)&seq_number);
    if (PRTE_ERR_NOT_FOUND == ret) {
        seq_number = (uint32_t *)malloc(sizeof(uint32_t));
        *seq_number = 0;
    } else if (PRTE_SUCCESS == ret) {
        *seq_number = *seq_number + 1;
    } else {
        PRTE_OUTPUT((prte_grpcomm_base_framework.framework_output,
                     "%s rpcomm:base:allgather cannot get signature from hash table",
                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_ERROR_LOG(ret);
        PRTE_RELEASE(cd);
        return;
    }
    ret = prte_hash_table_set_value_ptr(&prte_grpcomm_base.sig_table, (void *)cd->sig->signature, cd->sig->sz * sizeof(prte_process_name_t), (void *)seq_number);
    if (PRTE_SUCCESS != ret) {
        PRTE_OUTPUT((prte_grpcomm_base_framework.framework_output,
                     "%s rpcomm:base:allgather cannot add new signature to hash table",
                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_ERROR_LOG(ret);
        PRTE_RELEASE(cd);
        return;
    }
    coll = prte_grpcomm_base_get_tracker(cd->sig, true);
    if (NULL == coll) {
        PRTE_RELEASE(cd->sig);
        PRTE_RELEASE(cd);
        return;
    }
    PRTE_RELEASE(cd->sig);
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    /* cycle thru the actives and see who can process it */
    PRTE_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t) {
        if (NULL != active->module->allgather) {
            if (PRTE_SUCCESS == active->module->allgather(coll, cd->buf, cd->mode)) {
                break;
            }
        }
    }
    PRTE_RELEASE(cd);
}

int prte_grpcomm_API_allgather(prte_grpcomm_signature_t *sig,
                               prte_buffer_t *buf, int mode,
                               prte_grpcomm_cbfunc_t cbfunc,
                               void *cbdata)
{
    prte_grpcomm_caddy_t *cd;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:allgather",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    cd = PRTE_NEW(prte_grpcomm_caddy_t);
    /* ensure the data doesn't go away */
    PRTE_RETAIN(buf);
    prte_dss.copy((void **)&cd->sig, (void *)sig, PRTE_SIGNATURE);
    cd->buf = buf;
    cd->mode = mode;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, allgather_stub, cd);
    prte_event_set_priority(&cd->ev, PRTE_MSG_PRI);
    PRTE_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

prte_grpcomm_coll_t* prte_grpcomm_base_get_tracker(prte_grpcomm_signature_t *sig, bool create)
{
    prte_grpcomm_coll_t *coll;
    int rc;
    prte_namelist_t *nm;
    prte_list_t children;
    size_t n;

    /* search the existing tracker list to see if this already exists */
    PRTE_LIST_FOREACH(coll, &prte_grpcomm_base.ongoing, prte_grpcomm_coll_t) {
        if (NULL == sig->signature) {
            if (NULL == coll->sig->signature) {
                /* only one collective can operate at a time
                 * across every process in the system */
                return coll;
            }
            /* if only one is NULL, then we can't possibly match */
            break;
        }
        if (PRTE_EQUAL == (rc = prte_dss.compare(sig, coll->sig, PRTE_SIGNATURE))) {
            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:base:returning existing collective",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            return coll;
        }
    }
    /* if we get here, then this is a new collective - so create
     * the tracker for it */
    if (!create) {
        PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:base: not creating new coll",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

        return NULL;
    }
    coll = PRTE_NEW(prte_grpcomm_coll_t);
    prte_dss.copy((void **)&coll->sig, (void *)sig, PRTE_SIGNATURE);

    if (1 < prte_output_get_verbosity(prte_grpcomm_base_framework.framework_output)) {
        char *tmp=NULL;
        (void)prte_dss.print(&tmp, NULL, coll->sig, PRTE_SIGNATURE);
        prte_output(0, "%s grpcomm:base: creating new coll for%s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tmp);
        free(tmp);
    }

    prte_list_append(&prte_grpcomm_base.ongoing, &coll->super);

    /* now get the daemons involved */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRTE_ERROR_LOG(rc);
        return NULL;
    }

    /* cycle thru the array of daemons and compare them to our
     * children in the routing tree, counting the ones that match
     * so we know how many daemons we should receive contributions from */
    PRTE_CONSTRUCT(&children, prte_list_t);
    prte_routed.get_routing_list(&children);
    while (NULL != (nm = (prte_namelist_t*)prte_list_remove_first(&children))) {
        for (n=0; n < coll->ndmns; n++) {
            if (nm->name.vpid == coll->dmns[n]) {
                coll->nexpected++;
                break;
            }
        }
        PRTE_RELEASE(nm);
    }
    PRTE_LIST_DESTRUCT(&children);

    /* see if I am in the array of participants - note that I may
     * be in the rollup tree even though I'm not participating
     * in the collective itself */
    for (n=0; n < coll->ndmns; n++) {
        if (coll->dmns[n] == PRTE_PROC_MY_NAME->vpid) {
            coll->nexpected++;
            break;
        }
    }

    return coll;
}

static int create_dmns(prte_grpcomm_signature_t *sig,
                       prte_vpid_t **dmns, size_t *ndmns)
{
    size_t n;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_node_t *node;
    int i;
    prte_list_t ds;
    prte_namelist_t *nm;
    prte_vpid_t vpid;
    bool found;
    size_t nds=0;
    prte_vpid_t *dns=NULL;
    int rc = PRTE_SUCCESS;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:create_dmns called with %s signature",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == sig->signature) ? "NULL" : "NON-NULL"));

    /* if NULL == procs, or the target jobid is our own,
     * then all daemons are participating */
    if (NULL == sig->signature || PRTE_PROC_MY_NAME->jobid == sig->signature[0].jobid) {
        *ndmns = prte_process_info.num_daemons;
        *dmns = NULL;
        return PRTE_SUCCESS;
    }

    PRTE_CONSTRUCT(&ds, prte_list_t);
    for (n=0; n < sig->sz; n++) {
        if (NULL == (jdata = prte_get_job_data_object(sig->signature[n].jobid))) {
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        if (NULL == jdata->map || 0 == jdata->map->num_nodes) {
            /* we haven't generated a job map yet - if we are the HNP,
             * then we should only involve ourselves. Otherwise, we have
             * no choice but to abort to avoid hangs */
            if (PRTE_PROC_IS_MASTER) {
                rc = PRTE_SUCCESS;
                break;
            }
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        if (PRTE_VPID_WILDCARD == sig->signature[n].vpid) {
            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:base:create_dmns called for all procs in job %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(sig->signature[0].jobid)));
            /* all daemons hosting this jobid are participating */
            for (i=0; i < jdata->map->nodes->size; i++) {
                if (NULL == (node = prte_pointer_array_get_item(jdata->map->nodes, i))) {
                    continue;
                }
                if (NULL == node->daemon) {
                    rc = PRTE_ERR_NOT_FOUND;
                    goto done;
                }
                found = false;
                PRTE_LIST_FOREACH(nm, &ds, prte_namelist_t) {
                    if (nm->name.vpid == node->daemon->name.vpid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                         "%s grpcomm:base:create_dmns adding daemon %s to list",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         PRTE_NAME_PRINT(&node->daemon->name)));
                    nm = PRTE_NEW(prte_namelist_t);
                    nm->name.jobid = PRTE_PROC_MY_NAME->jobid;
                    nm->name.vpid = node->daemon->name.vpid;
                    prte_list_append(&ds, &nm->super);
                }
            }
        } else {
            /* lookup the daemon for this proc and add it to the list */
            PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                "%s sign: GETTING PROC OBJECT FOR %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PRTE_NAME_PRINT(&sig->signature[n])));
            if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, sig->signature[n].vpid))) {
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            if (NULL == proc->node || NULL == proc->node->daemon) {
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            vpid = proc->node->daemon->name.vpid;
            found = false;
            PRTE_LIST_FOREACH(nm, &ds, prte_namelist_t) {
                if (nm->name.vpid == vpid) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                nm = PRTE_NEW(prte_namelist_t);
                nm->name.jobid = PRTE_PROC_MY_NAME->jobid;
                nm->name.vpid = vpid;
                prte_list_append(&ds, &nm->super);
            }
        }
    }

  done:
    if (0 < prte_list_get_size(&ds)) {
        dns = (prte_vpid_t*)malloc(prte_list_get_size(&ds) * sizeof(prte_vpid_t));
        nds = 0;
        while (NULL != (nm = (prte_namelist_t*)prte_list_remove_first(&ds))) {
            PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:base:create_dmns adding daemon %s to array",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&nm->name)));
            dns[nds++] = nm->name.vpid;
            PRTE_RELEASE(nm);
        }
    }
    PRTE_LIST_DESTRUCT(&ds);
    *dmns = dns;
    *ndmns = nds;
    return rc;
}

static int pack_xcast(prte_grpcomm_signature_t *sig,
                      prte_buffer_t *buffer,
                      prte_buffer_t *message,
                      prte_rml_tag_t tag)
{
    int rc;
    prte_buffer_t data;
    int8_t flag;
    uint8_t *cmpdata;
    size_t cmplen;

    /* setup an intermediate buffer */
    PRTE_CONSTRUCT(&data, prte_buffer_t);

    /* pass along the signature */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(&data, &sig, 1, PRTE_SIGNATURE))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&data);
        return rc;
    }
    /* pass the final tag */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(&data, &tag, 1, PRTE_RML_TAG))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&data);
        return rc;
    }

    /* copy the payload into the new buffer - this is non-destructive, so our
     * caller is still responsible for releasing any memory in the buffer they
     * gave to us
     */
    if (PRTE_SUCCESS != (rc = prte_dss.copy_payload(&data, message))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&data);
        return rc;
    }

    /* see if we want to compress this message */
    if (prte_compress.compress_block((uint8_t*)data.base_ptr, data.bytes_used,
                                     &cmpdata, &cmplen)) {
        /* the data was compressed - mark that we compressed it */
        flag = 1;
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, &flag, 1, PRTE_INT8))) {
            PRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRTE_DESTRUCT(&data);
            return rc;
        }
        /* pack the compressed length */
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, &cmplen, 1, PRTE_SIZE))) {
            PRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRTE_DESTRUCT(&data);
            return rc;
        }
        /* pack the uncompressed length */
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, &data.bytes_used, 1, PRTE_SIZE))) {
            PRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRTE_DESTRUCT(&data);
            return rc;
        }
        /* pack the compressed info */
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, cmpdata, cmplen, PRTE_UINT8))) {
            PRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRTE_DESTRUCT(&data);
            return rc;
        }
        PRTE_DESTRUCT(&data);
        free(cmpdata);
    } else {
        /* mark that it was not compressed */
        flag = 0;
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, &flag, 1, PRTE_INT8))) {
            PRTE_ERROR_LOG(rc);
            PRTE_DESTRUCT(&data);
            free(cmpdata);
            return rc;
        }
        /* transfer the payload across */
        prte_dss.copy_payload(buffer, &data);
        PRTE_DESTRUCT(&data);
    }

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "MSG SIZE: %lu", buffer->bytes_used));
    return PRTE_SUCCESS;
}

void prte_grpcomm_base_mark_distance_recv (prte_grpcomm_coll_t *coll,
                                           uint32_t distance) {
    prte_bitmap_set_bit (&coll->distance_mask_recv, distance);
}

unsigned int prte_grpcomm_base_check_distance_recv (prte_grpcomm_coll_t *coll,
                                                    uint32_t distance) {
    return prte_bitmap_is_set_bit (&coll->distance_mask_recv, distance);
}
