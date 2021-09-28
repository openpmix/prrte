/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2020      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

#include "src/util/nidmap.h"

int prte_util_nidmap_create(prte_pointer_array_t *pool, pmix_data_buffer_t *buffer)
{
    char *raw = NULL;
    pmix_rank_t *vpids = NULL;
    uint8_t u8;
    int n, m, ndaemons, nbytes;
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

    /* pack a flag indicating if we are in a managed allocation */
    if (prte_managed_allocation) {
        u8 = 1;
    } else {
        u8 = 0;
    }
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &u8, 1, PMIX_UINT8);
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
     * them all. Someone can optimize this further later */
    nbytes = prte_process_info.num_daemons * sizeof(pmix_rank_t);
    vpids = (pmix_rank_t *) malloc(nbytes);

    ndaemons = 0;
    for (n = 0; n < pool->size; n++) {
        if (NULL == (nptr = (prte_node_t *) prte_pointer_array_get_item(pool, n))) {
            continue;
        }
        /* add the hostname to the argv */
        prte_argv_append_nosize(&names, nptr->name);
        als = NULL;
        if (NULL != nptr->aliases) {
            for (m=0; NULL != nptr->aliases[m]; m++) {
                prte_argv_append_nosize(&als, nptr->aliases[m]);
            }
            raw = prte_argv_join(als, ',');
            prte_argv_free(als);
            prte_argv_append_nosize(&aliases, raw);
            free(raw);
        } else {
            prte_argv_append_nosize(&aliases, "PRTENONE");
        }
        /* store the vpid */
        if (NULL == nptr->daemon) {
            vpids[ndaemons] = PMIX_RANK_INVALID;
        } else {
            vpids[ndaemons] = nptr->daemon->name.rank;
        }
        ++ndaemons;
    }

    /* little protection */
    if (NULL == names || NULL == aliases) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        free(vpids);
        return PRTE_ERR_NOT_FOUND;
    }

    /* construct the string of node names for compression */
    raw = prte_argv_join(names, ',');
    prte_argv_free(names);
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
        return rc;
    }
    /* add the object */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(vpids);
        return rc;
    }
    free(bo.bytes);

    /* construct the string of aliases for compression */
    raw = prte_argv_join(aliases, ';');
    prte_argv_free(aliases);
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
        return rc;
    }
    /* add the object */
    rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(bo.bytes);
        free(vpids);
        return rc;
    }
    free(bo.bytes);

    /* compress the vpids */
    if (PMIx_Data_compress((uint8_t *) vpids, nbytes, (uint8_t **) &bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
        free(vpids);
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = (char *) vpids;
        bo.size = nbytes * ndaemons;
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
    int cnt, n;
    bool compressed;
    size_t sz;
    pmix_byte_object_t pbo;
    char *raw = NULL, **names = NULL, **aliases = NULL;
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

    /* unpack the flag indicating if we are in managed allocation */
    cnt = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buf, &u8, &cnt, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (1 == u8) {
        prte_managed_allocation = true;
    } else {
        prte_managed_allocation = false;
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
    names = prte_argv_split(raw, ',');
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
    aliases = prte_argv_split(raw, ';');
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

    /* if we are the HNP, we don't need any of this stuff */
    if (PRTE_PROC_IS_MASTER) {
        rc = PRTE_SUCCESS;
        goto cleanup;
    }

    /* get the daemon job object */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* get our topology */
    t = (prte_topology_t *) prte_pointer_array_get_item(prte_node_topologies, 0);
    if (NULL == t) {
        /* should never happen */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    /* create the node pool array - this will include
     * _all_ nodes known to the allocation */
    for (n = 0; NULL != names[n]; n++) {
        /* do we already have this node? */
        nd = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, n);
        if (NULL != nd) {
            /* check the name */
            if (0 != strcmp(nd->name, names[n])) {
                free(nd->name);
                nd->name = strdup(names[n]);
            }
            if (0 != strcmp(aliases[n], "PRTENONE")) {
                if (NULL != nd->aliases) {
                    prte_argv_free(nd->aliases);
                }
                nd->aliases = prte_argv_split(aliases[n], ',');
            }
            continue;
        }
        /* add this name to the pool */
        nd = PRTE_NEW(prte_node_t);
        nd->name = strdup(names[n]);
        nd->index = n;
        prte_pointer_array_set_item(prte_node_pool, n, nd);
        /* add any aliases */
        if (0 != strcmp(aliases[n], "PRTENONE")) {
            nd->aliases = prte_argv_split(aliases[n], ',');
        }
        /* set the topology - always default to homogeneous
         * as that is the most common scenario */
        nd->topology = t;
        /* see if it has a daemon on it */
        if (PMIX_RANK_INVALID != vpid[n]) {
            proc = (prte_proc_t *) prte_pointer_array_get_item(daemons->procs, vpid[n]);
            if (NULL == proc) {
                proc = PRTE_NEW(prte_proc_t);
                PMIX_LOAD_PROCID(&proc->name, PRTE_PROC_MY_NAME->nspace, vpid[n]);
                proc->state = PRTE_PROC_STATE_RUNNING;
                PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_ALIVE);
                daemons->num_procs++;
                prte_pointer_array_set_item(daemons->procs, proc->name.rank, proc);
            }
            PRTE_RETAIN(nd);
            proc->node = nd;
            PRTE_RETAIN(proc);
            nd->daemon = proc;
        }
    }

    /* update num procs */
    if (prte_process_info.num_daemons != daemons->num_procs) {
        prte_process_info.num_daemons = daemons->num_procs;
    }
    /* need to update the routing plan */
    prte_routed.update_routing_plan();

cleanup:
    if (NULL != vpid) {
        free(vpid);
    }
    if (NULL != names) {
        prte_argv_free(names);
    }
    return rc;
}

int prte_util_pass_node_info(pmix_data_buffer_t *buffer)
{
    uint16_t *slots = NULL, slot = UINT16_MAX;
    uint8_t *flags = NULL, flag = UINT8_MAX;
    int8_t i8;
    int16_t i16;
    int32_t ntopos;
    int rc, m, n, nbitmap;
    bool compressed, unislots = true, uniflags = true;
    prte_node_t *nptr;
    pmix_byte_object_t bo;
    size_t sz, nslots;
    pmix_data_buffer_t bucket;
    prte_topology_t *t;
    pmix_topology_t pt;
    char **topos = NULL;

    /* make room for the number of slots on each node */
    nslots = sizeof(uint16_t) * prte_node_pool->size;
    slots = (uint16_t *) malloc(nslots);
    /* and for the flags for each node - only need one bit/node */
    nbitmap = (prte_node_pool->size / 8) + 1;
    flags = (uint8_t *) calloc(1, nbitmap);

    /* indicate if we have hetero nodes */
    if (prte_hetero_nodes) {
        i8 = 1;
    } else {
        i8 = 0;
    }
    rc = PMIx_Data_pack(NULL, buffer, &i8, 1, PMIX_INT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* we only need to send topologies if we have hetero nodes */
    if (prte_hetero_nodes) {
        PMIX_DATA_BUFFER_CONSTRUCT(&bucket);
        pt.source = strdup("hwloc");
        ntopos = 0;
        for (n = 0; n < prte_node_topologies->size; n++) {
            t = (prte_topology_t *) prte_pointer_array_get_item(prte_node_topologies, n);
            if (NULL == t) {
                continue;
            }
            /* pack the topology string */
            rc = PMIx_Data_pack(NULL, &bucket, &t->sig, 1, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                free(pt.source);
                goto cleanup;
            }
            /* track it */
            prte_argv_append_nosize(&topos, t->sig);
            /* pack the topology itself */
            pt.topology = t->topo;
            rc = PMIx_Data_pack(NULL, &bucket, &pt, 1, PMIX_TOPO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                free(pt.source);
                goto cleanup;
            }
            ++ntopos;
        }
        free(pt.source);
        /* pack the number of topologies */
        rc = PMIx_Data_pack(NULL, buffer, &ntopos, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* send them along */
        if (PMIx_Data_compress((uint8_t *) bucket.base_ptr, bucket.bytes_used,
                               (uint8_t **) &bo.bytes, &sz)) {
            /* the data was compressed - mark that we compressed it */
            compressed = true;
            rc = PMIx_Data_pack(NULL, buffer, &compressed, 1, PMIX_BOOL);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
            bo.size = sz;
        } else {
            /* mark that it was not compressed */
            compressed = false;
            rc = PMIx_Data_pack(NULL, buffer, &compressed, 1, PMIX_BOOL);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
            rc = PMIx_Data_unload(&bucket, &bo);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
        }
        /* pack the info */
        rc = PMIx_Data_pack(NULL, buffer, &bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&bucket);
            goto cleanup;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&bucket);
        free(bo.bytes);
    }

    /* construct the per-node info */
    PMIX_DATA_BUFFER_CONSTRUCT(&bucket);
    for (n = 0; n < prte_node_pool->size; n++) {
        if (NULL == (nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, n))) {
            continue;
        }
        /* track the topology, if required */
        if (prte_hetero_nodes && NULL != nptr->daemon) {
            rc = PMIx_Data_pack(NULL, &bucket, &nptr->daemon->name.rank, 1, PMIX_PROC_RANK);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
            /* find this signature in the topos */
            for (m = 0; NULL != topos && NULL != topos[m]; m++) {
                if (0 == strcmp(topos[m], nptr->topology->sig)) {
                    rc = PMIx_Data_pack(NULL, &bucket, &m, 1, PMIX_INT);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                        goto cleanup;
                    }
                    break;
                }
            }
        }
        /* store the number of slots */
        slots[n] = nptr->slots;
        if (UINT16_MAX == slot) {
            slot = nptr->slots;
        } else if (slot != nptr->slots) {
            unislots = false;
        }
        /* store the flag */
        if (PRTE_FLAG_TEST(nptr, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
            flags[n / 8] |= (1 << (7 - (n % 8)));
            if (UINT8_MAX == flag) {
                flag = 1;
            } else if (1 != flag) {
                uniflags = false;
            }
        } else {
            if (UINT8_MAX == flag) {
                flag = 0;
            } else if (0 != flag) {
                uniflags = false;
            }
        }
    }

    /* deal with the topology assignments */
    if (prte_hetero_nodes) {
        if (PMIx_Data_compress((uint8_t *) bucket.base_ptr, bucket.bytes_used,
                               (uint8_t **) &bo.bytes, &sz)) {
            /* mark that this was compressed */
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            compressed = false;
            bo.bytes = bucket.base_ptr;
            bo.size = bucket.bytes_used;
        }
        /* indicate compression */
        rc = PMIx_Data_pack(NULL, buffer, &compressed, 1, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&bucket);
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* add the object */
        rc = PMIx_Data_pack(NULL, buffer, &bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&bucket);
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
    }
    PMIX_DATA_BUFFER_DESTRUCT(&bucket);

    /* if we have uniform #slots, then just flag it - no
     * need to pass anything */
    if (unislots) {
        i16 = -1 * slot;
        rc = PMIx_Data_pack(NULL, buffer, &i16, 1, PMIX_INT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    } else {
        if (PMIx_Data_compress((uint8_t *) slots, nslots, (uint8_t **) &bo.bytes, &sz)) {
            /* mark that this was compressed */
            i16 = 1;
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            i16 = 0;
            compressed = false;
            bo.bytes = (char *) slots;
            bo.size = nslots;
        }
        /* indicate compression */
        rc = PMIx_Data_pack(NULL, buffer, &i16, 1, PMIX_INT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* add the object */
        rc = PMIx_Data_pack(NULL, buffer, &bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
    }

    /* if we have uniform flags, then just flag it - no
     * need to pass anything */
    if (uniflags) {
        if (1 == flag) {
            i8 = -1;
        } else {
            i8 = -2;
        }
        rc = PMIx_Data_pack(NULL, buffer, &i8, 1, PMIX_INT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    } else {
        if (PMIx_Data_compress(flags, nbitmap, (uint8_t **) &bo.bytes, &sz)) {
            /* mark that this was compressed */
            i8 = 2;
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            i8 = 3;
            compressed = false;
            bo.bytes = (char *) flags;
            bo.size = nbitmap;
        }
        /* indicate compression */
        rc = PMIx_Data_pack(NULL, buffer, &i8, 1, PMIX_INT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* add the object */
        rc = PMIx_Data_pack(NULL, buffer, &bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
    }

cleanup:
    if (NULL != slots) {
        free(slots);
    }
    if (NULL != flags) {
        free(flags);
    }
    if (NULL != topos) {
        prte_argv_free(topos);
    }
    return rc;
}

int prte_util_parse_node_info(pmix_data_buffer_t *buf)
{
    int8_t i8;
    int16_t i16;
    int32_t ntopos;
    bool compressed, found;
    int rc = PRTE_SUCCESS, cnt, n, m;
    prte_node_t *nptr;
    size_t sz;
    pmix_byte_object_t pbo;
    uint16_t *slots = NULL;
    uint8_t *flags = NULL;
    uint8_t *bytes = NULL;
    prte_topology_t *t2, *t3;
    pmix_topology_t ptopo;
    hwloc_topology_t topo;
    char *sig;
    pmix_data_buffer_t bucket;
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *sum;
    char **topos = NULL;
    pmix_rank_t drk;

    /* check to see if we have uniform topologies */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buf, &i8, &cnt, PMIX_INT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* we already defaulted to uniform topology, so only need to
     * process this if it is non-uniform */
    if (0 != i8) {
        prte_hetero_nodes = true;
        /* get the number of topologies */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &ntopos, &cnt, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the compression flag */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &compressed, &cnt, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the topology object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }

        /* if compressed, decompress */
        if (compressed) {
            if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &bytes, &sz)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                rc = PRTE_ERROR;
                goto cleanup;
            }
        } else {
            bytes = (uint8_t *) pbo.bytes;
            sz = pbo.size;
            pbo.bytes = NULL;
            pbo.size = 0;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo); // release pre-existing data
        PMIX_BYTE_OBJECT_LOAD(&pbo, bytes, sz);

        /* setup to unpack */
        PMIX_DATA_BUFFER_CONSTRUCT(&bucket);
        rc = PMIx_Data_load(&bucket, &pbo);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

        for (n = 0; n < ntopos; n++) {
            /* unpack the signature */
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &bucket, &sig, &cnt, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto cleanup;
            }
            /* cache it */
            prte_argv_append_nosize(&topos, sig);
            /* unpack the topology */
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &bucket, &ptopo, &cnt, PMIX_TOPO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto cleanup;
            }
            topo = ptopo.topology;
            ptopo.topology = NULL;
            PMIX_TOPOLOGY_DESTRUCT(&ptopo);
            /* see if we already have it - there aren't many topologies
             * in a cluster, so this won't take long */
            found = false;
            for (m = 0; m < prte_node_topologies->size; m++) {
                t3 = (prte_topology_t *) prte_pointer_array_get_item(prte_node_topologies, m);
                if (NULL == t3) {
                    continue;
                }
                if (0 == strcmp(sig, t3->sig)) {
                    found = true;
                    break;
                }
            }
            if (found) {
                hwloc_topology_destroy(topo);
                free(sig);
            } else {
                /* record it */
                t2 = PRTE_NEW(prte_topology_t);
                t2->sig = sig;
                t2->topo = topo;
                /* need to ensure the summary is setup */
                root = hwloc_get_root_obj(topo);
                root->userdata = (void *) PRTE_NEW(prte_hwloc_topo_data_t);
                sum = (prte_hwloc_topo_data_t *) root->userdata;
                sum->available = prte_hwloc_base_setup_summary(topo);
                t2->index = prte_pointer_array_add(prte_node_topologies, t2);
            }
        }
        PMIX_DATA_BUFFER_DESTRUCT(&bucket);

        /* now get the array of assigned topologies */
        /* unpack the compression flag */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &compressed, &cnt, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the topologies object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* if compressed, decompress */
        if (compressed) {
            if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &bytes, &sz)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                rc = PRTE_ERROR;
                goto cleanup;
            }
        } else {
            bytes = (uint8_t *) pbo.bytes;
            sz = pbo.size;
            pbo.bytes = NULL;
            pbo.size = 0;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo); // release pre-existing data
        PMIX_BYTE_OBJECT_LOAD(&pbo, bytes, sz);

        /* setup to unpack */
        PMIX_DATA_BUFFER_CONSTRUCT(&bucket);
        rc = PMIx_Data_load(&bucket, &pbo);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo); // release pre-existing data

        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &bucket, &drk, &cnt, PMIX_PROC_RANK);
        while (PMIX_SUCCESS == rc) {
            nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, drk);
            if (NULL == nptr) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &bucket, &m, &cnt, PMIX_INT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
            /* the topology signature we want is in that location in
             * the topos argv array */
            sig = topos[m];
            /* find that signature in our topologies - might be at a
             * different location */
            for (m = 0; m < prte_node_topologies->size; m++) {
                t3 = (prte_topology_t *) prte_pointer_array_get_item(prte_node_topologies, m);
                if (NULL == t3) {
                    continue;
                }
                if (0 == strcmp(sig, t3->sig)) {
                    nptr->topology = t3;
                    break;
                }
            }
            /* unpack the next daemon rank */
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &bucket, &drk, &cnt, PMIX_PROC_RANK);
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* check to see if we have uniform slot assignments */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buf, &i16, &cnt, PMIX_INT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if so, then make every node the same */
    if (0 > i16) {
        i16 = -1 * i16;
        for (n = 0; n < prte_node_pool->size; n++) {
            if (NULL != (nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, n))) {
                nptr->slots = i16;
            }
        }
    } else {
        /* unpack the slots object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* if compressed, decompress */
        if (1 == i16) {
            if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &slots, &sz)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                rc = PRTE_ERROR;
                goto cleanup;
            }
        } else {
            slots = (uint16_t *) pbo.bytes;
            pbo.bytes = NULL;
            pbo.size = 0;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

        /* cycle across the node pool and assign the values */
        for (n = 0, m = 0; n < prte_node_pool->size; n++) {
            if (NULL != (nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, n))) {
                nptr->slots = slots[m];
                ++m;
            }
        }
    }

    /* check to see if we have uniform flag assignments */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buf, &i8, &cnt, PMIX_INT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if so, then make every node the same */
    if (0 > i8) {
        i8 += 2;
        for (n = 0; n < prte_node_pool->size; n++) {
            if (NULL != (nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, n))) {
                if (i8) {
                    PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_SLOTS_GIVEN);
                } else {
                    PRTE_FLAG_UNSET(nptr, PRTE_NODE_FLAG_SLOTS_GIVEN);
                }
            }
        }
    } else {
        /* unpack the slots object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &pbo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* if compressed, decompress */
        if (2 == i8) {
            if (!PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size, (uint8_t **) &flags, &sz)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                rc = PRTE_ERROR;
                goto cleanup;
            }
        } else {
            flags = (uint8_t *) pbo.bytes;
            pbo.bytes = NULL;
            pbo.size = 0;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

        /* cycle across the node pool and assign the values */
        for (n = 0, m = 0; n < prte_node_pool->size; n++) {
            if (NULL != (nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, n))) {
                if (flags[m]) {
                    PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_SLOTS_GIVEN);
                } else {
                    PRTE_FLAG_UNSET(nptr, PRTE_NODE_FLAG_SLOTS_GIVEN);
                }
                ++m;
            }
        }
    }

