/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "types.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/dss/dss.h"
#include "src/mca/compress/compress.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/runtime/prrte_globals.h"

#include "src/util/nidmap.h"

int prrte_util_nidmap_create(prrte_pointer_array_t *pool,
                            prrte_buffer_t *buffer)
{
    char *raw = NULL;
    uint8_t *vpids=NULL, u8;
    uint16_t u16;
    uint32_t u32;
    int n, ndaemons, rc, nbytes;
    bool compressed;
    char **names = NULL, **ranks = NULL;
    prrte_node_t *nptr;
    prrte_byte_object_t bo, *boptr;
    size_t sz;

    /* pack a flag indicating if the HNP was included in the allocation */
    if (prrte_hnp_is_allocated) {
        u8 = 1;
    } else {
        u8 = 0;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &u8, 1, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* pack a flag indicating if we are in a managed allocation */
    if (prrte_managed_allocation) {
        u8 = 1;
    } else {
        u8 = 0;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &u8, 1, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
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
    if (256 >= pool->size) {
        nbytes = 1;
    } else if (65536 >= pool->size) {
        nbytes = 2;
    } else {
        nbytes = 4;
    }
    vpids = (uint8_t*)malloc(nbytes * pool->size);

    ndaemons = 0;
    for (n=0; n < pool->size; n++) {
        if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(pool, n))) {
            continue;
        }
        /* add the hostname to the argv */
        prrte_argv_append_nosize(&names, nptr->name);
        /* store the vpid */
        if (1 == nbytes) {
            if (NULL == nptr->daemon) {
                vpids[ndaemons] = UINT8_MAX;
            } else {
                vpids[ndaemons] = nptr->daemon->name.vpid;
            }
        } else if (2 == nbytes) {
            if (NULL == nptr->daemon) {
                u16 = UINT16_MAX;
            } else {
                u16 = nptr->daemon->name.vpid;
            }
            memcpy(&vpids[nbytes*ndaemons], &u16, 2);
        } else {
            if (NULL == nptr->daemon) {
                u32 = UINT32_MAX;
            } else {
                u32 = nptr->daemon->name.vpid;
            }
            memcpy(&vpids[nbytes*ndaemons], &u32, 4);
        }
        ++ndaemons;
    }

    /* construct the string of node names for compression */
    raw = prrte_argv_join(names, ',');
    if (prrte_compress.compress_block((uint8_t*)raw, strlen(raw)+1,
                                     (uint8_t**)&bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = (uint8_t*)raw;
        bo.size = strlen(raw)+1;
    }
    /* indicate compression */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &compressed, 1, PRRTE_BOOL))) {
        if (compressed) {
            free(bo.bytes);
        }
        goto cleanup;
    }
    /* if compressed, provide the uncompressed size */
    if (compressed) {
        sz = strlen(raw)+1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &sz, 1, PRRTE_SIZE))) {
            free(bo.bytes);
            goto cleanup;
        }
    }
    /* add the object */
    boptr = &bo;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT))) {
        if (compressed) {
            free(bo.bytes);
        }
        goto cleanup;
    }
    if (compressed) {
        free(bo.bytes);
    }

    /* compress the vpids */
    if (prrte_compress.compress_block(vpids, nbytes*ndaemons,
                                     (uint8_t**)&bo.bytes, &sz)) {
        /* mark that this was compressed */
        compressed = true;
        bo.size = sz;
    } else {
        /* mark that this was not compressed */
        compressed = false;
        bo.bytes = vpids;
        bo.size = nbytes*ndaemons;
    }
    /* indicate compression */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &compressed, 1, PRRTE_BOOL))) {
        if (compressed) {
            free(bo.bytes);
        }
        goto cleanup;
    }
    /* provide the #bytes/vpid */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &nbytes, 1, PRRTE_INT))) {
        if (compressed) {
            free(bo.bytes);
        }
        goto cleanup;
    }
    /* if compressed, provide the uncompressed size */
    if (compressed) {
        sz = nbytes*ndaemons;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &sz, 1, PRRTE_SIZE))) {
            free(bo.bytes);
            goto cleanup;
        }
    }
    /* add the object */
    boptr = &bo;
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT))) {
        if (compressed) {
            free(bo.bytes);
        }
        goto cleanup;
    }
    if (compressed) {
        free(bo.bytes);
    }

  cleanup:
    if (NULL != names) {
        prrte_argv_free(names);
    }
    if (NULL != raw) {
        free(raw);
    }
    if (NULL != ranks) {
        prrte_argv_free(ranks);
    }
    if (NULL != vpids) {
        free(vpids);
    }

    return rc;
}

