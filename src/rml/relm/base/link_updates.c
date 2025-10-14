/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/rml/rml.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/types.h"
#include "src/rml/relm/util.h"
#include "src/rml/relm/base/state_machine.h"

// Convert an index in the state machine's bitmaps to the link's rank
static pmix_rank_t link_i_to_r(const int index);
// Convert a link's rank to its index in the state machine's bitmaps
static int link_r_to_i(const pmix_rank_t rank);

// Send any link updates that we are able to. We cannot send an update to a
// given link unless we have updated upstreams on all other links. We don't need
// to send a link update unless that link's downstream update is pending.
static void try_send_pending_link_updates(void);

// Remove any messages to or from a failed rank, or messages that
// we are no longer on the direct path of
static void purge(const prte_rml_recovery_status_t* status);

static int prte_relm_base_pack_link_dst_updates(
    pmix_data_buffer_t* buf, prte_relm_rank_t* rank
){
    int ret = PMIX_SUCCESS;
    prte_relm_guid_t guid;
    prte_relm_msg_t* msg;
    PMIX_HASH_TABLE_FOREACH(guid, uint64, msg, &rank->msgs){
        if(PRTE_RELM_STATE_INVALID == msg->state) continue;

        // We wouldn't expect to see a sending state here, but just in case.
        // The sending message will go first, and we don't want to double up
        bool sending_to_sent = PRTE_RELM_STATE_SENDING == msg->state;
        if(sending_to_sent) msg->state = PRTE_RELM_STATE_SENT;

        ret = prte_relm_sm->pack_state_update(buf, msg);

        if(sending_to_sent) msg->state = PRTE_RELM_STATE_SENDING;

        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            break;
        }
    }
    return ret;
}

int prte_relm_base_pack_link_update(
    pmix_data_buffer_t* buf, pmix_rank_t link
) {
    // Pack my current depth, so receiver can ignore lingering updates from
    // before a promotion
    int ret = PMIx_Data_pack(
        NULL, buf, &prte_rml_base.cur_node.depth, 1, PMIX_PROC_RANK
    );
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return ret;
    }

    pmix_rank_t dst;
    prte_relm_rank_t* rank;
    PMIX_HASH_TABLE_FOREACH(dst, uint32, rank, &prte_relm_sm->ranks){
        // Pack each message that sends through this link
        if(link == prte_rml_get_route(dst)){
            ret = prte_relm_base_pack_link_dst_updates(buf, rank);
            if(PMIX_SUCCESS != ret) break;
        }
    }

    return ret;
}

void prte_relm_base_update_link(
  pmix_data_buffer_t* buf, pmix_rank_t link
) {
    // Ignore lingering updates from old links
    if(link != prte_rml_get_route(link)) return;

    // Check the depth being reported by the sender
    int count = 1;
    pmix_rank_t link_depth;
    int ret = PMIx_Data_unpack(NULL, buf, &link_depth, &count, PMIX_PROC_RANK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return;
    }
    // Ignore lingering updates from a link was also promoted up when I was
    pmix_rank_t expected_depth = prte_rml_base.cur_node.depth +
        ( link == prte_rml_base.lifeline ? -1 : 1 );
    if(link_depth != expected_depth) return;

    // Just handle each state update as individual messages
    char* prev = NULL, *end = buf->base_ptr+buf->bytes_used;
    while(buf->unpack_ptr != prev && (prev = buf->unpack_ptr) != end){
        prte_relm_message_handler(link, buf);
    }

    // Set upstream to updated
    pmix_bitmap_t* up_bits = &prte_relm_sm->upstream_links_updated;
    pmix_bitmap_set_bit(up_bits, link_r_to_i(link));

    // Check if we can now send any pending downstream updates
    try_send_pending_link_updates();
}

void prte_relm_base_fault_handler(const prte_rml_recovery_status_t* status){
    if(status->scope != PRTE_RML_FAULT_SCOPE_LOCAL) return;

    purge(status);

    pmix_bitmap_t* upstream_updated = &prte_relm_sm->upstream_links_updated;
    pmix_bitmap_t* downstream_updated = &prte_relm_sm->downstream_links_updated;

    const pmix_rank_t* prev_chld = (pmix_rank_t*)status->prev_children.array;
    const int radix = prte_rml_base.radix;

    for(int idx = 0; idx < radix+1; idx++){
        const pmix_rank_t rank = link_i_to_r(idx);
        if(rank == PMIX_RANK_INVALID){
            // No information to exchange with invalid links
            pmix_bitmap_set_bit(upstream_updated, idx);
            pmix_bitmap_set_bit(downstream_updated, idx);
        } else if(status->promoted){
            // If promoted, all valid links need full exchanges
            pmix_bitmap_clear_bit(upstream_updated, idx);
            pmix_bitmap_clear_bit(downstream_updated, idx);
        } else if(!pmix_bitmap_is_set_bit(downstream_updated, idx)){
            // Haven't received needed data since last promotion, skip for now
        } else if(rank != (idx < radix ? prev_chld[idx] : status->prev_parent)){
            // Link changed, send a downstream update to them
            prte_relm_send_link_update(rank);
        }
    }

    // See if we can send any pending updates now
    try_send_pending_link_updates();
}

