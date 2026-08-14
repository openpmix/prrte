/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2020      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/pmix_argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/rml/rml.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/util/nidmap.h"

int prte_util_nidmap_create(pmix_pointer_array_t *pool, pmix_data_buffer_t *buffer)
{
    char *raw = NULL;
    pmix_rank_t *vpids = NULL;
    int32_t *ndidx = NULL;
    uint8_t u8;
    int n, m, ndaemons, nbytes;
    pmix_rank_t span;
    bool compressed;
    char **names = NULL;
    char **aliases = NULL, **als;
    prte_node_t *nptr;
    pmix_byte_object_t bo;
    size_t sz;
    pmix_status_t rc;

    /* pack a flag indicating if the HNP was included in the allocation */
    if (prte_hnp_is_allocated) {
        u8 = 1;
    } else {
        u8 = 0;
    }
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &u8, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* Pack the size of the daemon vpid space, [0, span). This can be larger
     * than the number of live daemons packed below: a DVM shrink leaves a
     * permanent hole in the vpid space (the DVM never reuses a daemon vpid),
     * so the departed daemon has no entry in the name/vpid lists. Receivers
     * need this exact span - not the live count - so their routing tree covers
     * the same rank space the HNP uses, and so they can identify the holes as
     * permanently-dead ranks to route around (#2491).
     *
     * The span must cover every vpid we actually pack below. num_daemons is
     * normally that span, but it can transiently lag the pool: a bootstrap
     * daemon that departed and rebooted still has its node->daemon entry (so it
     * is packed), yet num_daemons is not restored until the daemon formally
     * reports its (re)launch. Encoding num_daemons as the span in that window
     * would exclude the top vpid, corrupting the receiver's routing tree. So
     * pre-scan the pool for the highest daemon vpid and use span =
     * max(num_daemons, highest_vpid + 1) - large enough to cover every packed
     * daemon while still preserving a top-of-range shrink hole. */
    span = prte_process_info.num_daemons;
    for (n = 0; n < pool->size; n++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(pool, n);
        if (NULL == nptr || NULL == nptr->daemon) {
            continue;
        }
        if (nptr->daemon->name.rank + 1 > span) {
            span = nptr->daemon->name.rank + 1;
        }
    }
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &span, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* daemon vpids start from 0 and increase linearly by one
     * up to the number of nodes in the system. The vpid is
     * a 32-bit value. We don't know how many of the nodes
     * in the system have daemons - we may not be using them
     * all just yet. However, even the largest systems won't
     * have more than a million nodes for quite some time,
     * so for now we'll just allocate enough space to hold
     * them all. Someone can optimize this further later. Size
     * the buffer to the span so it holds every packed vpid. */
    nbytes = span * sizeof(pmix_rank_t);
    vpids = (pmix_rank_t *) malloc(nbytes);
    if (NULL == vpids) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    /* Every node also carries the index of the slot it occupies in *our* node
     * pool.  That index is the node's identity across the DVM - it is the
     * PMIX_NODEID every daemon hands its local clients, and the subscript the
     * PMIX_SERVER_URI query resolves a nodeid through - so the receiver has to
     * place the node in the same slot we hold it in.  It cannot derive that
     * from the packing order: a node whose daemon has departed stays in our
     * pool with no daemon and is skipped below, so the packed sequence is
     * compacted while the pool is not.  Each node has exactly one daemon here,
     * so this list is no longer than the vpid list. */
    ndidx = (int32_t *) malloc(span * sizeof(int32_t));
    if (NULL == ndidx) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        free(vpids);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    ndaemons = 0;
    for (n = 0; n < pool->size; n++) {
        if (NULL == (nptr = (prte_node_t *) pmix_pointer_array_get_item(pool, n))) {
            continue;
        }
        if (NULL == nptr->daemon) {
            continue;
        }
        /* add the hostname to the argv */
        PMIx_Argv_append_nosize(&names, nptr->name);
        als = NULL;
        if (NULL != nptr->aliases) {
            for (m=0; NULL != nptr->aliases[m]; m++) {
                // skip any localhost entries
                if (0 == strcmp(nptr->aliases[m], "localhost") ||
                    0 == strcmp(nptr->aliases[m], "127.0.0.1")) {
                    continue;
                }
                PMIx_Argv_append_nosize(&als, nptr->aliases[m]);
            }
            raw = PMIx_Argv_join(als, ',');
            PMIx_Argv_free(als);
            PMIx_Argv_append_nosize(&aliases, raw);
            free(raw);
        } else {
            PMIx_Argv_append_nosize(&aliases, "PRTENONE");
        }
        /* store the vpid and the pool slot this node occupies */
        vpids[ndaemons] = nptr->daemon->name.rank;
        ndidx[ndaemons] = nptr->index;
        ++ndaemons;
    }

    /* little protection */
    if (NULL == names || NULL == aliases) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        free(vpids);
        free(ndidx);
        return PRTE_ERR_NOT_FOUND;
    }

    /* construct the string of node names for compression */
    raw = PMIx_Argv_join(names, ',');
    PMIx_Argv_free(names);
    if (PMIx_Data_compress((uint8_t *) raw, strlen(raw) + 1, (uint8_t **) &bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
        free(raw);
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = (char *) raw;
        bo.size = strlen(raw) + 1;
    }
    /* indicate compression */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(vpids);
        free(ndidx);
        return rc;
    }
    /* add the object */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(vpids);
        free(ndidx);
        return rc;
    }
    free(bo.bytes);
    bo.bytes = NULL;

    /* construct the string of aliases for compression */
    raw = PMIx_Argv_join(aliases, ';');
    PMIx_Argv_free(aliases);
    if (PMIx_Data_compress((uint8_t *) raw, strlen(raw) + 1, (uint8_t **) &bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
        free(raw);
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = (char *) raw;
        bo.size = strlen(raw) + 1;
    }
    /* indicate compression */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(vpids);
        free(ndidx);
        return rc;
    }
    /* add the object */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(vpids);
        free(ndidx);
        return rc;
    }
    free(bo.bytes);
    bo.bytes = NULL;

    /* compress the vpids. Only the entries we actually filled: the buffer is
     * sized to the vpid *span*, which exceeds the daemon count whenever the
     * DVM carries a shrink hole, and the tail past ndaemons is uninitialized
     * heap. The receiver reads one vpid per packed node name, so those bytes
     * were never wanted - they were just being compressed and shipped to
     * every daemon (and made the compressed and uncompressed encodings
     * disagree about the object's length). */
    nbytes = ndaemons * sizeof(pmix_rank_t);
    if (PMIx_Data_compress((uint8_t *) vpids, nbytes, (uint8_t **) &bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
        free(vpids);
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = (char *) vpids;
        bo.size = ndaemons * sizeof(pmix_rank_t);
    }
    /* indicate compression */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(ndidx);
        return rc;
    }
    /* add the object */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(ndidx);
        return rc;
    }
    free(bo.bytes);
    bo.bytes = NULL;

    /* compress the pool indices - again only the entries we filled */
    nbytes = ndaemons * sizeof(int32_t);
    if (PMIx_Data_compress((uint8_t *) ndidx, nbytes, (uint8_t **) &bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
        free(ndidx);
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = (char *) ndidx;
        bo.size = nbytes;
    }
    /* indicate compression */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        return rc;
    }
    /* add the object */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        return rc;
    }
    free(bo.bytes);

    return rc;
}

