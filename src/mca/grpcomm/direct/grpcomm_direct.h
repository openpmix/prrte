/* -*- C -*-
 *
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
    bool follower;
    pmix_proc_t *addmembers;  // procs supplied as add-members
    size_t naddmembers;
    pmix_proc_t *final_order;
    size_t nfinal;
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
    /* type of collective */
    bool bootstrap;

    /*** NON-BOOTSTRAP TRACKERS ***/
    size_t nexpected;  // number of buckets expected
    size_t nreported;  // number reported in

    /*** BOOTSTRAP TRACKERS ***/
    // "leaders" are group members reporting as
    // themselves for bootstrap - they know how
    // many leaders there are (which is in the bootstrap
    // parameter), but not who they are. Bootstrap is
    // complete when nleaders_reported == bootstrap
    // AND naddmembers_reported == naddmembers
    size_t nleaders;  // number of leaders expected
    size_t nleaders_reported;  // number reported in
    // "add-members" are procs that report with NULL
    // for the proc parameter - thereby indicating that
    // they don't know the other procs in the group
    size_t nfollowers;  // number of add-member procs expected to participate
    size_t nfollowers_reported;  // number reported in

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

#if PMIX_NUMERIC_VERSION >= 0x00060000

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_grp_recv(int status, pmix_proc_t *sender,
                                  pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata);


PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_grp_release(int status, pmix_proc_t *sender,
                                	 pmix_data_buffer_t *buffer,
                                	 prte_rml_tag_t tag, void *cbdata);

static inline void print_signature(prte_grpcomm_direct_group_signature_t *sig)
{
    char **msg = NULL;
    char *tmp;
    size_t n;

    PMIx_Argv_append_nosize(&msg, "SIGNATURE:");
    pmix_asprintf(&tmp, "\tOP: %s", PMIx_Group_operation_string(sig->op));
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tGRPID: %s", sig->groupID);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tASSIGN CTXID: %s", sig->assignID ? "T" : "F");
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    if (sig->assignID) {
        pmix_asprintf(&tmp, "\tCTXID: %lu", sig->ctxid);
        PMIx_Argv_append_nosize(&msg, tmp);
        free(tmp);
    }

    pmix_asprintf(&tmp, "\tNMEMBERS: %lu", sig->nmembers);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);
    if (0 < sig->nmembers) {
        for (n=0; n < sig->nmembers; n++) {
            pmix_asprintf(&tmp, "\t\t%s", PMIX_NAME_PRINT(&sig->members[n]));
            PMIx_Argv_append_nosize(&msg, tmp);
            free(tmp);
        }
    }

    pmix_asprintf(&tmp, "\tBOOTSTRAP: %lu", sig->bootstrap);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tFOLLOWER: %s", sig->follower ? "T" : "F");
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tNADDMEMBERS: %lu", sig->naddmembers);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);
    if (0 < sig->naddmembers) {
        for (n=0; n < sig->naddmembers; n++) {
            pmix_asprintf(&tmp, "\t\t%s", PMIX_NAME_PRINT(&sig->addmembers[n]));
            PMIx_Argv_append_nosize(&msg, tmp);
            free(tmp);
        }
    }

    tmp = PMIx_Argv_join(msg, '\n');
    PMIx_Argv_free(msg);
    pmix_output(0, "%s", tmp);
    free(tmp);
}
#endif

END_C_DECLS

#endif
