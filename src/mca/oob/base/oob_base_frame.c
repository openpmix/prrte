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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include "src/class/prrte_bitmap.h"
#include "src/mca/mca.h"
#include "src/runtime/prrte_progress_threads.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/mca/rml/base/base.h"
#include "src/mca/oob/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/oob/base/static-components.h"

/*
 * Global variables
 */
prrte_oob_base_t prrte_oob_base = {0};

static int prrte_oob_base_close(void)
{
    prrte_oob_base_component_t *component;
    prrte_mca_base_component_list_item_t *cli;
    prrte_object_t *value;
    uint64_t key;

    /* shutdown all active transports */
    while (NULL != (cli = (prrte_mca_base_component_list_item_t *) prrte_list_remove_first (&prrte_oob_base.actives))) {
        component = (prrte_oob_base_component_t*)cli->cli_component;
        if (NULL != component->shutdown) {
            component->shutdown();
        }
        PRRTE_RELEASE(cli);
    }

    /* destruct our internal lists */
    PRRTE_DESTRUCT(&prrte_oob_base.actives);

    /* release all peers from the hash table */
    PRRTE_HASH_TABLE_FOREACH(key, uint64, value, &prrte_oob_base.peers) {
        if (NULL != value) {
            PRRTE_RELEASE(value);
        }
    }

    PRRTE_DESTRUCT(&prrte_oob_base.peers);

    return prrte_mca_base_framework_components_close(&prrte_oob_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
static int prrte_oob_base_open(prrte_mca_base_open_flag_t flags)
{
    /* setup globals */
    prrte_oob_base.max_uri_length = -1;
    PRRTE_CONSTRUCT(&prrte_oob_base.peers, prrte_hash_table_t);
    prrte_hash_table_init(&prrte_oob_base.peers, 128);
    PRRTE_CONSTRUCT(&prrte_oob_base.actives, prrte_list_t);

     /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_oob_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, oob, "Out-of-Band Messaging Subsystem",
                                 NULL, prrte_oob_base_open, prrte_oob_base_close,
                                 prrte_oob_base_static_components, 0);


PRRTE_CLASS_INSTANCE(prrte_oob_send_t,
                   prrte_object_t,
                   NULL, NULL);

static void pr_cons(prrte_oob_base_peer_t *ptr)
{
    ptr->component = NULL;
    PRRTE_CONSTRUCT(&ptr->addressable, prrte_bitmap_t);
    prrte_bitmap_init(&ptr->addressable, 8);
}
static void pr_des(prrte_oob_base_peer_t *ptr)
{
    PRRTE_DESTRUCT(&ptr->addressable);
}
PRRTE_CLASS_INSTANCE(prrte_oob_base_peer_t,
                   prrte_object_t,
                   pr_cons, pr_des);