int prte_util_decode_nidmap(pmix_data_buffer_t *buf)
{
    uint8_t u8;
    pmix_rank_t *vpid = NULL;
    pmix_rank_t ndmns = 0;
    int32_t *ndidx = NULL;
    int cnt, n, nnodes;
    bool compressed;
    size_t sz, isz;
    pmix_byte_object_t pbo;
    char *raw = NULL, **names = NULL, **aliases = NULL;
    char *seen = NULL;
    prte_node_t *nd;
    prte_job_t *daemons;
    prte_proc_t *proc;
    prte_topology_t *t = NULL;
    pmix_status_t rc;

    /* unpack the flag indicating if HNP is in allocation */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &u8, &cnt, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (1 == u8) {
        prte_hnp_is_allocated = true;
    } else {
        prte_hnp_is_allocated = false;
    }

    /* unpack the size of the daemon vpid space (may exceed the live daemon
     * count when the DVM carries shrunk-out vpid holes - see nidmap_create) */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &ndmns, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack compression flag for node names */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &compressed, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack the nodename object */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, decompress */
    if (compressed) {
        if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &raw, &sz)) {
            PRTE_ERROR_LOG(PRTE_ERROR);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            rc = PRTE_ERROR;
            goto cleanup;
        }
    } else {
        raw = (char *) pbo.bytes;
        pbo.bytes = NULL; // protect the data
        pbo.size = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    names = PMIx_Argv_split(raw, ',');
    free(raw);

    /* unpack compression flag for node aliases */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &compressed, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack the aliases object */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, decompress */
    if (compressed) {
        if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &raw, &sz)) {
            PRTE_ERROR_LOG(PRTE_ERROR);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            rc = PRTE_ERROR;
            goto cleanup;
        }
    } else {
        raw = (char *) pbo.bytes;
        pbo.bytes = NULL; // protect the data
        pbo.size = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    aliases = PMIx_Argv_split(raw, ';');
    free(raw);

    /* unpack compression flag for daemon vpids */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &compressed, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack the vpid object */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, decompress */
    if (compressed) {
        if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &vpid, &sz)) {
            PRTE_ERROR_LOG(PRTE_ERROR);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            rc = PRTE_ERROR;
            goto cleanup;
        }
    } else {
        vpid = (pmix_rank_t *) pbo.bytes;
        sz = pbo.size;
        pbo.bytes = NULL;
        pbo.size = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

    /* unpack compression flag for the node pool indices */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &compressed, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack the pool index object */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, decompress */
    if (compressed) {
        if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &ndidx, &isz)) {
            PRTE_ERROR_LOG(PRTE_ERROR);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            rc = PRTE_ERROR;
            goto cleanup;
        }
    } else {
        ndidx = (int32_t *) pbo.bytes;
        isz = pbo.size;
        pbo.bytes = NULL;
        pbo.size = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

    /* if we are the HNP, we don't need any of this stuff */
    if (PRTE_PROC_IS_MASTER) {
        rc = PRTE_SUCCESS;
        goto cleanup;
    }

    /* get the daemon job object */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons) {
        /* should never happen - we are a daemon reading our own DVM's map */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* the four arrays are built in lockstep by nidmap_create, so anything
     * shorter than the node-name list means the message is not one of ours -
     * and the loop below indexes all four by the same subscript */
    nnodes = (NULL == names) ? 0 : PMIx_Argv_count(names);
    if (NULL == names || NULL == aliases || NULL == vpid || NULL == ndidx
        || nnodes > PMIx_Argv_count(aliases)
        || (size_t) nnodes > sz / sizeof(pmix_rank_t)
        || (size_t) nnodes > isz / sizeof(int32_t)) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        rc = PRTE_ERR_BAD_PARAM;
        goto cleanup;
    }

    /* get our topology */
    t = (prte_topology_t *) pmix_pointer_array_get_item(prte_node_topologies, 0);
    if (NULL == t) {
        /* should never happen */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    /* Rebuild the node pool - this will include _all_ nodes known to the
     * allocation.  Every node goes in the slot the sender holds it in, and
     * every node is (re)bound to the daemon the sender says is on it: a DVM
     * that has grown, shrunk, or both hands us this map more than once, and
     * neither the node's slot nor its daemon is what it was the first time.
     * Doing the binding only for a slot we did not already have - which is
     * what this loop used to do - meant that after the first decode populated
     * the pool, every later decode was very nearly a no-op: a daemon that
     * predated a grow never learned the new daemon's vpid at all, so
     * daemons->procs had a hole where odls looks up the parent of every proc
     * in a job, and that daemon launched nothing (#2616). */
    for (n = 0; n < nnodes; n++) {
        if (0 > ndidx[n]) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            rc = PRTE_ERR_BAD_PARAM;
            goto cleanup;
        }
        nd = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, ndidx[n]);
        if (NULL == nd) {
            /* add this node to the pool, in the sender's slot */
            nd = PMIX_NEW(prte_node_t);
            nd->name = strdup(names[n]);
            nd->index = ndidx[n];
            pmix_pointer_array_set_item(prte_node_pool, ndidx[n], nd);
            /* set the topology - always default to homogeneous
             * as that is the most common scenario. Retain it: the node holds a
             * counted reference so the topology cannot go away underneath it */
            PMIX_RETAIN(t);
            nd->topology = t;
        } else if (0 != strcmp(nd->name, names[n])) {
            /* the sender has put a different machine in this slot - the old
             * name's aliases are not this one's, so they go too rather than
             * surviving into a node that never claimed them */
            free(nd->name);
            nd->name = strdup(names[n]);
            if (NULL != nd->aliases) {
                PMIx_Argv_free(nd->aliases);
                nd->aliases = NULL;
            }
        }
        /* refresh the aliases */
        if (0 != strcmp(aliases[n], "PRTENONE")) {
            if (NULL != nd->aliases) {
                PMIx_Argv_free(nd->aliases);
            }
            nd->aliases = PMIx_Argv_split(aliases[n], ',');
        }
        /* record the daemon on it */
        proc = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, vpid[n]);
        if (NULL == proc) {
            proc = PMIX_NEW(prte_proc_t);
            PMIX_LOAD_PROCID(&proc->name, PRTE_PROC_MY_NAME->nspace, vpid[n]);
            proc->state = PRTE_PROC_STATE_RUNNING;
            PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_ALIVE);
            daemons->num_procs++;
            pmix_pointer_array_set_item(daemons->procs, proc->name.rank, proc);
        }
        /* the node backpointer is borrowed, not retained (see
         * prte_proc_destruct) */
        proc->node = nd;
        if (nd->daemon != proc) {
            /* the node holds a counted reference on its daemon, so a rebind
             * has to drop the one it was holding */
            if (NULL != nd->daemon) {
                PMIX_RELEASE(nd->daemon);
            }
            PMIX_RETAIN(proc);
            nd->daemon = proc;
        }
    }

    /* A node the sender did not name has no daemon on it any more - it was
     * shrunk out of the DVM, or its grow was rolled back.  The sender cleared
     * its own backpointer when that happened, and we have to do the same:
     * anything that reaches a daemon through the node pool (the
     * PMIX_SERVER_URI query does exactly that) would otherwise be handed a
     * proc for a vpid this DVM has permanently retired. */
    seen = (char *) calloc(prte_node_pool->size, sizeof(char));
    if (NULL == seen) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }
    for (n = 0; n < nnodes; n++) {
        seen[ndidx[n]] = 1;
    }
    for (n = 0; n < prte_node_pool->size; n++) {
        if (seen[n]) {
            continue;
        }
        nd = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
        if (NULL == nd || NULL == nd->daemon) {
            continue;
        }
        PMIX_RELEASE(nd->daemon);
        nd->daemon = NULL;
    }

    /* Record any vpid holes as departed ranks. The DVM spans [0, ndmns) daemon
     * vpids, but a shrunk-out (or not-yet-present bootstrap) daemon leaves a
     * hole with no entry above, so daemons->procs has a gap at that rank.
     * Marking the gap makes this daemon's routing tree route around the hole
     * exactly as the HNP and the surviving daemons do - closing the gap that a
     * brand-new daemon (empty failure set) would otherwise have (#2491). On an
     * unshrunk, fully-present DVM ndmns equals the live count, so nothing is
     * marked and behavior is unchanged. */
    bool newdead = false;
    for (pmix_rank_t r = 0; r < ndmns; r++) {
        if (NULL != pmix_pointer_array_get_item(daemons->procs, r)) {
            continue;
        }
        // In a bootstrapped DVM a vpid hole is not necessarily permanent: the
        // node can reboot and its daemon return with the same rank. Record it
        // as absent (clearable by the unheal path) rather than dead. In a
        // launched/elastic DVM the hole is permanent (#2491) and goes to
        // dead_dmns exactly as before.
        if (prte_bootstrap_setup) {
            if (!pmix_bitmap_is_set_bit(&prte_rml_base.absent_dmns, r)) {
                pmix_bitmap_set_bit(&prte_rml_base.absent_dmns, r);
                newdead = true;
            }
        } else if (!pmix_bitmap_is_set_bit(&prte_rml_base.dead_dmns, r)) {
            pmix_bitmap_set_bit(&prte_rml_base.dead_dmns, r);
            newdead = true;
        }
    }

    /* update num daemons and (re)build the routing tree if the vpid span grew
     * or a new hole appeared */
    if (prte_process_info.num_daemons != ndmns || newdead) {
        prte_process_info.num_daemons = ndmns;
        prte_rml_compute_routing_tree();
    }


