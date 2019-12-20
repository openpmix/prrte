/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
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
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The OpenRTE Group Communications
 *
 * The OpenRTE Group Comm framework provides communication services that
 * span entire jobs or collections of processes. It is not intended to be
 * used for point-to-point communications (the RML does that), nor should
 * it be viewed as a high-performance communication channel for large-scale
 * data transfers.
 */

#ifndef MCA_GRPCOMM_H
#define MCA_GRPCOMM_H

/*
 * includes
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/class/prrte_list.h"
#include "src/class/prrte_bitmap.h"
#include "src/dss/dss_types.h"

#include "src/mca/rml/rml_types.h"

BEGIN_C_DECLS

/* define a callback function to be invoked upon
 * collective completion */
typedef void (*prrte_grpcomm_cbfunc_t)(int status, prrte_buffer_t *buf, void *cbdata);

/* Define a collective signature so we don't need to
 * track global collective id's */
typedef struct {
    prrte_object_t super;
    prrte_process_name_t *signature;
    size_t sz;
} prrte_grpcomm_signature_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_grpcomm_signature_t);

/* Internal component object for tracking ongoing
 * allgather operations */
typedef struct {
    prrte_list_item_t super;
    /* collective's signature */
    prrte_grpcomm_signature_t *sig;
    /* collection bucket */
    prrte_buffer_t bucket;
    /* participating daemons */
    prrte_vpid_t *dmns;
    /** number of participating daemons */
    size_t ndmns;
    /** my index in the dmns array */
    unsigned long my_rank;
    /* number of buckets expected */
    size_t nexpected;
    /* number reported in */
    size_t nreported;
    /* distance masks for receive */
    prrte_bitmap_t distance_mask_recv;
    /* received buckets */
    prrte_buffer_t ** buffers;
    /* callback function */
    prrte_grpcomm_cbfunc_t cbfunc;
    /* user-provided callback data */
    void *cbdata;
} prrte_grpcomm_coll_t;
PRRTE_CLASS_DECLARATION(prrte_grpcomm_coll_t);

/*
 * Component functions - all MUST be provided!
 */


/* initialize the selected module */
typedef int (*prrte_grpcomm_base_module_init_fn_t)(void);

/* finalize the selected module */
typedef void (*prrte_grpcomm_base_module_finalize_fn_t)(void);

/* Scalably send a message. Caller will provide an array
 * of daemon vpids that are to receive the message. A NULL
 * pointer indicates that all daemons are participating. */
typedef int (*prrte_grpcomm_base_module_xcast_fn_t)(prrte_vpid_t *vpids,
                                                   size_t nprocs,
                                                   prrte_buffer_t *msg);

/* allgather - gather data from all specified daemons. Barrier operations
 * will provide a zero-byte buffer. Caller will provide an array
 * of daemon vpids that are participating in the allgather via the
 * prrte_grpcomm_coll_t object. A NULL pointer indicates that all daemons
 * are participating.
 *
 * NOTE: this is a non-blocking call. The callback function cached in
 * the prrte_grpcomm_coll_t will be invoked upon completion. */
typedef int (*prrte_grpcomm_base_module_allgather_fn_t)(prrte_grpcomm_coll_t *coll,
                                                       prrte_buffer_t *buf, int mode);

/*
 * Ver 3.0 - internal modules
 */
typedef struct {
    prrte_grpcomm_base_module_init_fn_t           init;
    prrte_grpcomm_base_module_finalize_fn_t       finalize;
    /* collective operations */
    prrte_grpcomm_base_module_xcast_fn_t          xcast;
    prrte_grpcomm_base_module_allgather_fn_t      allgather;
} prrte_grpcomm_base_module_t;

/* the Public APIs */
/* Scalably send a message. Caller will provide an array
 * of process names that are to receive the message. A NULL
 * pointer indicates that all known procs are to receive
 * the message. A pointer to a name that includes PRRTE_VPID_WILDCARD
 * will send the message to all procs in the specified jobid.
 * The message will be sent to the daemons hosting the specified
 * procs for processing and relay. */
typedef int (*prrte_grpcomm_base_API_xcast_fn_t)(prrte_grpcomm_signature_t *sig,
                                                prrte_rml_tag_t tag,
                                                prrte_buffer_t *msg);

/* allgather - gather data from all specified procs. Barrier operations
 * will provide a zero-byte buffer. Caller will provide an array
 * of application proc vpids that are participating in the allgather. A NULL
 * pointer indicates that all known procs are participating. A pointer
 * to a name that includes PRRTE_VPID_WILDCARD indicates that all procs
 * in the specified jobid are contributing.
 *
 * NOTE: this is a non-blocking call. The provided callback function
 * will be invoked upon completion. */
typedef int (*prrte_grpcomm_base_API_allgather_fn_t)(prrte_grpcomm_signature_t *sig,
                                                    prrte_buffer_t *buf, int mode,
                                                    prrte_grpcomm_cbfunc_t cbfunc,
                                                    void *cbdata);
typedef struct {
    /* collective operations */
    prrte_grpcomm_base_API_xcast_fn_t             xcast;
    prrte_grpcomm_base_API_allgather_fn_t         allgather;
} prrte_grpcomm_API_module_t;


/*
 * the standard component data structure
 */
struct prrte_grpcomm_base_component_3_0_0_t {
    prrte_mca_base_component_t base_version;
    prrte_mca_base_component_data_t base_data;
};
typedef struct prrte_grpcomm_base_component_3_0_0_t prrte_grpcomm_base_component_3_0_0_t;
typedef prrte_grpcomm_base_component_3_0_0_t prrte_grpcomm_base_component_t;



/*
 * Macro for use in components that are of type grpcomm v3.0.0
 */
#define PRRTE_GRPCOMM_BASE_VERSION_3_0_0 \
    /* grpcomm v3.0 is chained to MCA v2.0 */ \
    PRRTE_MCA_BASE_VERSION_2_1_0("grpcomm", 3, 0, 0)

/* Global structure for accessing grpcomm functions */
PRRTE_EXPORT extern prrte_grpcomm_API_module_t prrte_grpcomm;

END_C_DECLS

#endif
