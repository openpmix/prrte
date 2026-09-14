/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "constants.h"

#include <string.h>

#include "src/mca/state/state.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

#include "src/rml/rml.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/types.h"
#include "src/rml/relm/util.h"

// Handle a state update started by a downstream's message
static void downstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
);

// Handle a state update started by an upstream's message
static void upstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
);

// Handle a state update requested locally
static void local_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
);

// Callback for cache timeout events. Calls a state update on msg in cb_data
static void evict(int fd, short args, void* cb_data);

// Set while local_update is posting a run of PENDING messages, so the ACK of
// each one leaves the walk to the loop rather than recursing into the next
static bool waking_pending = false;

int prte_relm_pack_state_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg
) {
    /* every caller treats the answer as a PMIx status, and the packers below
     * produce one, so the argument check has to speak the same language */
    int ret = PMIX_SUCCESS;
    if (NULL == buf || NULL == msg) {
        ret = PMIX_ERR_BAD_PARAM;
    }

    if (PMIX_SUCCESS == ret) {
        ret = prte_relm_pack_signature(buf, msg);
    }
    if (PMIX_SUCCESS == ret) {
        ret = prte_relm_pack_uid(buf, msg->prev_uid);
    }
    if (PMIX_SUCCESS == ret) {
        ret = prte_relm_pack_state(buf, msg->state);
    }
    // test ret first: it is the only thing guarding msg against NULL
    if (PMIX_SUCCESS == ret && PRTE_RELM_STATE_SENDING == msg->state) {
        ret = prte_relm_pack_data(buf, msg->data);
    }

    if (PMIX_SUCCESS != ret) {
        PRTE_RELM_MSG_ERROR_LOG(msg, prte_pmix_convert_status(ret));
    }
    return ret;
}

void prte_relm_handle_state_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state,
    pmix_rank_t src
) {
    if (PRTE_PROC_MY_NAME->rank == src) {
        local_update(buf, msg, state);
    } else if (prte_relm_downstream_rank(msg) == src) {
        downstream_update(buf, msg, state);
    } else if (prte_relm_upstream_rank(msg) == src) {
        upstream_update(buf, msg, state);
    }
    // Ignore lingering messages from old links - prte_relm_message_handler
    // has already dropped any it received, before looking the message up
}

static void downstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
) {
    PRTE_HIDE_UNUSED_PARAMS(buf);
    switch (state) {
    case PRTE_RELM_STATE_ACKED:
        if (PRTE_RELM_STATE_INVALID == msg->state) {
            // Unknown msg, must have been ACKACKED already
            prte_relm_update_state(msg, PRTE_RELM_STATE_ACKACKED);
            break;
        }
        prte_relm_update_state(msg, PRTE_RELM_STATE_ACKED);
        break;

    case PRTE_RELM_STATE_REQUESTED:
        prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
        break;

    default:
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
    }
}