cleanup:
    if (NULL != vpid) {
        free(vpid);
    }
    if (NULL != ndidx) {
        free(ndidx);
    }
    if (NULL != seen) {
        free(seen);
    }
    if (NULL != names) {
        PMIx_Argv_free(names);
    }
    /* the alias list was leaked on every single decode, including the clean
     * one - and every daemon decodes a nidmap on each DVM update */
    if (NULL != aliases) {
        PMIx_Argv_free(aliases);
    }
    return rc;
}

/* The jobs already running in the DVM.
 *
 * A daemon that has just joined the DVM has to be told about the jobs that
 * were already running in it, or a proc of one of those jobs interacting with
 * a proc of the new one lands on a daemon that cannot resolve its namespace.
 * This used to be packed into the launch message - by definition the launch
 * that brought the daemons in - which tied the size of every such launch
 * message to the number of jobs resident in the DVM, and sent them to every
 * daemon in it rather than to the ones that needed them.  It belongs beside
 * the nidmap: the same message, sent on the same event, saying the same kind
 * of thing.
 *
 * "exclude" is the job whose launch brought the new daemons in.  Packing it
 * here would be worse than redundant: a daemon that has already recorded a
 * namespace *drops* the copy in the launch message, so a catch-up entry for
 * a job that has not been mapped yet would leave every daemon holding a
 * procless version of it forever.
 */
