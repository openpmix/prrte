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
#include "prrte_config.h"


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
#include "src/runtime/prrte_globals.h"

#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/base/base.h"

static int pack_xcast(prrte_grpcomm_signature_t *sig,
                      prrte_buffer_t *buffer,
                      prrte_buffer_t *message,
                      prrte_rml_tag_t tag);

static int create_dmns(prrte_grpcomm_signature_t *sig,
                       prrte_vpid_t **dmns, size_t *ndmns);

typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    prrte_grpcomm_signature_t *sig;
    prrte_buffer_t *buf;
    int mode;
    prrte_grpcomm_cbfunc_t cbfunc;
    void *cbdata;
} prrte_grpcomm_caddy_t;
static void gccon(prrte_grpcomm_caddy_t *p)
{
    p->sig = NULL;
    p->buf = NULL;
    p->mode = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void gcdes(prrte_grpcomm_caddy_t *p)
{
    if (NULL != p->buf) {
        PRRTE_RELEASE(p->buf);
    }
}
static PRRTE_CLASS_INSTANCE(prrte_grpcomm_caddy_t,
                          prrte_object_t,
                          gccon, gcdes);

int prrte_grpcomm_API_xcast(prrte_grpcomm_signature_t *sig,
                           prrte_rml_tag_t tag,
                           prrte_buffer_t *msg)
{
    int rc = PRRTE_ERROR;
    prrte_buffer_t *buf;
    prrte_grpcomm_base_active_t *active;
    prrte_vpid_t *dmns;
    size_t ndmns;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:xcast sending %u bytes to tag %ld",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (NULL == msg) ? 0 : (unsigned int)msg->bytes_used, (long)tag));

    /* this function does not access any framework-global data, and
     * so it does not require us to push it into the event library */

    /* prep the output buffer */
    buf = PRRTE_NEW(prrte_buffer_t);

    /* create the array of participating daemons */
    if (PRRTE_SUCCESS != (rc = create_dmns(sig, &dmns, &ndmns))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return rc;
    }

    /* setup the payload */
    if (PRRTE_SUCCESS != (rc = pack_xcast(sig, buf, msg, tag))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        if (NULL != dmns) {
            free(dmns);
        }
        return rc;
    }

    /* cycle thru the actives and see who can send it */
    PRRTE_LIST_FOREACH(active, &prrte_grpcomm_base.actives, prrte_grpcomm_base_active_t) {
        if (NULL != active->module->xcast) {
            if (PRRTE_SUCCESS == (rc = active->module->xcast(dmns, ndmns, buf))) {
                break;
            }
        }
    }
    PRRTE_RELEASE(buf);  // if the module needs to keep the buf, it should PRRTE_RETAIN it
    if (NULL != dmns) {
        free(dmns);
    }
    return rc;
}

static void allgather_stub(int fd, short args, void *cbdata)
{
    prrte_grpcomm_caddy_t *cd = (prrte_grpcomm_caddy_t*)cbdata;
    int ret = PRRTE_SUCCESS;
    int rc;
    prrte_grpcomm_base_active_t *active;
    prrte_grpcomm_coll_t *coll;
    uint32_t *seq_number;

    PRRTE_ACQUIRE_OBJECT(cd);

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:allgather stub",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* retrieve an existing tracker, create it if not
     * already found. The allgather module is responsible
     * for releasing it upon completion of the collective */
    ret = prrte_hash_table_get_value_ptr(&prrte_grpcomm_base.sig_table, (void *)cd->sig->signature, cd->sig->sz * sizeof(prrte_process_name_t), (void **)&seq_number);
    if (PRRTE_ERR_NOT_FOUND == ret) {
        seq_number = (uint32_t *)malloc(sizeof(uint32_t));
        *seq_number = 0;
    } else if (PRRTE_SUCCESS == ret) {
        *seq_number = *seq_number + 1;
    } else {
        PRRTE_OUTPUT((prrte_grpcomm_base_framework.framework_output,
                     "%s rpcomm:base:allgather cannot get signature from hash table",
                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(cd);
        return;
    }
    ret = prrte_hash_table_set_value_ptr(&prrte_grpcomm_base.sig_table, (void *)cd->sig->signature, cd->sig->sz * sizeof(prrte_process_name_t), (void *)seq_number);
    if (PRRTE_SUCCESS != ret) {
        PRRTE_OUTPUT((prrte_grpcomm_base_framework.framework_output,
                     "%s rpcomm:base:allgather cannot add new signature to hash table",
                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(cd);
        return;
    }
    coll = prrte_grpcomm_base_get_tracker(cd->sig, true);
    if (NULL == coll) {
        PRRTE_RELEASE(cd->sig);
        PRRTE_RELEASE(cd);
        return;
    }
    PRRTE_RELEASE(cd->sig);
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    /* cycle thru the actives and see who can process it */
    PRRTE_LIST_FOREACH(active, &prrte_grpcomm_base.actives, prrte_grpcomm_base_active_t) {
        if (NULL != active->module->allgather) {
            if (PRRTE_SUCCESS == (rc = active->module->allgather(coll, cd->buf, cd->mode))) {
                break;
            }
        }
    }
    PRRTE_RELEASE(cd);
}

int prrte_grpcomm_API_allgather(prrte_grpcomm_signature_t *sig,
                               prrte_buffer_t *buf, int mode,
                               prrte_grpcomm_cbfunc_t cbfunc,
                               void *cbdata)
{
    prrte_grpcomm_caddy_t *cd;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:allgather",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    cd = PRRTE_NEW(prrte_grpcomm_caddy_t);
    /* ensure the data doesn't go away */
    PRRTE_RETAIN(buf);
    prrte_dss.copy((void **)&cd->sig, (void *)sig, PRRTE_SIGNATURE);
    cd->buf = buf;
    cd->mode = mode;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prrte_event_set(prrte_event_base, &cd->ev, -1, PRRTE_EV_WRITE, allgather_stub, cd);
    prrte_event_set_priority(&cd->ev, PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(cd);
    prrte_event_active(&cd->ev, PRRTE_EV_WRITE, 1);
    return PRRTE_SUCCESS;
}

prrte_grpcomm_coll_t* prrte_grpcomm_base_get_tracker(prrte_grpcomm_signature_t *sig, bool create)
{
    prrte_grpcomm_coll_t *coll;
    int rc;
    prrte_namelist_t *nm;
    prrte_list_t children;
    size_t n;

    /* search the existing tracker list to see if this already exists */
    PRRTE_LIST_FOREACH(coll, &prrte_grpcomm_base.ongoing, prrte_grpcomm_coll_t) {
        if (NULL == sig->signature) {
            if (NULL == coll->sig->signature) {
                /* only one collective can operate at a time
                 * across every process in the system */
                return coll;
            }
            /* if only one is NULL, then we can't possibly match */
            break;
        }
        if (PRRTE_EQUAL == (rc = prrte_dss.compare(sig, coll->sig, PRRTE_SIGNATURE))) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:base:returning existing collective",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            return coll;
        }
    }
    /* if we get here, then this is a new collective - so create
     * the tracker for it */
    if (!create) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:base: not creating new coll",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

        return NULL;
    }
    coll = PRRTE_NEW(prrte_grpcomm_coll_t);
    prrte_dss.copy((void **)&coll->sig, (void *)sig, PRRTE_SIGNATURE);

    if (1 < prrte_output_get_verbosity(prrte_grpcomm_base_framework.framework_output)) {
        char *tmp=NULL;
        (void)prrte_dss.print(&tmp, NULL, coll->sig, PRRTE_SIGNATURE);
        prrte_output(0, "%s grpcomm:base: creating new coll for%s",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), tmp);
        free(tmp);
    }

    prrte_list_append(&prrte_grpcomm_base.ongoing, &coll->super);

    /* now get the daemons involved */
    if (PRRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRRTE_ERROR_LOG(rc);
        return NULL;
    }

    /* cycle thru the array of daemons and compare them to our
     * children in the routing tree, counting the ones that match
     * so we know how many daemons we should receive contributions from */
    PRRTE_CONSTRUCT(&children, prrte_list_t);
    prrte_routed.get_routing_list(&children);
    while (NULL != (nm = (prrte_namelist_t*)prrte_list_remove_first(&children))) {
        for (n=0; n < coll->ndmns; n++) {
            if (nm->name.vpid == coll->dmns[n]) {
                coll->nexpected++;
                break;
            }
        }
        PRRTE_RELEASE(nm);
    }
    PRRTE_LIST_DESTRUCT(&children);

    /* see if I am in the array of participants - note that I may
     * be in the rollup tree even though I'm not participating
     * in the collective itself */
    for (n=0; n < coll->ndmns; n++) {
        if (coll->dmns[n] == PRRTE_PROC_MY_NAME->vpid) {
            coll->nexpected++;
            break;
        }
    }

    return coll;
}

static int create_dmns(prrte_grpcomm_signature_t *sig,
                       prrte_vpid_t **dmns, size_t *ndmns)
{
    size_t n;
    prrte_job_t *jdata;
    prrte_proc_t *proc;
    prrte_node_t *node;
    int i;
    prrte_list_t ds;
    prrte_namelist_t *nm;
    prrte_vpid_t vpid;
    bool found;
    size_t nds=0;
    prrte_vpid_t *dns=NULL;
    int rc = PRRTE_SUCCESS;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:create_dmns called with %s signature",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (NULL == sig->signature) ? "NULL" : "NON-NULL"));

    /* if NULL == procs, or the target jobid is our own,
     * then all daemons are participating */
    if (NULL == sig->signature || PRRTE_PROC_MY_NAME->jobid == sig->signature[0].jobid) {
        *ndmns = prrte_process_info.num_procs;
        *dmns = NULL;
        return PRRTE_SUCCESS;
    }

    PRRTE_CONSTRUCT(&ds, prrte_list_t);
    for (n=0; n < sig->sz; n++) {
        if (NULL == (jdata = prrte_get_job_data_object(sig->signature[n].jobid))) {
            rc = PRRTE_ERR_NOT_FOUND;
            break;
        }
        if (NULL == jdata->map || 0 == jdata->map->num_nodes) {
            /* we haven't generated a job map yet - if we are the HNP,
             * then we should only involve ourselves. Otherwise, we have
             * no choice but to abort to avoid hangs */
            if (PRRTE_PROC_IS_MASTER) {
                rc = PRRTE_SUCCESS;
                break;
            }
            rc = PRRTE_ERR_NOT_FOUND;
            break;
        }
        if (PRRTE_VPID_WILDCARD == sig->signature[n].vpid) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:base:create_dmns called for all procs in job %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_JOBID_PRINT(sig->signature[0].jobid)));
            /* all daemons hosting this jobid are participating */
            for (i=0; i < jdata->map->nodes->size; i++) {
                if (NULL == (node = prrte_pointer_array_get_item(jdata->map->nodes, i))) {
                    continue;
                }
                if (NULL == node->daemon) {
                    rc = PRRTE_ERR_NOT_FOUND;
                    goto done;
                }
                found = false;
                PRRTE_LIST_FOREACH(nm, &ds, prrte_namelist_t) {
                    if (nm->name.vpid == node->daemon->name.vpid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PRRTE_OUTPUT_VERBOSE((5, prrte_grpcomm_base_framework.framework_output,
                                         "%s grpcomm:base:create_dmns adding daemon %s to list",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         PRRTE_NAME_PRINT(&node->daemon->name)));
                    nm = PRRTE_NEW(prrte_namelist_t);
                    nm->name.jobid = PRRTE_PROC_MY_NAME->jobid;
                    nm->name.vpid = node->daemon->name.vpid;
                    prrte_list_append(&ds, &nm->super);
                }
            }
        } else {
            /* lookup the daemon for this proc and add it to the list */
            PRRTE_OUTPUT_VERBOSE((5, prrte_grpcomm_base_framework.framework_output,
                                "%s sign: GETTING PROC OBJECT FOR %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&sig->signature[n])));
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, sig->signature[n].vpid))) {
                rc = PRRTE_ERR_NOT_FOUND;
                goto done;
            }
            if (NULL == proc->node || NULL == proc->node->daemon) {
                rc = PRRTE_ERR_NOT_FOUND;
                goto done;
            }
            vpid = proc->node->daemon->name.vpid;
            found = false;
            PRRTE_LIST_FOREACH(nm, &ds, prrte_namelist_t) {
                if (nm->name.vpid == vpid) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                nm = PRRTE_NEW(prrte_namelist_t);
                nm->name.jobid = PRRTE_PROC_MY_NAME->jobid;
                nm->name.vpid = vpid;
                prrte_list_append(&ds, &nm->super);
            }
        }
    }

  done:
    if (0 < prrte_list_get_size(&ds)) {
        dns = (prrte_vpid_t*)malloc(prrte_list_get_size(&ds) * sizeof(prrte_vpid_t));
        nds = 0;
        while (NULL != (nm = (prrte_namelist_t*)prrte_list_remove_first(&ds))) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:base:create_dmns adding daemon %s to array",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&nm->name)));
            dns[nds++] = nm->name.vpid;
            PRRTE_RELEASE(nm);
        }
    }
    PRRTE_LIST_DESTRUCT(&ds);
    *dmns = dns;
    *ndmns = nds;
    return rc;
}

