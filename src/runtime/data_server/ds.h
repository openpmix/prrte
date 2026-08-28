/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Data server for PRTE
 */
#ifndef PRTE_DS_INTERNAL_H
#define PRTE_DS_INTERNAL_H

#include "prte_config.h"
#include "types.h"

#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/* define an object to hold data */
typedef struct {
    /* base object */
    pmix_object_t super;
    /* index of this object in the storage array */
    int32_t index;
    // daemon that sent the data
    pmix_proc_t proxy;
    /* process that owns this data - only the
     * owner can remove it
     */
    pmix_proc_t owner;
    /* effective uid and gid of the owner - absent an accessor list,
     * these ARE the access rule: the data belongs to its publisher */
    uint32_t uid;
    uint32_t gid;
    /* the accessor lists the publisher gave, if any
     * (PMIX_ACCESS_USERIDS / PMIX_ACCESS_GRPIDS, either at the top level
     * or inside a PMIX_ACCESS_PERMISSIONS array).  Each list that is
     * present is a requirement the requestor must meet. */
    uint32_t *auids;
    size_t nauids;
    uint32_t *agids;
    size_t nagids;
    /* characteristics */
    pmix_data_range_t range;
    pmix_persistence_t persistence;
    /* and the values themselves - we store them as a list
     * because we may (if persistence is set to "first-read")
     * remove them upon read */
    pmix_list_t info;
} prte_data_object_t;
PMIX_CLASS_DECLARATION(prte_data_object_t);


/* define a request object for delayed answers */
typedef struct {
    pmix_list_item_t super;
    /* the timeout event, armed only for a parked request that was given a
     * PMIX_TIMEOUT.  Named "ev" by the caddy convention */
    prte_event_t ev;
    bool timer_active;
    pmix_proc_t proxy;
    pmix_proc_t requestor;
    int room_number;
    /* effective uid and gid of the requestor - what an accessor list, or
     * the publisher's own identity, is checked against */
    uint32_t uid;
    uint32_t gid;
    pmix_data_range_t range;
    char **keys;
} prte_data_req_t;
PMIX_CLASS_DECLARATION(prte_data_req_t);


/* define a container for data object cleanups */
typedef struct {
    pmix_list_item_t super;
    prte_data_object_t *data;
} prte_data_cleanup_t;


/* define a caddy for pointing to pmix_info_t that
 * are to be included in an answer */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t source;
    pmix_info_t info;
} prte_ds_info_t;
PMIX_CLASS_DECLARATION(prte_ds_info_t);


typedef struct {
    pmix_pointer_array_t store;
    pmix_list_t pending;
    int output;
    int verbosity;
} prte_data_store_t;

extern prte_data_store_t prte_data_store;

PRTE_EXPORT pmix_status_t prte_ds_publish(pmix_proc_t *sender,
                                          pmix_data_buffer_t *buffer,
                                          pmix_data_buffer_t *answer);

PRTE_EXPORT pmix_status_t prte_ds_lookup(pmix_proc_t *sender, int room_number,
                                         pmix_data_buffer_t *buffer,
                                         pmix_data_buffer_t *answer);

PRTE_EXPORT pmix_status_t prte_ds_unpublish(pmix_proc_t *sender,
                                            pmix_data_buffer_t *buffer,
                                            pmix_data_buffer_t *answer);

PRTE_EXPORT void prte_ds_purge(pmix_proc_t *sender,
                               pmix_data_buffer_t *buffer,
                               pmix_data_buffer_t *answer);

/* Apply the PUBLISHER's range: may this requestor see this item?  This is
 * an access rule, so it governs lookup - not removal, which is a question
 * of ownership (see ds_unpublish.c). */
PRTE_EXPORT pmix_status_t prte_data_server_check_range(prte_data_req_t *req,
                                                       prte_data_object_t *data);