cleanup:
    if (NULL != slots) {
        free(slots);
    }
    if (NULL != flags) {
        free(flags);
    }
    if (NULL != topos) {
        prte_argv_free(topos);
    }
    return rc;
}

int prte_util_generate_ppn(prte_job_t *jdata, pmix_data_buffer_t *buf)
{
    uint16_t ppn;
    int rc = PRTE_SUCCESS;
    prte_app_idx_t i;
    int j, k;
    pmix_byte_object_t bo;
    bool compressed;
    prte_node_t *nptr;
    prte_proc_t *proc;
    size_t sz;
    pmix_data_buffer_t bucket;
    prte_app_context_t *app;

    for (i = 0; i < jdata->num_apps; i++) {
        PMIX_DATA_BUFFER_CONSTRUCT(&bucket);
        /* for each app_context */
        if (NULL != (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            for (j = 0; j < jdata->map->num_nodes; j++) {
                if (NULL
                    == (nptr = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, j))) {
                    continue;
                }
                if (NULL == nptr->daemon) {
                    continue;
                }
                ppn = 0;
                for (k = 0; k < nptr->procs->size; k++) {
                    if (NULL
                        != (proc = (prte_proc_t *) prte_pointer_array_get_item(nptr->procs, k))) {
                        if (PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)
                            && proc->app_idx == app->idx) {
                            ++ppn;
                        }
                    }
                }
                if (0 < ppn) {
                    rc = PMIx_Data_pack(NULL, &bucket, &nptr->index, 1, PMIX_INT32);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                        goto cleanup;
                    }
                    rc = PMIx_Data_pack(NULL, &bucket, &ppn, 1, PMIX_UINT16);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                        goto cleanup;
                    }
                }
            }
        }

        if (PMIx_Data_compress((uint8_t *) bucket.base_ptr, bucket.bytes_used,
                               (uint8_t **) &bo.bytes, &sz)) {
            /* mark that this was compressed */
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            compressed = false;
            rc = PMIx_Data_unload(&bucket, &bo);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&bucket);
                goto cleanup;
            }
        }
        PMIX_DATA_BUFFER_DESTRUCT(&bucket);
        /* indicate compression */
        rc = PMIx_Data_pack(NULL, buf, &compressed, 1, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (compressed) {
                PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            }
            goto cleanup;
        }
        /* add the object */
        rc = PMIx_Data_pack(NULL, buf, &bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            break;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    }

