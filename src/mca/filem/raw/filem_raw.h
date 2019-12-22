/*
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRtE_FILEM_RAW_EXPORT_H
#define PRRtE_FILEM_RAW_EXPORT_H

#include "prrte_config.h"

#include "src/mca/mca.h"
#include "src/class/prrte_object.h"
#include "src/event/event-internal.h"

#include "src/mca/filem/filem.h"

BEGIN_C_DECLS

PRRTE_MODULE_EXPORT extern prrte_filem_base_component_t prrte_filem_raw_component;
PRRTE_EXPORT extern prrte_filem_base_module_t prrte_filem_raw_module;

extern bool prrte_filem_raw_flatten_trees;

#define PRRTE_FILEM_RAW_CHUNK_MAX 16384

/* local classes */
typedef struct {
    prrte_list_item_t super;
    prrte_list_t xfers;
    int32_t status;
    prrte_filem_completion_cbfunc_t cbfunc;
    void *cbdata;
} prrte_filem_raw_outbound_t;
PRRTE_CLASS_DECLARATION(prrte_filem_raw_outbound_t);

typedef struct {
    prrte_list_item_t super;
    prrte_filem_raw_outbound_t *outbound;
    prrte_app_idx_t app_idx;
    prrte_event_t ev;
    bool pending;
    char *src;
    char *file;
    int32_t type;
    int32_t nchunk;
    int status;
    prrte_vpid_t nrecvd;
} prrte_filem_raw_xfer_t;
PRRTE_CLASS_DECLARATION(prrte_filem_raw_xfer_t);

typedef struct {
    prrte_list_item_t super;
    prrte_app_idx_t app_idx;
    prrte_event_t ev;
    bool pending;
    int fd;
    char *file;
    char *top;
    char *fullpath;
    int32_t type;
    char **link_pts;
    prrte_list_t outputs;
} prrte_filem_raw_incoming_t;
PRRTE_CLASS_DECLARATION(prrte_filem_raw_incoming_t);

typedef struct {
    prrte_list_item_t super;
    int numbytes;
    unsigned char data[PRRTE_FILEM_RAW_CHUNK_MAX];
} prrte_filem_raw_output_t;
PRRTE_CLASS_DECLARATION(prrte_filem_raw_output_t);

END_C_DECLS

#endif /* PRRtE_FILEM_RAW_EXPORT_H */
