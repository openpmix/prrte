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
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The PRTE Group Communications
 *
 * The PRTE Group Comm framework provides communication services that
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

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/mca.h"
#include "src/rml/rml_types.h"

BEGIN_C_DECLS

typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_lock_t lock;
    pmix_group_operation_t op;
    char *grpid;
    const pmix_proc_t *procs;
    size_t nprocs;
    const pmix_info_t *directives;
    size_t ndirs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} prte_pmix_grp_caddy_t;
PMIX_CLASS_DECLARATION(prte_pmix_grp_caddy_t);

/* define a callback function to be invoked upon
 * collective completion */
typedef void (*prte_grpcomm_cbfunc_t)(int status, pmix_data_buffer_t *buf, void *cbdata);

/*
 * Component functions - all MUST be provided!
 */

/* initialize the selected module */
typedef int (*prte_grpcomm_base_module_init_fn_t)(void);

/* finalize the selected module */
typedef void (*prte_grpcomm_base_module_finalize_fn_t)(void);


/* Scalably send a message. */
typedef int (*prte_grpcomm_base_module_xcast_fn_t)(prte_rml_tag_t tag,
                                                   pmix_data_buffer_t *msg);


/* fence - gather data from all specified procs. Barrier operations
 * will provide NULL data.
 *
 * NOTE: this is a non-blocking call. The callback function
 * will be invoked upon completion. */
typedef int (*prte_grpcomm_base_module_fence_fn_t)(const pmix_proc_t procs[], size_t nprocs,
                                                   const pmix_info_t info[], size_t ninfo, char *data,
                                                   size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata);


/* support group operations - this is basically a fence
 * operation, but there are enough differences to warrant keeping it
 * separate to avoid over-complicating the fence code */
typedef int (*prte_grpcomm_base_module_grp_fn_t)(pmix_group_operation_t op, char *grpid,
                                                 const pmix_proc_t procs[], size_t nprocs,
                                                 const pmix_info_t directives[], size_t ndirs,
                                                 pmix_info_cbfunc_t cbfunc, void *cbdata);
/*
 * Ver 4.0
 */
typedef struct {
    prte_grpcomm_base_module_init_fn_t          init;
    prte_grpcomm_base_module_finalize_fn_t      finalize;
    /* collective operations */
    prte_grpcomm_base_module_xcast_fn_t         xcast;
    prte_grpcomm_base_module_fence_fn_t         fence;
    prte_grpcomm_base_module_grp_fn_t           group;
} prte_grpcomm_base_module_t;

/*
 * the standard component data structure
 */
typedef pmix_mca_base_component_t prte_grpcomm_base_component_t;

/*
 * Macro for use in components that are of type grpcomm v3.0.0
 */
#define PRTE_GRPCOMM_BASE_VERSION_4_0_0       \
    /* grpcomm v4.0 is chained to MCA v2.0 */ \
    PRTE_MCA_BASE_VERSION_3_0_0("grpcomm", 4, 0, 0)

/* Global structure for accessing grpcomm functions */
PRTE_EXPORT extern prte_grpcomm_base_module_t prte_grpcomm;

END_C_DECLS

#endif
