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
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/mca.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/output.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */

#include "src/mca/grpcomm/base/static-components.h"

/*
 * Global variables
 */
prte_grpcomm_base_t prte_grpcomm_base = {{{0}}};

prte_grpcomm_API_module_t prte_grpcomm = {.xcast = prte_grpcomm_API_xcast,
                                          .allgather = prte_grpcomm_API_allgather,
                                          .rbcast = prte_grpcomm_API_rbcast,
                                          .register_cb = prte_grpcomm_API_register_cb,
                                          .unregister_cb = NULL};

static int base_register(prte_mca_base_register_flag_t flags)
{
    prte_grpcomm_base.context_id = 1;
    prte_mca_base_var_register("prte", "grpcomm", "base", "starting_context_id",
                               "Starting value for assigning context id\'s",
                               PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_grpcomm_base.context_id);

    return PRTE_SUCCESS;
}

static int prte_grpcomm_base_close(void)
{
    prte_grpcomm_base_active_t *active;
    void *key;
    size_t size;
    uint32_t *seq_number;

    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST);

    /* Close the active modules */
    PRTE_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t)
    {
        if (NULL != active->module->finalize) {
            active->module->finalize();
        }
    }
    PRTE_LIST_DESTRUCT(&prte_grpcomm_base.actives);
    PRTE_LIST_DESTRUCT(&prte_grpcomm_base.ongoing);
    for (void *_nptr = NULL;
         PRTE_SUCCESS
         == prte_hash_table_get_next_key_ptr(&prte_grpcomm_base.sig_table, &key, &size,
                                             (void **) &seq_number, _nptr, &_nptr);) {
        free(seq_number);
    }
    PRTE_DESTRUCT(&prte_grpcomm_base.sig_table);

    return prte_mca_base_framework_components_close(&prte_grpcomm_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_grpcomm_base_open(prte_mca_base_open_flag_t flags)
{
    PRTE_CONSTRUCT(&prte_grpcomm_base.actives, prte_list_t);
    PRTE_CONSTRUCT(&prte_grpcomm_base.ongoing, prte_list_t);
    PRTE_CONSTRUCT(&prte_grpcomm_base.sig_table, prte_hash_table_t);
    prte_hash_table_init(&prte_grpcomm_base.sig_table, 128);
    prte_grpcomm_base.context_id = UINT32_MAX;

    return prte_mca_base_framework_components_open(&prte_grpcomm_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, grpcomm, "GRPCOMM", base_register, prte_grpcomm_base_open,
                                prte_grpcomm_base_close, prte_grpcomm_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

PRTE_CLASS_INSTANCE(prte_grpcomm_base_active_t, prte_list_item_t, NULL, NULL);

static void scon(prte_grpcomm_signature_t *p)
{
    p->signature = NULL;
    p->sz = 0;
}
static void sdes(prte_grpcomm_signature_t *p)
{
    if (NULL != p->signature) {
        free(p->signature);
    }
}
PRTE_CLASS_INSTANCE(prte_grpcomm_signature_t, prte_object_t, scon, sdes);

static void ccon(prte_grpcomm_coll_t *p)
{
    p->sig = NULL;
    PMIX_DATA_BUFFER_CONSTRUCT(&p->bucket);
    PRTE_CONSTRUCT(&p->distance_mask_recv, prte_bitmap_t);
    p->dmns = NULL;
    p->ndmns = 0;
    p->nexpected = 0;
    p->nreported = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
    p->buffers = NULL;
}
static void cdes(prte_grpcomm_coll_t *p)
{
    if (NULL != p->sig) {
        PRTE_RELEASE(p->sig);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&p->bucket);
    PRTE_DESTRUCT(&p->distance_mask_recv);
    free(p->dmns);
    free(p->buffers);
}
PRTE_CLASS_INSTANCE(prte_grpcomm_coll_t, prte_list_item_t, ccon, cdes);
