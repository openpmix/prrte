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
        // PMIX_CHECK_PROCID_STRICT, not PMIX_CHECK_PROCID: purging is by
        // identity, and neither an unset nspace nor a wildcard rank may be
        // allowed to stand for anybody else's posted receive
        if(!PMIX_CHECK_PROCID_STRICT(&post->peer, peer)) continue;

        pmix_list_remove_item(&prte_rml_base.posted_recvs, &post->super);
        PMIX_RELEASE(post);
    }

    prte_rml_recv_t *msg, *next_msg;
    PMIX_LIST_FOREACH_SAFE(
        msg, next_msg, &prte_rml_base.unmatched_msgs, prte_rml_recv_t
    ) {
        // as above; and note it compares the nspace strings rather than the
        // addresses of the two char arrays - "!=" on those is a pointer
        // comparison that is always true, so no held message was ever purged
        if(!PMIX_CHECK_PROCID_STRICT(&msg->sender, peer)) continue;

        pmix_list_remove_item(&prte_rml_base.unmatched_msgs, &msg->super);
        PMIX_RELEASE(msg);
    }
}