int prrte_util_decode_nidmap(prrte_buffer_t *buf)
{
    uint8_t u8, *vp8 = NULL;
    uint16_t *vp16 = NULL;
    uint32_t *vp32 = NULL, vpid;
    int cnt, rc, nbytes, n;
    bool compressed;
    size_t sz;
    prrte_byte_object_t *boptr;
    char *raw = NULL, **names = NULL;
    prrte_node_t *nd;
    prrte_job_t *daemons;
    prrte_proc_t *proc;
    prrte_topology_t *t = NULL;

    /* unpack the flag indicating if HNP is in allocation */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &u8, &cnt, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    if (1 == u8) {
        prrte_hnp_is_allocated = true;
    } else {
        prrte_hnp_is_allocated = false;
    }

    /* unpack the flag indicating if we are in managed allocation */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &u8, &cnt, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    if (1 == u8) {
        prrte_managed_allocation = true;
    } else {
        prrte_managed_allocation = false;
    }

    /* unpack compression flag for node names */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &compressed, &cnt, PRRTE_BOOL))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, get the uncompressed size */
    if (compressed) {
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* unpack the nodename object */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, decompress */
    if (compressed) {
        if (!prrte_compress.decompress_block((uint8_t**)&raw, sz,
                                            boptr->bytes, boptr->size)) {
            PRRTE_ERROR_LOG(PRRTE_ERROR);
            if (NULL != boptr->bytes) {
                free(boptr->bytes);
            }
            free(boptr);
            rc = PRRTE_ERROR;
            goto cleanup;
        }
    } else {
        raw = (char*)boptr->bytes;
        boptr->bytes = NULL;
        boptr->size = 0;
    }
    if (NULL != boptr->bytes) {
        free(boptr->bytes);
    }
    free(boptr);
    names = prrte_argv_split(raw, ',');
    free(raw);


    /* unpack compression flag for daemon vpids */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &compressed, &cnt, PRRTE_BOOL))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack the #bytes/vpid */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &nbytes, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, get the uncompressed size */
    if (compressed) {
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* unpack the vpid object */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if compressed, decompress */
    if (compressed) {
        if (!prrte_compress.decompress_block((uint8_t**)&vp8, sz,
                                            boptr->bytes, boptr->size)) {
            PRRTE_ERROR_LOG(PRRTE_ERROR);
            if (NULL != boptr->bytes) {
                free(boptr->bytes);
            }
            free(boptr);
            rc = PRRTE_ERROR;
            goto cleanup;
        }
    } else {
        vp8 = (uint8_t*)boptr->bytes;
        sz = boptr->size;
        boptr->bytes = NULL;
        boptr->size = 0;
    }
    if (NULL != boptr->bytes) {
        free(boptr->bytes);
    }
    free(boptr);
    if (2 == nbytes) {
        vp16 = (uint16_t*)vp8;
        vp8 = NULL;
    } else if (4 == nbytes) {
        vp32 = (uint32_t*)vp8;
        vp8 = NULL;
    }

    /* if we are the HNP, we don't need any of this stuff */
    if (PRRTE_PROC_IS_MASTER) {
        rc = PRRTE_SUCCESS;
        goto cleanup;
    }

    /* get the daemon job object */
    daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);

    /* get our topology */
    for (n=0; n < prrte_node_topologies->size; n++) {
        if (NULL != (t = (prrte_topology_t*)prrte_pointer_array_get_item(prrte_node_topologies, n))) {
            break;
        }
    }
    if (NULL == t) {
        /* should never happen */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        rc = PRRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    /* create the node pool array - this will include
     * _all_ nodes known to the allocation */
    for (n=0; NULL != names[n]; n++) {
        /* add this name to the pool */
        nd = PRRTE_NEW(prrte_node_t);
        nd->name = strdup(names[n]);
        nd->index = n;
        prrte_pointer_array_set_item(prrte_node_pool, n, nd);
        /* see if this is our node */
        if (prrte_check_host_is_local(names[n])) {
            /* add our aliases as an attribute - will include all the interface aliases captured in prrte_init */
            raw = prrte_argv_join(prrte_process_info.aliases, ',');
            prrte_set_attribute(&nd->attributes, PRRTE_NODE_ALIAS, PRRTE_ATTR_LOCAL, raw, PRRTE_STRING);
            free(raw);
        }
        /* set the topology - always default to homogeneous
         * as that is the most common scenario */
        nd->topology = t;
        /* see if it has a daemon on it */
        if (1 == nbytes && UINT8_MAX != vp8[n]) {
            vpid = vp8[n];
        } else if (2 == nbytes && UINT16_MAX != vp16[n]) {
            vpid = vp16[n];
        } else if (4 == nbytes && UINT32_MAX != vp32[n]) {
            vpid = vp32[n];
        } else {
            vpid = UINT32_MAX;
        }
        if (UINT32_MAX != vpid) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(daemons->procs, vpid))) {
                proc = PRRTE_NEW(prrte_proc_t);
                proc->name.jobid = PRRTE_PROC_MY_NAME->jobid;
                proc->name.vpid = vpid;
                proc->state = PRRTE_PROC_STATE_RUNNING;
                PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_ALIVE);
                daemons->num_procs++;
                prrte_pointer_array_set_item(daemons->procs, proc->name.vpid, proc);
            }
            PRRTE_RETAIN(nd);
            proc->node = nd;
            PRRTE_RETAIN(proc);
            nd->daemon = proc;
        }
    }

    /* update num procs */
    if (prrte_process_info.num_procs != daemons->num_procs) {
        prrte_process_info.num_procs = daemons->num_procs;
    }
    /* need to update the routing plan */
    prrte_routed.update_routing_plan();

    if (prrte_process_info.max_procs < prrte_process_info.num_procs) {
        prrte_process_info.max_procs = prrte_process_info.num_procs;
    }

  cleanup:
    if (NULL != vp8) {
        free(vp8);
    }
    if (NULL != vp16) {
        free(vp16);
    }
    if (NULL != vp32) {
        free(vp32);
    }
    if (NULL != names) {
        prrte_argv_free(names);
    }
    return rc;
}