/* Is this one of the jobs a daemon joining the DVM needs to be told about?
 *
 * A connected tool is carried in prte_job_data as a job, and it is not one:
 * its proc runs on no daemon at all, so its "parent" is the
 * PMIX_RANK_INVALID prte_proc_construct left there, and a receiver has
 * nowhere to place it - it would fail the placement below and take the DVM
 * down with it.  No daemon but the one the tool connected through has any
 * use for it either.
 */
static bool catchup_worthy(prte_job_t *jptr, prte_job_t *exclude)
{
    if (NULL == jptr || jptr == exclude) {
        return false;
    }
    if (PRTE_FLAG_TEST(jptr, PRTE_JOB_FLAG_TOOL)) {
        return false;
    }
    return true;
}

int prte_util_pack_job_catchup(pmix_data_buffer_t *buffer, prte_job_t *exclude)
{
    prte_job_t *jptr;
    pmix_status_t rc;
    int32_t njobs = 0;
    int i;

    /* count what we are about to send - the receiver reads a count rather
     * than unpacking until the buffer runs out, because the per-daemon
     * wireup records follow it in the same message */
    for (i = 1; i < prte_job_data->size; i++) {
        jptr = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, i);
        if (!catchup_worthy(jptr, exclude)) {
            continue;
        }
        ++njobs;
    }
    rc = PMIx_Data_pack(NULL, buffer, &njobs, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 == njobs) {
        return PRTE_SUCCESS;
    }

    for (i = 1; i < prte_job_data->size; i++) {
        jptr = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, i);
        if (!catchup_worthy(jptr, exclude)) {
            continue;
        }
        /* prte_job_pack carries the job's node and proc maps, from which the
         * receiver rebuilds each proc's rank and hosting daemon - which is
         * why this has to be decoded after the nidmap above, and why the
         * launch message's second copy of the parent vpid was redundant.
         *
         * No cpusets: the only daemon that keeps what this message says is
         * one that did not already know the job (the decoder discards a job
         * it has), and a daemon that was not in the DVM when the job was
         * launched hosts none of its procs.  So the bindings would be
         * carried to the one place that cannot use them. */
        rc = prte_job_pack(buffer, jptr, PRTE_JOB_PACK_NO_CPUSETS);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}