cleanup:
    return rc;
}

int prte_util_decode_ppn(prte_job_t *jdata, pmix_data_buffer_t *buf)
{
    int32_t index;
    prte_app_idx_t n;
    int cnt, rc = PRTE_SUCCESS, m;
    pmix_byte_object_t bo;
    bool compressed;
    uint8_t *bytes;
    size_t sz;
    uint16_t ppn, k;
    prte_node_t *node;
    prte_proc_t *proc;
    pmix_data_buffer_t bucket;

    /* reset any flags */
    for (m = 0; m < jdata->map->nodes->size; m++) {
        if (NULL != (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }

    for (n = 0; n < jdata->num_apps; n++) {
        /* unpack the compression flag */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &compressed, &cnt, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the byte object describing this app */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }

        if (PRTE_PROC_IS_MASTER) {
            /* just discard it */
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            continue;
        }

        /* decompress if required */
        if (compressed) {
            if (!PMIx_Data_decompress((uint8_t *) bo.bytes, bo.size, &bytes, &sz)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
                PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                return PRTE_ERROR;
            }
        } else {
            bytes = (uint8_t *) bo.bytes;
            sz = bo.size;
            bo.bytes = NULL;
            bo.size = 0;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&bo); // release pre-existing data
        PMIX_BYTE_OBJECT_LOAD(&bo, bytes, sz);

        /* setup to unpack */
        PMIX_DATA_BUFFER_CONSTRUCT(&bucket);
        rc = PMIx_Data_load(&bucket, &bo);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);

        /* unpack each node and its ppn */
        cnt = 1;
        while (PMIX_SUCCESS == (rc = PMIx_Data_unpack(NULL, &bucket, &index, &cnt, PMIX_INT32))) {
            /* get the corresponding node object */
            if (NULL
                == (node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, index))) {
                rc = PRTE_ERR_NOT_FOUND;
                PRTE_ERROR_LOG(rc);
                goto error;
            }
            /* add the node to the job map if not already assigned */
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_RETAIN(node);
                prte_pointer_array_add(jdata->map->nodes, node);
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
            }
            /* get the ppn */
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &bucket, &ppn, &cnt, PMIX_UINT16);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto error;
            }
            /* create a proc object for each one */
            for (k = 0; k < ppn; k++) {
                proc = PRTE_NEW(prte_proc_t);
                PMIX_LOAD_NSPACE(proc->name.nspace, jdata->nspace);
                /* leave the vpid undefined as this will be determined
                 * later when we do the overall ranking */
                proc->app_idx = n;
                proc->parent = node->daemon->name.rank;
                PRTE_RETAIN(node);
                proc->node = node;
                /* flag the proc as ready for launch */
                proc->state = PRTE_PROC_STATE_INIT;
                prte_pointer_array_add(node->procs, proc);
                node->num_procs++;
                /* we will add the proc to the jdata array when we
                 * compute its rank */
            }
            cnt = 1;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&bucket);
    }
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }

    /* reset any flags */
    for (m = 0; m < jdata->map->nodes->size; m++) {
        node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m);
        if (NULL != node) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }
    return PRTE_SUCCESS;

error:
    PMIX_DATA_BUFFER_DESTRUCT(&bucket);
    /* reset any flags */
    for (m = 0; m < jdata->map->nodes->size; m++) {
        node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m);
        if (NULL != node) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }
    return rc;
}