static int pack_xcast(prrte_grpcomm_signature_t *sig,
                      prrte_buffer_t *buffer,
                      prrte_buffer_t *message,
                      prrte_rml_tag_t tag)
{
    int rc;
    prrte_buffer_t data;
    int8_t flag;
    uint8_t *cmpdata;
    size_t cmplen;

    /* setup an intermediate buffer */
    PRRTE_CONSTRUCT(&data, prrte_buffer_t);

    /* pass along the signature */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&data, &sig, 1, PRRTE_SIGNATURE))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&data);
        return rc;
    }
    /* pass the final tag */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&data, &tag, 1, PRRTE_RML_TAG))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&data);
        return rc;
    }

    /* copy the payload into the new buffer - this is non-destructive, so our
     * caller is still responsible for releasing any memory in the buffer they
     * gave to us
     */
    if (PRRTE_SUCCESS != (rc = prrte_dss.copy_payload(&data, message))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&data);
        return rc;
    }

    /* see if we want to compress this message */
    if (prrte_compress.compress_block((uint8_t*)data.base_ptr, data.bytes_used,
                                     &cmpdata, &cmplen)) {
        /* the data was compressed - mark that we compressed it */
        flag = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &flag, 1, PRRTE_INT8))) {
            PRRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRRTE_DESTRUCT(&data);
            return rc;
        }
        /* pack the compressed length */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &cmplen, 1, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRRTE_DESTRUCT(&data);
            return rc;
        }
        /* pack the uncompressed length */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &data.bytes_used, 1, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRRTE_DESTRUCT(&data);
            return rc;
        }
        /* pack the compressed info */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, cmpdata, cmplen, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(rc);
            free(cmpdata);
            PRRTE_DESTRUCT(&data);
            return rc;
        }
        PRRTE_DESTRUCT(&data);
        free(cmpdata);
    } else {
        /* mark that it was not compressed */
        flag = 0;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &flag, 1, PRRTE_INT8))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&data);
            free(cmpdata);
            return rc;
        }
        /* transfer the payload across */
        prrte_dss.copy_payload(buffer, &data);
        PRRTE_DESTRUCT(&data);
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "MSG SIZE: %lu", buffer->bytes_used));
    return PRRTE_SUCCESS;
}

void prrte_grpcomm_base_mark_distance_recv (prrte_grpcomm_coll_t *coll,
                                           uint32_t distance) {
    prrte_bitmap_set_bit (&coll->distance_mask_recv, distance);
}

unsigned int prrte_grpcomm_base_check_distance_recv (prrte_grpcomm_coll_t *coll,
                                                    uint32_t distance) {
    return prrte_bitmap_is_set_bit (&coll->distance_mask_recv, distance);
}