int prrte_util_pass_node_info(prrte_buffer_t *buffer)
{
    uint16_t *slots=NULL, slot = UINT16_MAX;
    uint8_t *flags=NULL, flag = UINT8_MAX;
    int8_t i8, ntopos;
    int rc, n, nbitmap, nstart;
    bool compressed, unislots = true, uniflags = true, unitopos = true;
    prrte_node_t *nptr;
    prrte_byte_object_t bo, *boptr;
    size_t sz, nslots;
    prrte_buffer_t bucket;
    prrte_topology_t *t;

    /* make room for the number of slots on each node */
    nslots = sizeof(uint16_t) * prrte_node_pool->size;
    slots = (uint16_t*)malloc(nslots);
    /* and for the flags for each node - only need one bit/node */
    nbitmap = (prrte_node_pool->size / 8) + 1;
    flags = (uint8_t*)calloc(1, nbitmap);

    /* handle the topologies - as the most common case by far
     * is to have homogeneous topologies, we only send them
     * if something is different. We know that the HNP is
     * the first topology, and that any differing topology
     * on the compute nodes must follow. So send the topologies
     * if and only if:
     *
     * (a) the HNP is being used to house application procs and
     *     there is more than one topology in our array; or
     *
     * (b) the HNP is not being used, but there are more than
     *     two topologies in our array, thus indicating that
     *     there are multiple topologies on the compute nodes
     */
    if (!prrte_hnp_is_allocated || (PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping) & PRRTE_MAPPING_NO_USE_LOCAL)) {
        nstart = 1;
    } else {
        nstart = 0;
    }
    PRRTE_CONSTRUCT(&bucket, prrte_buffer_t);
    ntopos = 0;
    for (n=nstart; n < prrte_node_topologies->size; n++) {
        if (NULL == (t = (prrte_topology_t*)prrte_pointer_array_get_item(prrte_node_topologies, n))) {
            continue;
        }
        /* pack the index */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&bucket, &t->index, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&bucket);
            goto cleanup;
        }
        /* pack this topology string */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&bucket, &t->sig, 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&bucket);
            goto cleanup;
        }
        /* pack the topology itself */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&bucket, &t->topo, 1, PRRTE_HWLOC_TOPO))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&bucket);
            goto cleanup;
        }
        ++ntopos;
    }
    /* pack the number of topologies in allocation */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &ntopos, 1, PRRTE_INT8))) {
        goto cleanup;
    }
    if (1 < ntopos) {
        /* need to send them along */
        if (prrte_compress.compress_block((uint8_t*)bucket.base_ptr, bucket.bytes_used,
                                         &bo.bytes, &sz)) {
            /* the data was compressed - mark that we compressed it */
            compressed = true;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &compressed, 1, PRRTE_BOOL))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&bucket);
                goto cleanup;
            }
            /* pack the uncompressed length */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &bucket.bytes_used, 1, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&bucket);
                goto cleanup;
            }
            bo.size = sz;
        } else {
            /* mark that it was not compressed */
            compressed = false;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &compressed, 1, PRRTE_BOOL))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&bucket);
                goto cleanup;
            }
            prrte_dss.unload(&bucket, (void**)&bo.bytes, &bo.size);
        }
        unitopos = false;
        /* pack the info */
        boptr = &bo;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&bucket);
            goto cleanup;
        }
        PRRTE_DESTRUCT(&bucket);
        free(bo.bytes);
    }

    /* construct the per-node info */
    PRRTE_CONSTRUCT(&bucket, prrte_buffer_t);
    for (n=0; n < prrte_node_pool->size; n++) {
        if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
            continue;
        }
        /* track the topology, if required */
        if (!unitopos) {
            i8 = nptr->topology->index;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&bucket, &i8, 1, PRRTE_INT8))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&bucket);
                goto cleanup;
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
        if (PRRTE_FLAG_TEST(nptr, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
            flags[n/8] |= (1 << (7 - (n % 8)));
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
    if (!unitopos) {
        if (prrte_compress.compress_block((uint8_t*)bucket.base_ptr, bucket.bytes_used,
                                         (uint8_t**)&bo.bytes, &sz)) {
            /* mark that this was compressed */
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            compressed = false;
            bo.bytes = (uint8_t*)bucket.base_ptr;
            bo.size = bucket.bytes_used;
        }
        /* indicate compression */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &compressed, 1, PRRTE_BOOL))) {
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* if compressed, provide the uncompressed size */
        if (compressed) {
            sz = nslots;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &sz, 1, PRRTE_SIZE))) {
                free(bo.bytes);
                goto cleanup;
            }
        }
        /* add the object */
        boptr = &bo;
        rc = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT);
        if (compressed) {
            free(bo.bytes);
        }
    }
    PRRTE_DESTRUCT(&bucket);

    /* if we have uniform #slots, then just flag it - no
     * need to pass anything */
    if (unislots) {
        i8 = -1 * slot;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &i8, 1, PRRTE_INT8))) {
            goto cleanup;
        }
    } else {
        if (prrte_compress.compress_block((uint8_t*)slots, nslots,
                                         (uint8_t**)&bo.bytes, &sz)) {
            /* mark that this was compressed */
            i8 = 1;
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            i8 = 0;
            compressed = false;
            bo.bytes = flags;
            bo.size = nbitmap;
        }
        /* indicate compression */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &i8, 1, PRRTE_INT8))) {
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* if compressed, provide the uncompressed size */
        if (compressed) {
            sz = nslots;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &sz, 1, PRRTE_SIZE))) {
                free(bo.bytes);
                goto cleanup;
            }
        }
        /* add the object */
        boptr = &bo;
        rc = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT);
        if (compressed) {
            free(bo.bytes);
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
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &i8, 1, PRRTE_INT8))) {
            goto cleanup;
        }
    } else {
        if (prrte_compress.compress_block(flags, nbitmap,
                                         (uint8_t**)&bo.bytes, &sz)) {
            /* mark that this was compressed */
            i8 = 2;
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            i8 = 3;
            compressed = false;
            bo.bytes = flags;
            bo.size = nbitmap;
        }
        /* indicate compression */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &i8, 1, PRRTE_INT8))) {
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* if compressed, provide the uncompressed size */
        if (compressed) {
            sz = nbitmap;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &sz, 1, PRRTE_SIZE))) {
                free(bo.bytes);
                goto cleanup;
            }
        }
        /* add the object */
        boptr = &bo;
        rc = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT);
        if (compressed) {
            free(bo.bytes);
        }
    }

  cleanup:
    if (NULL != slots) {
        free(slots);
    }
    if (NULL != flags) {
        free(flags);
    }
    return rc;
}