int prte_util_decode_job_catchup(pmix_data_buffer_t *buffer)
{
    prte_job_t *jptr, *daemons;
    prte_proc_t *pptr, *dmn;
    prte_node_t *node;
    pmix_status_t rc;
    int32_t njobs, cnt;
    int n, v;

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &njobs, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 == njobs) {
        return PRTE_SUCCESS;
    }

    /* the nidmap was decoded ahead of us, so every daemon named in it now has
     * a proc object bound to its node - which is how we place these procs */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL == daemons) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    for (n = 0; n < njobs; n++) {
        jptr = NULL;
        rc = prte_job_unpack(buffer, &jptr, NULL);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* we may already know this one - every daemon gets this message, not
         * just the ones that just joined */
        if (NULL != prte_get_job_data_object(jptr->nspace)) {
            jptr->index = -1;
            PMIX_RELEASE(jptr);
            continue;
        }
        prte_set_job_data_object(jptr);
        if (NULL == jptr->map) {
            jptr->map = PMIX_NEW(prte_job_map_t);
        }

        for (v = 0; v < jptr->procs->size; v++) {
            pptr = (prte_proc_t *) pmix_pointer_array_get_item(jptr->procs, v);
            if (NULL == pptr) {
                continue;
            }
            dmn = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, pptr->parent);
            if (NULL == dmn || NULL == dmn->node) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                return PRTE_ERR_NOT_FOUND;
            }
            /* the node backpointer is borrowed, not retained */
            pptr->node = dmn->node;

            /* add the node to the job map, if needed */
            if (!PRTE_FLAG_TEST(pptr->node, PRTE_NODE_FLAG_MAPPED)) {
                PMIX_RETAIN(pptr->node);
                pmix_pointer_array_add(jptr->map->nodes, pptr->node);
                jptr->map->num_nodes++;
                PRTE_FLAG_SET(pptr->node, PRTE_NODE_FLAG_MAPPED);
            }
            /* add this proc to that node */
            PMIX_RETAIN(pptr);
            pmix_pointer_array_add(pptr->node->procs, pptr);
            pptr->node->num_procs++;
        }
        /* reset the mapped flags */
        for (v = 0; v < jptr->map->nodes->size; v++) {
            node = (prte_node_t *) pmix_pointer_array_get_item(jptr->map->nodes, v);
            if (NULL != node) {
                PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
            }
        }

        /* Tell our own PMIx server about it, and do not wait: nothing later
         * in this message depends on the registration, and the launch message
         * that could care about it cannot even have been built yet - the
         * master sends this at VM_READY and the launch message several states
         * later.  Registering here rather than at launch time is what lets
         * the launch path stop carrying these jobs at all. */
        rc = prte_pmix_server_register_nspace(jptr, NULL, NULL);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}
