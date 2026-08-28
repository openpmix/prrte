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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#ifndef PRTE_DATA_SERVER_H
#define PRTE_DATA_SERVER_H

#include "prte_config.h"
#include "types.h"

#include "src/rml/rml_types.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

#define PRTE_PMIX_PUBLISH_CMD    0x01
#define PRTE_PMIX_LOOKUP_CMD     0x02
#define PRTE_PMIX_UNPUBLISH_CMD  0x03
#define PRTE_PMIX_PURGE_PROC_CMD 0x04

/* Directive for PMIx_Publish: replace this publisher's OWN prior
 * publication of the keys being published rather than failing with
 * PMIX_ERR_DUPLICATE_KEY.
 *
 * The Standard defines no such attribute, and refusing duplicates creates
 * the need for one: a process that wants to update a value it published
 * itself would otherwise have to PMIx_Unpublish first, and a publisher
 * that skipped that step used to get a silent no-op.  The directive reaches
 * only the caller's own publications - a published item belongs to the
 * process that published it, the same rule PMIx_Unpublish applies - so it
 * is a republish and not a way to take a live name away from somebody
 * else.  A publish whose keys collide with ANOTHER publisher's is refused
 * with or without it.
 */
#define PRTE_PUBLISH_REPLACE     "prte.pub.replace"     // (bool) replace the caller's own prior publication of these keys

/* The effective uid and gid of the process a RELAYED request is being made
 * on behalf of, carried beside PMIX_REQUESTOR and honored under the same
 * rule: only a tool may claim them.
 *
 * A cross-DVM request arrives under the relaying daemon's own tool
 * identity, and PMIx appends that identity's PMIX_USERID and PMIX_GRPID to
 * the array it hands us.  Ownership is decided by the publishing USER, so
 * without these the far end would store an item under the relay's uid and
 * test every later removal against it.  They are PRRTE-private keys rather
 * than a second PMIX_USERID precisely because PMIx adds its own: two
 * entries under one key would leave the reader guessing which came from
 * where, and the answer would depend on array order.
 */
#define PRTE_PUBLISH_REQ_UID     "prte.pub.ruid"        // (uint32) effective uid of the process a relay is acting for
#define PRTE_PUBLISH_REQ_GID     "prte.pub.rgid"        // (uint32) effective gid of the process a relay is acting for

/* Which application ended, on a purge carrying a PMIX_PERSIST_APP horizon.
 *
 * A purge names whose data is in question with a pmix_proc_t, and an
 * application is not a process: an MPMD job's applications share the one
 * namespace assigned to the job, so the namespace and a wildcard rank name
 * every app at once.  The horizons that need to say more than a process
 * name can carry say it here - this one, and PMIX_SESSION_ID for
 * PMIX_PERSIST_SESSION.
 */
#define PRTE_PURGE_APP_IDX       "prte.purge.appidx"    // (uint32) index of the application that terminated

/* Purge THIS process's store directly, with no message and no reply.
 *
 * The store that has to act is in every case one the caller already has in
 * hand: a daemon reaps its own children and is told when a namespace ends,
 * and the master holds every job object.  So a lifetime ending is a
 * function call rather than a message - which is what makes the PROC
 * horizon affordable at all, since that one fires once per terminating
 * process.
 *
 * `target` names whose data goes, with PMIX_RANK_WILDCARD for "any rank of
 * this namespace" and an empty namespace for "anybody" (the session
 * horizon, where the session id is what selects).  `qualifier` is the app
 * index for PMIX_PERSIST_APP and the session id for PMIX_PERSIST_SESSION,
 * and is ignored otherwise. */
PRTE_EXPORT void prte_data_server_purge_local(const pmix_proc_t *target,
                                              pmix_persistence_t horizon,
                                              uint32_t qualifier);

/* provide hooks to startup and finalize the data server */
PRTE_EXPORT int prte_data_server_init(void);
PRTE_EXPORT void prte_data_server_finalize(void);

/* provide hook for the non-blocking receive */
PRTE_EXPORT void prte_data_server(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata);

END_C_DECLS

#endif /* PRTE_DATA_SERVER_H */