static pmix_rank_t link_i_to_r(const int index){
    if(index == prte_rml_base.radix) return prte_rml_base.lifeline;
    if(index >= (int)prte_rml_base.children.size) return PMIX_RANK_INVALID;
    if(index < 0) return PMIX_RANK_INVALID;
    return ((pmix_rank_t*)prte_rml_base.children.array)[index];
}
static int link_r_to_i(const pmix_rank_t rank){
    if(rank == prte_rml_base.lifeline) return prte_rml_base.radix;
    int i = prte_rml_get_subtree_index(rank);
    return rank == link_i_to_r(i) ? i : -1;
}

static void try_send_pending_link_updates(void){
    const int n_bits = prte_rml_base.radix+1;

    // Are there any updates we're waiting to send?
    pmix_bitmap_t* dn_bits = &prte_relm_sm->downstream_links_updated;
    int n_pending_dn = pmix_bitmap_num_unset_bits(dn_bits, n_bits);
    if(0 == n_pending_dn) return;

    // If we're waiting on more than one upstream to send us updates after a
    // promotion, we can't have enough information to send any updates ourselves
    pmix_bitmap_t* up_bits = &prte_relm_sm->upstream_links_updated;
    int n_pending_up = pmix_bitmap_num_unset_bits(up_bits, n_bits);

    if(1 < n_pending_up) return;
    if(1 == n_pending_up){
        // We're waiting on exactly one upstream, so we have all other data and
        // can update that one rank if needed. Can't update others until we get
        // that last upstream update.
        int idx;
        pmix_bitmap_find_and_set_first_unset_bit(up_bits, &idx);
        pmix_bitmap_clear_bit(up_bits, idx);

        if(!pmix_bitmap_is_set_bit(dn_bits, idx)){
            pmix_bitmap_set_bit(dn_bits, idx);
            prte_relm_send_link_update(link_i_to_r(idx));
        }
        return;
    }

    // We have all the info, send all needed updates
    int idx;
    for(; n_pending_dn > 0; n_pending_dn--){
        pmix_bitmap_find_and_set_first_unset_bit(dn_bits, &idx);
        prte_relm_send_link_update(link_i_to_r(idx));
    }
}

static size_t purge_rank(
    const prte_rml_recovery_status_t* status, prte_relm_rank_t* rank,
    prte_relm_guid_t* purged_buf
) {
    size_t n_purged = 0;

    prte_relm_guid_t guid;
    prte_relm_msg_t* msg;
    PMIX_HASH_TABLE_FOREACH(guid, uint64, msg, &rank->msgs){
        bool purge = !prte_rml_is_node_up(msg->src);
        if(!purge && status->promoted){
            //We may no longer be in the path for this message
            pmix_rank_t up = prte_relm_sm->upstream_rank(msg);
            pmix_rank_t down = prte_relm_sm->downstream_rank(msg);
            purge = up == down;
        }
        if(purge){
            purged_buf[n_purged++] = PRTE_RELM_GUID(msg);
            PMIX_RELEASE(msg);
        }
    }
    for(size_t i = 0; i < n_purged; i++){
        pmix_hash_table_remove_value_uint64(&rank->msgs, purged_buf[i]);
    }

    return n_purged;
}

static void purge(const prte_rml_recovery_status_t* status){
    for(size_t i = 0; i < status->failed_ranks.size; i++){
        pmix_hash_table_remove_value_uint32(
            &prte_relm_sm->ranks, ((pmix_rank_t*)status->failed_ranks.array)[i]
        );
    }

    pmix_rank_t* empty = malloc(
        pmix_hash_table_get_size(&prte_relm_sm->ranks) * sizeof(pmix_rank_t)
    );
    size_t n_empty = 0;

    prte_relm_guid_t* purged_buf = NULL;
    size_t p_alloc = 0;

    pmix_rank_t dst;
    prte_relm_rank_t* rank;
    PMIX_HASH_TABLE_FOREACH(dst, uint32, rank, &prte_relm_sm->ranks){
        size_t n_msgs = pmix_hash_table_get_size(&rank->msgs);
        if(p_alloc < n_msgs){
            free(purged_buf);
            purged_buf = malloc(n_msgs*sizeof(prte_relm_guid_t));
            p_alloc = n_msgs;
        }

        size_t n_purged = purge_rank(status, rank, purged_buf);

        if(n_msgs == n_purged){
            empty[n_empty++] = dst;
            PMIX_RELEASE(rank);
        }
    }
    for(size_t i = 0; i < n_empty; i++){
        pmix_hash_table_remove_value_uint32(&prte_relm_sm->ranks, empty[i]);
    }

    free(empty);
    free(purged_buf);
}