/* Apply the publisher's ACCESS PERMISSIONS: may this requestor's uid and
 * gid see this item?  A publisher that named no accessors keeps the data to
 * itself - the requestor must present the publisher's own uid and gid - and
 * each list it did name (PMIX_ACCESS_USERIDS, PMIX_ACCESS_GRPIDS) is a
 * requirement rather than a grant, so a requestor must satisfy every list
 * that is present.  Returns PMIX_ERR_NO_PERMISSIONS when refused, which is
 * the status the retrieval rules ask for. */
PRTE_EXPORT pmix_status_t prte_data_server_check_access(prte_data_req_t *req,
                                                        prte_data_object_t *data);

/* Has data with this persistence outlived the lifetime that just ended?
 *
 * The persistence values are not a numeric ladder that can be compared -
 * PMIX_PERSIST_INDEF is 0 and outlives all of them - so the ordering is
 * spelled out rather than derived.  A horizon of PMIX_PERSIST_INVALID means
 * no lifetime ended and the caller asked for everything, which is what an
 * explicit PMIx_Unpublish(NULL, ...) means: a publisher taking back all of
 * its own data regardless of how long it had asked for it to be kept.
 *
 * Two persistences are removed by NO horizon.  PMIX_PERSIST_INDEF is
 * retained until specifically deleted, and PMIX_PERSIST_FIRST_READ until
 * the read that consumes it - neither criterion is a lifetime, so neither
 * is met by one ending. */
PRTE_EXPORT bool prte_data_server_expires_by(pmix_persistence_t persist,
                                             pmix_persistence_t horizon);

/* Apply the REQUESTER's range: is this publisher one the lookup asked to
 * search?  The PMIx retrieval rules constrain a lookup to data whose
 * publisher falls within the range the requester gave (the default being
 * PMIX_RANGE_SESSION), which is what keeps duplicate keys published on
 * different ranges apart.  Both checks have to pass. */
PRTE_EXPORT pmix_status_t prte_data_server_check_search_range(prte_data_req_t *req,
                                                              prte_data_object_t *data);

/* Relay a request to the external data server named by
 * prte_data_server_uri, and answer the requesting daemon when it replies.
 * Returns PMIX_SUCCESS once it owns the request - including when the
 * request failed and it has already sent the failure - so the caller must
 * not answer one it handed over.  See ds_relay.c. */
PRTE_EXPORT pmix_status_t prte_ds_relay(pmix_proc_t *sender, int room_number,
                                        uint8_t command,
                                        pmix_data_buffer_t *buffer);

/* Honor the identity a RELAYED request claims: PMIX_REQUESTOR names the
 * process it is being made on behalf of, and PRTE_PUBLISH_REQ_UID /
 * PRTE_PUBLISH_REQ_GID that process's effective uid and gid.  Only a TOOL
 * may claim any of them: a daemon of another DVM attaches to us as a tool
 * and reissues what its own client asked for, whereas an application
 * process has no such standing and allowing it would let any process
 * publish - and unpublish - under another's identity.
 *
 * Call this AFTER scanning the array for your own directives, or PMIx's
 * own PMIX_USERID for the relay will overwrite the claimed one.  *uid and
 * *gid may be NULL where the caller has no use for them. */
PRTE_EXPORT void prte_ds_check_requestor(pmix_proc_t *owner,
                                         uint32_t *uid, uint32_t *gid,
                                         const pmix_info_t info[], size_t ninfo);

/* May a requestor presenting this uid and gid remove - or take back with
 * PRTE_PUBLISH_REPLACE - this item?
 *
 * Published data is owned by the USER that published it, not by the
 * process: a process that exits takes no data with it, and a later job of
 * the same user must be able to reclaim a name its predecessor left
 * behind.  The test is therefore the recorded uid, and the gid where both
 * are known.
 *
 * This is an OWNERSHIP question and has nothing to do with access.  An
 * accessor list widens who may READ an item and confers no removal; an
 * owner whose own accessor list excludes it may remove what it cannot
 * read. */
PRTE_EXPORT bool prte_data_server_owns(uint32_t uid, uint32_t gid,
                                       prte_data_object_t *data);

END_C_DECLS

#endif /* PRTE_DS_INTERNAL_H */