static void upstream_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
) {
    switch (state) {
    case PRTE_RELM_STATE_SENDING: {
        pmix_byte_object_t bo = prte_relm_unpack_data(buf);
        if (0 == bo.size) {
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else if (NULL != msg->data.bytes) {
            /* Seeing SENDING again for a message we already hold the data for
             * is ordinary - a replay we asked for, or a resend after the tree
             * moved - and it carries the same bytes, so keeping ours and
             * dropping the copy is right.
             *
             * Different bytes under the same <src,uid,dst> is not ordinary.
             * Two distinct messages are wearing one identity, and only one of
             * them can be delivered under it: whichever we drop here is lost
             * for good, and its sender will be ACKed for the other one and
             * believe it arrived. Silently losing a message is the one thing
             * this layer exists to prevent, and we cannot tell which of the
             * two payloads is the real one, so stop rather than deliver the
             * wrong bytes and call it success. The UIDs are generated, so
             * arriving here at all means our identity space is already
             * broken. */
            bool same = bo.size == msg->data.size &&
                        0 == memcmp(bo.bytes, msg->data.bytes, bo.size);
            PMIx_Byte_object_destruct(&bo);
            if (!same) {
                PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_FATAL);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                break;
            }
        } else {
            msg->data = bo;
        }
        prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
        break;
    }

    case PRTE_RELM_STATE_ACKACKED:
        prte_relm_update_state(msg, PRTE_RELM_STATE_ACKACKED);
        break;

    // The rest will only be sent from upstream during a link update,
    // which lets us simplify the logic at times
    case PRTE_RELM_STATE_SENT:
        /* Upstream is telling a new neighbour that it passed this message
         * on. If we have no record of it, it did not arrive: the daemon that
         * held it died between us. Nothing below us can have it either - we
         * are the only way down - and a link update only goes to links that
         * changed, so no one further down may ever hear of it. Recording SENT
         * and waiting, which is what the generic transition does, strands the
         * message for good with its data pinned at the source. Ask for the
         * replay instead: a duplicate is harmless, because a message that has
         * already been posted only ACKs again.
         *
         * The destination cannot take the generic transition at all, since
         * a local SENT means *we* passed the message on and the destination
         * never does. What upstream needs from it is whatever it has already
         * told the link that died - an ACK, or a request for a replay. */
        if (PRTE_RELM_STATE_INVALID == msg->state) {
            PRTE_RELM_MSG_OUTPUT(1, msg, "requesting replay of a lost message");
            prte_relm_update_state(msg, PRTE_RELM_STATE_REQUESTED);
        } else if (PRTE_PROC_MY_NAME->rank == msg->dst) {
            if (PRTE_RELM_STATE_ACKED == msg->state ||
                PRTE_RELM_STATE_REQUESTED == msg->state) {
                prte_relm_send_state_upstream(msg);
            }
        } else {
            prte_relm_update_state(msg, PRTE_RELM_STATE_SENT);
        }
        break;

    case PRTE_RELM_STATE_ACKED:
        // Must either be invalid or acked currently, so no full update needed
        msg->state = PRTE_RELM_STATE_ACKED;
        break;

    case PRTE_RELM_STATE_REQUESTED:
        if (PRTE_RELM_STATE_ACKED == msg->state) {
            PRTE_RELM_MSG_OUTPUT(1, msg, "replaying ack");
            prte_relm_send_state_upstream(msg);
        } else if (PRTE_RELM_STATE_INVALID == msg->state) {
            PRTE_RELM_MSG_OUTPUT(1, msg, "requesting replay");
            prte_relm_update_state(msg, PRTE_RELM_STATE_REQUESTED);
        }
        break;

    default:
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
    }
}