int prrte_util_parse_node_info(prrte_buffer_t *buf)
{
    int8_t i8;
    bool compressed;
    int rc = PRRTE_SUCCESS, cnt, n, m, index;
    prrte_node_t *nptr;
    size_t sz;
    prrte_byte_object_t *boptr;
    uint16_t *slots = NULL;
    uint8_t *flags = NULL;
    uint8_t *bytes = NULL;
    prrte_topology_t *t2;
    hwloc_topology_t topo;
    char *sig;
    prrte_buffer_t bucket;

    /* check to see if we have uniform topologies */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &i8, &cnt, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    /* we already defaulted to uniform topology, so only need to
     * process this if it is non-uniform */
    if (1 < i8) {
        /* unpack the compression flag */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &compressed, &cnt, PRRTE_BOOL))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        if (compressed) {
            /* get the uncompressed size */
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
        /* unpack the topology object */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }

        /* if compressed, decompress */
        if (compressed) {
            if (!prrte_compress.decompress_block((uint8_t**)&bytes, sz,
                                                boptr->bytes, boptr->size)) {
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                if (NULL != boptr->bytes) {
                    free(boptr->bytes);
                }
                free(boptr);
                rc = PRRTE_ERROR;
                goto cleanup;
            }
        } else {
            bytes = (uint8_t*)boptr->bytes;
            sz = boptr->size;
            boptr->bytes = NULL;
            boptr->size = 0;
        }
        if (NULL != boptr->bytes) {
            free(boptr->bytes);
        }
        /* setup to unpack */
        PRRTE_CONSTRUCT(&bucket, prrte_buffer_t);
        prrte_dss.load(&bucket, bytes, sz);

        for (n=0; n < i8; n++) {
            /* unpack the index */
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(&bucket, &index, &cnt, PRRTE_INT))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* unpack the signature */
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(&bucket, &sig, &cnt, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* unpack the topology */
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(&bucket, &topo, &cnt, PRRTE_HWLOC_TOPO))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* record it */
            t2 = PRRTE_NEW(prrte_topology_t);
            t2->index = index;
            t2->sig = sig;
            t2->topo = topo;
            prrte_pointer_array_set_item(prrte_node_topologies, index, t2);
        }
        PRRTE_DESTRUCT(&bucket);

        /* now get the array of assigned topologies */
        /* unpack the compression flag */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &compressed, &cnt, PRRTE_BOOL))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        if (compressed) {
            /* get the uncompressed size */
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
        /* unpack the topologies object */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* if compressed, decompress */
        if (compressed) {
            if (!prrte_compress.decompress_block((uint8_t**)&bytes, sz,
                                                boptr->bytes, boptr->size)) {
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                if (NULL != boptr->bytes) {
                    free(boptr->bytes);
                }
                free(boptr);
                rc = PRRTE_ERROR;
                goto cleanup;
            }
        } else {
            bytes = (uint8_t*)boptr->bytes;
            sz = boptr->size;
            boptr->bytes = NULL;
            boptr->size = 0;
        }
        if (NULL != boptr->bytes) {
            free(boptr->bytes);
        }
        free(boptr);
        PRRTE_CONSTRUCT(&bucket, prrte_buffer_t);
        prrte_dss.load(&bucket, bytes, sz);
        /* cycle across the node pool and assign the values */
        for (n=0; n < prrte_node_pool->size; n++) {
            if (NULL != (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
                /* unpack the next topology index */
                cnt = 1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(&bucket, &i8, &cnt, PRRTE_INT8))) {
                    PRRTE_ERROR_LOG(rc);
                    goto cleanup;
                }
                nptr->topology = prrte_pointer_array_get_item(prrte_node_topologies, index);
            }
        }
    }

    /* check to see if we have uniform slot assignments */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &i8, &cnt, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if so, then make every node the same */
    if (0 > i8) {
        i8 = -1 * i8;
        for (n=0; n < prrte_node_pool->size; n++) {
            if (NULL != (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
                nptr->slots = i8;
            }
        }
    } else {
        /* if compressed, get the uncompressed size */
        if (1 == i8) {
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
        /* unpack the slots object */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* if compressed, decompress */
        if (1 == i8) {
            if (!prrte_compress.decompress_block((uint8_t**)&slots, sz,
                                                boptr->bytes, boptr->size)) {
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                if (NULL != boptr->bytes) {
                    free(boptr->bytes);
                }
                free(boptr);
                rc = PRRTE_ERROR;
                goto cleanup;
            }
        } else {
            slots = (uint16_t*)boptr->bytes;
            boptr->bytes = NULL;
            boptr->size = 0;
        }
        if (NULL != boptr->bytes) {
            free(boptr->bytes);
        }
        free(boptr);
        /* cycle across the node pool and assign the values */
        for (n=0, m=0; n < prrte_node_pool->size; n++) {
            if (NULL != (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
                nptr->slots = slots[m];
                ++m;
            }
        }
    }

    /* check to see if we have uniform flag assignments */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &i8, &cnt, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if so, then make every node the same */
    if (0 > i8) {
         i8 += 2;
        for (n=0; n < prrte_node_pool->size; n++) {
            if (NULL != (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
                if (i8) {
                    PRRTE_FLAG_SET(nptr, PRRTE_NODE_FLAG_SLOTS_GIVEN);
                } else {
                    PRRTE_FLAG_UNSET(nptr, PRRTE_NODE_FLAG_SLOTS_GIVEN);
                }
            }
        }
    } else {
        /* if compressed, get the uncompressed size */
        if (1 == i8) {
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
        /* unpack the slots object */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* if compressed, decompress */
        if (1 == i8) {
            if (!prrte_compress.decompress_block((uint8_t**)&flags, sz,
                                                boptr->bytes, boptr->size)) {
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                if (NULL != boptr->bytes) {
                    free(boptr->bytes);
                }
                free(boptr);
                rc = PRRTE_ERROR;
                goto cleanup;
            }
        } else {
            flags = (uint8_t*)boptr->bytes;
            boptr->bytes = NULL;
            boptr->size = 0;
        }
        if (NULL != boptr->bytes) {
            free(boptr->bytes);
        }
        free(boptr);
        /* cycle across the node pool and assign the values */
        for (n=0, m=0; n < prrte_node_pool->size; n++) {
            if (NULL != (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
                if (flags[m]) {
                    PRRTE_FLAG_SET(nptr, PRRTE_NODE_FLAG_SLOTS_GIVEN);
                } else {
                    PRRTE_FLAG_UNSET(nptr, PRRTE_NODE_FLAG_SLOTS_GIVEN);
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
    return rc;
}


int prrte_util_generate_ppn(prrte_job_t *jdata,
                           prrte_buffer_t *buf)
{
    uint16_t ppn;
    uint8_t *bytes;
    int32_t nbytes;
    int rc = PRRTE_SUCCESS;
    prrte_app_idx_t i;
    int j, k;
    prrte_byte_object_t bo, *boptr;
    bool compressed;
    prrte_node_t *nptr;
    prrte_proc_t *proc;
    size_t sz;
    prrte_buffer_t bucket;

    PRRTE_CONSTRUCT(&bucket, prrte_buffer_t);

    for (i=0; i < jdata->num_apps; i++) {
        /* for each app_context */
        for (j=0; j < jdata->map->nodes->size; j++) {
            if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, j))) {
                continue;
            }
            if (NULL == nptr->daemon) {
                continue;
            }
            ppn = 0;
            for (k=0; k < nptr->procs->size; k++) {
                if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(nptr->procs, k))) {
                    if (proc->name.jobid == jdata->jobid) {
                        ++ppn;
                    }
                }
            }
            if (0 < ppn) {
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&bucket, &nptr->index, 1, PRRTE_STD_CNTR))) {
                    goto cleanup;
                }
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&bucket, &ppn, 1, PRRTE_UINT16))) {
                    goto cleanup;
                }
            }
        }
        prrte_dss.unload(&bucket, (void**)&bytes, &nbytes);

        if (prrte_compress.compress_block(bytes, (size_t)nbytes,
                                         (uint8_t**)&bo.bytes, &sz)) {
            /* mark that this was compressed */
            compressed = true;
            bo.size = sz;
        } else {
            /* mark that this was not compressed */
            compressed = false;
            bo.bytes = bytes;
            bo.size = nbytes;
        }
        /* indicate compression */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &compressed, 1, PRRTE_BOOL))) {
            if (compressed) {
                free(bo.bytes);
            }
            goto cleanup;
        }
        /* if compressed, provide the uncompressed size */
        if (compressed) {
            sz = nbytes;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &sz, 1, PRRTE_SIZE))) {
                free(bo.bytes);
                goto cleanup;
            }
        }
        /* add the object */
        boptr = &bo;
        rc = prrte_dss.pack(buf, &boptr, 1, PRRTE_BYTE_OBJECT);
        if (PRRTE_SUCCESS != rc) {
            break;
        }
    }

  cleanup:
    PRRTE_DESTRUCT(&bucket);
    return rc;
}

