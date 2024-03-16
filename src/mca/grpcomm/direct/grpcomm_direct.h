/* -*- C -*-
 *
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef GRPCOMM_DIRECT_H
#define GRPCOMM_DIRECT_H

#include "prte_config.h"

#include "src/mca/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/*
 * Grpcomm interfaces
 */

typedef struct {
	prte_grpcomm_base_component_t super;
	// track ongoing fence operations - list of prte_grpcomm_fence_t
	pmix_list_t fence_ops;
	// track ongoiong group operations - list of prte_grpcomm_group_t
	pmix_list_t group_ops;
} prte_grpcomm_direct_component_t;

PRTE_MODULE_EXPORT extern prte_grpcomm_direct_component_t prte_mca_grpcomm_direct_component;
extern prte_grpcomm_base_module_t prte_grpcomm_direct_module;


/* Define collective signatures so we don't need to
 * track global collective id's. We provide a unique
 * signature struct for each collective type so that
 * they can be customized for that collective without
 * interfering with other collectives */
typedef struct {
    pmix_object_t super;
    pmix_proc_t *signature;
    size_t sz;
} prte_grpcomm_direct_fence_signature_t;
PRTE_MODULE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_direct_fence_signature_t);

typedef struct {
    pmix_object_t super;
    pmix_group_operation_t op;
    char *groupID;
    bool assignID;
    size_t ctxid;
    bool ctxid_assigned;
    pmix_proc_t *members; // initially supplied procs
    size_t nmembers;
    size_t bootstrap;
    pmix_proc_t *addmembers;  // procs supplied as add-members
    size_t naddmembers;
} prte_grpcomm_direct_group_signature_t;
PRTE_MODULE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_direct_group_signature_t);


/* Internal component object for tracking ongoing
 * allgather operations */
typedef struct {
    pmix_list_item_t super;
    /* collective's signature */
    prte_grpcomm_direct_fence_signature_t *sig;
    pmix_status_t status;
    /* collection bucket */
    pmix_data_buffer_t bucket;
    /* participating daemons */
    pmix_rank_t *dmns;
    /** number of participating daemons */
    size_t ndmns;
    /** my index in the dmns array */
    unsigned long my_rank;
    /* number of buckets expected */
    size_t nexpected;
    /* number reported in */
    size_t nreported;
    /* controls values */
    int timeout;
    /* callback function */
    pmix_modex_cbfunc_t cbfunc;
    /* user-provided callback data */
    void *cbdata;
} prte_grpcomm_fence_t;
PMIX_CLASS_DECLARATION(prte_grpcomm_fence_t);

/* Internal component object for tracking ongoing
 * group operations */
typedef struct {
    pmix_list_item_t super;
    /* collective's signature */
    prte_grpcomm_direct_group_signature_t *sig;
    pmix_status_t status;
    /* participating daemons */
    pmix_rank_t *dmns;
    /** number of participating daemons */
    size_t ndmns;
    /** my index in the dmns array */
    unsigned long my_rank;
    /* number of buckets expected */
    size_t nexpected;
    /* number reported in */
    size_t nreported;
    /* controls values */
    bool assignID;
    int timeout;
    size_t memsize;
    void *grpinfo;  // info list of group info
    void *endpts;   // info list of endpts
    /* callback function */
    pmix_info_cbfunc_t cbfunc;
    /* user-provided callback data */
    void *cbdata;
} prte_grpcomm_group_t;
PMIX_CLASS_DECLARATION(prte_grpcomm_group_t);

typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_grpcomm_direct_fence_signature_t *sig;
    pmix_data_buffer_t *buf;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_info_t *info;
    size_t ninfo;
    char *data;
    size_t ndata;
    pmix_modex_cbfunc_t cbfunc;
    void *cbdata;
} prte_pmix_fence_caddy_t;
PMIX_CLASS_DECLARATION(prte_pmix_fence_caddy_t);


/* xcast functions */
PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_xcast(prte_rml_tag_t tag,
                              pmix_data_buffer_t *msg);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_xcast_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tg, void *cbdata);

/* fence functions */
PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_fence(const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t info[], size_t ninfo, char *data,
                              size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_fence_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_fence_release(int status, pmix_proc_t *sender,
                                	   pmix_data_buffer_t *buffer,
                                	   prte_rml_tag_t tag, void *cbdata);


/* group functions */
PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_group(pmix_group_operation_t op, char *grpid,
                              const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_info_cbfunc_t cbfunc, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_grp_recv(int status, pmix_proc_t *sender,
                                  pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata);


PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_grp_release(int status, pmix_proc_t *sender,
                                	 pmix_data_buffer_t *buffer,
                                	 prte_rml_tag_t tag, void *cbdata);

END_C_DECLS

#endif
