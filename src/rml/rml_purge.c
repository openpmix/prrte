/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include "src/util/pmix_output.h"
#include "src/threads/pmix_threads.h"

#include "src/rml/rml.h"
#include "src/rml/oob/oob.h"

void prte_rml_purge(pmix_proc_t* peer){
    prte_rml_posted_recv_t *post, *next_post;
    PMIX_LIST_FOREACH_SAFE(
        post, next_post, &prte_rml_base.posted_recvs, prte_rml_posted_recv_t
    ) {
        // Don't use PMIX_CHECK_PROCID, because we don't want to match wildcards
        if(!PMIx_Check_nspace(post->peer.nspace, peer->nspace)) continue;
        if(post->peer.rank   != peer->rank  ) continue;

        pmix_list_remove_item(&prte_rml_base.posted_recvs, &post->super);
        PMIX_RELEASE(post);
    }

    prte_rml_recv_t *msg, *next_msg;
    PMIX_LIST_FOREACH_SAFE(
        msg, next_msg, &prte_rml_base.unmatched_msgs, prte_rml_recv_t
    ) {
        if(msg->sender.nspace != peer->nspace) continue;
        if(msg->sender.rank   != peer->rank  ) continue;

        pmix_list_remove_item(&prte_rml_base.unmatched_msgs, &msg->super);
        PMIX_RELEASE(msg);
    }
}