int prrte_util_decode_ppn(prrte_job_t *jdata,
                         prrte_buffer_t *buf)
{
    prrte_std_cntr_t index;
    prrte_app_idx_t n;
    int cnt, rc=PRRTE_SUCCESS, m;
    prrte_byte_object_t *boptr;
    bool compressed;
    uint8_t *bytes;
    size_t sz;
    uint16_t ppn, k;
    prrte_node_t *node;
    prrte_proc_t *proc;
    prrte_buffer_t bucket;

    /* reset any flags */
    for (m=0; m < prrte_node_pool->size; m++) {
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, m))) {
            PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
        }
    }

    for (n=0; n < jdata->num_apps; n++) {
        /* unpack the compression flag */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &compressed, &cnt, PRRTE_BOOL))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* if compressed, unpack the raw size */
        if (compressed) {
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &sz, &cnt, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
        /* unpack the byte object describing this app */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buf, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        if (PRRTE_PROC_IS_MASTER) {
            /* just discard it */
            free(boptr->bytes);
            free(boptr);
            continue;
        }

        /* decompress if required */
        if (compressed) {
            if (!prrte_compress.decompress_block(&bytes, sz,
                                                boptr->bytes, boptr->size)) {
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                PRRTE_RELEASE(boptr);
                return PRRTE_ERROR;
            }
        } else {
            bytes = boptr->bytes;
            sz = boptr->size;
            boptr->bytes = NULL;
            boptr->size = 0;
        }
        if (NULL != boptr->bytes) {
            free(boptr->bytes);
        }
        free(boptr);

        /* setup to unpack */
        PRRTE_CONSTRUCT(&bucket, prrte_buffer_t);
        prrte_dss.load(&bucket, bytes, sz);

        /* unpack each node and its ppn */
        cnt = 1;
        while (PRRTE_SUCCESS == (rc = prrte_dss.unpack(&bucket, &index, &cnt, PRRTE_STD_CNTR))) {
            /* get the corresponding node object */
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, index))) {
                rc = PRRTE_ERR_NOT_FOUND;
                PRRTE_ERROR_LOG(rc);
                goto error;
            }
            /* add the node to the job map if not already assigned */
            if (!PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_MAPPED)) {
                PRRTE_RETAIN(node);
                prrte_pointer_array_add(jdata->map->nodes, node);
                PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_MAPPED);
            }
            /* get the ppn */
            cnt = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(&bucket, &ppn, &cnt, PRRTE_UINT16))) {
                PRRTE_ERROR_LOG(rc);
                goto error;
            }
            /* create a proc object for each one */
            for (k=0; k < ppn; k++) {
                proc = PRRTE_NEW(prrte_proc_t);
                proc->name.jobid = jdata->jobid;
                /* leave the vpid undefined as this will be determined
                 * later when we do the overall ranking */
                proc->app_idx = n;
                proc->parent = node->daemon->name.vpid;
                PRRTE_RETAIN(node);
                proc->node = node;
                /* flag the proc as ready for launch */
                proc->state = PRRTE_PROC_STATE_INIT;
                prrte_pointer_array_add(node->procs, proc);
                node->num_procs++;
                /* we will add the proc to the jdata array when we
                 * compute its rank */
            }
            node->num_procs += ppn;
            cnt = 1;
        }
        PRRTE_DESTRUCT(&bucket);
    }
    if (PRRTE_SUCCESS != rc && PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PRRTE_ERROR_LOG(rc);
    }

    /* reset any flags */
    for (m=0; m < jdata->map->nodes->size; m++) {
        node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m);
        if (NULL != node) {
            PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
        }
    }
    return PRRTE_SUCCESS;

  error:
    PRRTE_DESTRUCT(&bucket);
    /* reset any flags */
    for (m=0; m < jdata->map->nodes->size; m++) {
        node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m);
        if (NULL != node) {
            PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
        }
    }
    return rc;
}