static void local_update(
    pmix_data_buffer_t* buf, prte_relm_msg_t* msg, prte_relm_state_t state
) {
    switch (state) {
    case PRTE_RELM_STATE_SENT:
        if (PRTE_PROC_MY_NAME->rank == msg->dst) {
            // Can't have sent the message downstream if I'm the destination
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else if (PRTE_RELM_STATE_SENDING == msg->state) {
            msg->state = state;
            pmix_rank_t r = PRTE_PROC_MY_NAME->rank;
            /* a replay's cached copy can expire while the replay is on the
             * wire, and then there is nothing left to cache */
            if (r != msg->src && r != msg->dst && NULL != msg->data.bytes) {
                prte_relm_update_state(msg, PRTE_RELM_STATE_CACHED);
            }
        } else if (PRTE_RELM_STATE_ACKED == msg->state ||
                   PRTE_RELM_STATE_REQUESTED == msg->state) {
            prte_relm_send_state_upstream(msg);
        } else if (PRTE_RELM_STATE_INVALID == msg->state) {
            msg->state = state;
        }
        break;

    case PRTE_RELM_STATE_REQUESTED:
        if (PRTE_RELM_STATE_SENT == msg->state ||
            PRTE_RELM_STATE_INVALID == msg->state) {
            if (NULL != msg->data.bytes) {
                PRTE_RELM_MSG_OUTPUT(1, msg, "replaying");
                prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
            } else {
                msg->state = state;
                prte_relm_send_state_upstream(msg);
            }
        } else if (PRTE_RELM_STATE_ACKED == msg->state) {
            prte_relm_send_state_upstream(msg);
        }
        break;

    case PRTE_RELM_STATE_SENDING:
        if (PRTE_RELM_STATE_ACKED == msg->state) {
            prte_relm_send_state_upstream(msg);
            prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);
        } else if (NULL == msg->data.bytes) {
            prte_relm_update_state(msg, PRTE_RELM_STATE_REQUESTED);
        } else if (PRTE_PROC_MY_NAME->rank == msg->dst) {
            if (prte_relm_prev_is_posted(msg)) {
                msg->state = PRTE_RELM_STATE_SENT;
                prte_relm_post(msg);
                prte_relm_update_state(msg, PRTE_RELM_STATE_ACKED);
            } else {
                prte_relm_update_state(msg, PRTE_RELM_STATE_PENDING);
            }
        } else if (PRTE_RELM_STATE_SENDING != msg->state) {
            msg->state = state;
            prte_relm_send_state_downstream(msg);
        }
        break;

    case PRTE_RELM_STATE_PENDING:
        if (PRTE_RELM_STATE_ACKED == msg->state) {
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else {
            msg->state = state;
        }
        break;

    case PRTE_RELM_STATE_ACKED: {
        if (PRTE_PROC_MY_NAME->rank == msg->src) {
            msg->state = PRTE_RELM_STATE_ACKED;
            prte_relm_update_state(msg, PRTE_RELM_STATE_ACKACKED);
            break;
        } else if (state == msg->state) {
            break;
        }

        msg->state = state;
        prte_relm_send_state_upstream(msg);
        prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);

        // Previous messages are implicitly acked
        prte_relm_msg_t* prev = msg;
        while (NULL != (prev = prte_relm_find_prev_msg(prev))) {
            if (state == prev->state) {
                break;
            }
            prev->state = state;
            prte_relm_update_state(prev, PRTE_RELM_STATE_EVICTED);
        }

        /* Posting the next PENDING message ACKs it, and that ACK arrives
         * right back here to post the one after. Done as recursion that is
         * several frames per message, so the stack grows with the backlog -
         * and a backlog is exactly what builds up behind a message lost to a
         * daemon failure, until its replay arrives and releases the lot at
         * once. Walk the chain from the outermost ACK instead; the nested
         * ACKs only get as far as this point. */
        if (waking_pending) {
            break;
        }
        waking_pending = true;
        prte_relm_msg_t* next = prte_relm_find_next_msg(msg);
        while (NULL != next && PRTE_RELM_STATE_PENDING == next->state) {
            prte_relm_signature_t sig = {
                .src = next->src,
                .uid = next->uid,
                .dst = next->dst
            };
            prte_relm_update_state(next, PRTE_RELM_STATE_SENDING);
            /* look it up again rather than trust the pointer: an update is
             * free to complete and release the message it was given */
            next = prte_relm_find_msg(&sig);
            if (NULL == next || PRTE_RELM_STATE_ACKED != next->state) {
                // gone, or still waiting on its predecessor
                break;
            }
            next = prte_relm_find_next_msg(next);
        }
        waking_pending = false;
        break;
    }

    case PRTE_RELM_STATE_NEW:
        if (PRTE_RELM_STATE_INVALID != msg->state) {
            /* Either two attempts to start the same msg, or somehow a new msg
             * was given the same uid as an existing msg. The UID came from our
             * own counter and every message we start is a fresh signature, so
             * neither can happen while this daemon's own bookkeeping is sound:
             * getting here means our message table no longer identifies
             * messages uniquely, and from now on an ACK may credit the wrong
             * one. There is nothing narrower to do - handing the message a
             * different UID would leave the stale entry that proves the
             * counter is untrustworthy, and every other daemon on the path
             * keys on the same <src,uid> we just duplicated. */
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_FATAL);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        } else if (NULL == buf) {
            // We need the msg data for this state update
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
        } else {
            int rc = PMIx_Data_unload(buf, &msg->data);
            if (PMIX_SUCCESS != rc) {
                /* without the payload there is nothing to send, and carrying
                 * on sends a replay request to ourselves - which fails the
                 * job anyway, reporting the wrong thing. The message was
                 * never linked in, so it goes back out the way it came. */
                PRTE_RELM_MSG_ERROR_LOG(msg, prte_pmix_convert_status(rc));
                prte_relm_release_msg(msg);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                break;
            }

            prte_relm_rank_t* rank = prte_relm_get_rank(msg->dst);
            msg->prev_uid = rank->my_last_msg;
            rank->my_last_msg = msg->uid;

            prte_relm_msg_t* prev_msg = prte_relm_find_prev_msg(msg);
            if (NULL != prev_msg) {
                prev_msg->next_uid = msg->uid;
            }

            prte_relm_update_state(msg, PRTE_RELM_STATE_SENDING);
        }
        break;

    case PRTE_RELM_STATE_ACKACKED:
        if (PRTE_PROC_MY_NAME->rank != msg->dst) {
            /* send_state_downstream packs msg->state, so ACKACKED has to be
             * in place while it runs - and only while it runs. It is an
             * ephemeral state, and the message can outlive the release
             * below: a send still in flight holds a reference, and so can a
             * cache entry's eviction. Anything that reaches the message
             * through prte_relm_update_state finds an ephemeral state stored
             * and fails the job instead - and when that is the destructor's
             * eviction, the refusal also leaves the freed message linked on
             * the cache list. */
            prte_relm_state_t held = msg->state;
            msg->state = state;
            prte_relm_send_state_downstream(msg);
            msg->state = held;
        }
        if (PRTE_PROC_MY_NAME->rank == msg->src) {
            prte_relm_rank_t* rank = prte_relm_get_rank(msg->dst);
            if (rank->my_last_msg == msg->uid) {
                rank->my_last_msg = PRTE_RELM_UID_NONE;
            }
        }
        prte_relm_release_msg(msg);
        break;

    case PRTE_RELM_STATE_CACHED: {
        if (NULL == msg->data.bytes) {
            // Don't cache without the data
            PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
            break;
        }

        if (msg->cached) {
            pmix_list_remove_item(&prte_relm_sm->cached_messages, &msg->super);
            prte_event_evtimer_del(&msg->eviction_ev);
        }

        msg->cached = true;
        pmix_list_append(&prte_relm_sm->cached_messages, &msg->super);
        // Cache timeout event calls update to evicted state
        prte_event_evtimer_set(prte_event_base, &msg->eviction_ev, evict, msg);
        prte_event_evtimer_add(&msg->eviction_ev, &prte_relm_sm->cache_tv);

        size_t n = pmix_list_get_size(&prte_relm_sm->cached_messages);
        if (n > prte_relm_sm->max_cache_count) {
            prte_relm_msg_t* first = (prte_relm_msg_t*)
                pmix_list_get_first(&prte_relm_sm->cached_messages);
            prte_relm_update_state(first, PRTE_RELM_STATE_EVICTED);
        }
        break;
    }

    case PRTE_RELM_STATE_EVICTED:
        if (msg->cached) {
            msg->cached = false;
            pmix_list_remove_item(&prte_relm_sm->cached_messages, &msg->super);
            prte_event_evtimer_del(&msg->eviction_ev);
        }
        if (NULL != msg->data.bytes) {
            PMIx_Byte_object_destruct(&msg->data);
        }
        break;

    default:
        PRTE_RELM_MSG_ERROR_LOG(msg, PRTE_ERR_BAD_PARAM);
    }
}

static void evict(int fd, short args, void* cb_data) {
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
    prte_relm_msg_t* msg = (prte_relm_msg_t*) cb_data;
    prte_relm_update_state(msg, PRTE_RELM_STATE_EVICTED);
}
