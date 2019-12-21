/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
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
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"

#include "src/mca/grpcomm/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/grpcomm/base/static-components.h"

/*
 * Global variables
 */
prrte_grpcomm_base_t prrte_grpcomm_base = {{{0}}};

prrte_grpcomm_API_module_t prrte_grpcomm = {
    prrte_grpcomm_API_xcast,
    prrte_grpcomm_API_allgather
};

static bool recv_issued = false;

static int base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_grpcomm_base.context_id = 1;
    prrte_mca_base_var_register("prrte", "grpcomm", "base", "starting_context_id",
                                "Starting value for assigning context id\'s",
                                PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &prrte_grpcomm_base.context_id);

    return PRRTE_SUCCESS;
}

static int prrte_grpcomm_base_close(void)
{
    prrte_grpcomm_base_active_t *active;
    void *key;
    size_t size;
    uint32_t *seq_number;

    if (recv_issued) {
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_XCAST);
        recv_issued = false;
    }

    /* Close the active modules */
    PRRTE_LIST_FOREACH(active, &prrte_grpcomm_base.actives, prrte_grpcomm_base_active_t) {
        if (NULL != active->module->finalize) {
            active->module->finalize();
        }
    }
    PRRTE_LIST_DESTRUCT(&prrte_grpcomm_base.actives);
    PRRTE_LIST_DESTRUCT(&prrte_grpcomm_base.ongoing);
    for (void *_nptr=NULL;                                   \
         PRRTE_SUCCESS == prrte_hash_table_get_next_key_ptr(&prrte_grpcomm_base.sig_table, &key, &size, (void **)&seq_number, _nptr, &_nptr);) {
        free(seq_number);
    }
    PRRTE_DESTRUCT(&prrte_grpcomm_base.sig_table);

    return prrte_mca_base_framework_components_close(&prrte_grpcomm_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_grpcomm_base_open(prrte_mca_base_open_flag_t flags)
{
    PRRTE_CONSTRUCT(&prrte_grpcomm_base.actives, prrte_list_t);
    PRRTE_CONSTRUCT(&prrte_grpcomm_base.ongoing, prrte_list_t);
    PRRTE_CONSTRUCT(&prrte_grpcomm_base.sig_table, prrte_hash_table_t);
    prrte_hash_table_init(&prrte_grpcomm_base.sig_table, 128);

    return prrte_mca_base_framework_components_open(&prrte_grpcomm_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, grpcomm, "GRPCOMM", base_register,
                                 prrte_grpcomm_base_open,
                                 prrte_grpcomm_base_close,
                                 prrte_grpcomm_base_static_components, 0);

PRRTE_CLASS_INSTANCE(prrte_grpcomm_base_active_t,
                   prrte_list_item_t,
                   NULL, NULL);

static void scon(prrte_grpcomm_signature_t *p)
{
    p->signature = NULL;
    p->sz = 0;
}
static void sdes(prrte_grpcomm_signature_t *p)
{
    if (NULL != p->signature) {
        free(p->signature);
    }
}
PRRTE_CLASS_INSTANCE(prrte_grpcomm_signature_t,
                   prrte_object_t,
                   scon, sdes);

static void ccon(prrte_grpcomm_coll_t *p)
{
    p->sig = NULL;
    PRRTE_CONSTRUCT(&p->bucket, prrte_buffer_t);
    PRRTE_CONSTRUCT(&p->distance_mask_recv, prrte_bitmap_t);
    p->dmns = NULL;
    p->ndmns = 0;
    p->nexpected = 0;
    p->nreported = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
    p->buffers = NULL;
}
static void cdes(prrte_grpcomm_coll_t *p)
{
    if (NULL != p->sig) {
        PRRTE_RELEASE(p->sig);
    }
    PRRTE_DESTRUCT(&p->bucket);
    PRRTE_DESTRUCT(&p->distance_mask_recv);
    free(p->dmns);
    free(p->buffers);
}
PRRTE_CLASS_INSTANCE(prrte_grpcomm_coll_t,
                   prrte_list_item_t,
                   ccon, cdes);
