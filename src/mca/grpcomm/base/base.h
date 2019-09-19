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
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_GRPCOMM_BASE_H
#define MCA_GRPCOMM_BASE_H

/*
 * includes
 */
#include "prrte_config.h"

#include "src/class/prrte_list.h"
#include "src/class/prrte_hash_table.h"
#include "src/dss/dss_types.h"
#include "src/mca/base/prrte_mca_base_framework.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/mca.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/grpcomm/grpcomm.h"


/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

/*
 * MCA framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_grpcomm_base_framework;
/*
 * Select an available component.
 */
PRRTE_EXPORT int prrte_grpcomm_base_select(void);

/*
 * globals that might be needed
 */
typedef struct {
    prrte_list_item_t super;
    int pri;
    prrte_grpcomm_base_module_t *module;
    prrte_mca_base_component_t *component;
} prrte_grpcomm_base_active_t;
PRRTE_CLASS_DECLARATION(prrte_grpcomm_base_active_t);

typedef struct {
    prrte_list_t actives;
    prrte_list_t ongoing;
    prrte_hash_table_t sig_table;
    char *transports;
    size_t context_id;
} prrte_grpcomm_base_t;

PRRTE_EXPORT extern prrte_grpcomm_base_t prrte_grpcomm_base;

/* Public API stubs */
PRRTE_EXPORT int prrte_grpcomm_API_xcast(prrte_grpcomm_signature_t *sig,
                                         prrte_rml_tag_t tag,
                                         prrte_buffer_t *buf);

PRRTE_EXPORT int prrte_grpcomm_API_allgather(prrte_grpcomm_signature_t *sig,
                                             prrte_buffer_t *buf, int mode,
                                             prrte_grpcomm_cbfunc_t cbfunc,
                                             void *cbdata);

PRRTE_EXPORT prrte_grpcomm_coll_t* prrte_grpcomm_base_get_tracker(prrte_grpcomm_signature_t *sig, bool create);
PRRTE_EXPORT void prrte_grpcomm_base_mark_distance_recv(prrte_grpcomm_coll_t *coll, uint32_t distance);
PRRTE_EXPORT unsigned int prrte_grpcomm_base_check_distance_recv(prrte_grpcomm_coll_t *coll, uint32_t distance);

END_C_DECLS
#endif
