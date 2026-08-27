/*
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <pmix.h>

#include "src/mca/base/pmix_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/pmix_output.h"

#include "src/mca/state/state.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/rml/rml_contact.h"
#include "src/rml/oob/oob.h"
#include "src/rml/relm/relm.h"

prte_rml_base_t prte_rml_base = {
    .rml_output = -1,
    .routed_output = -1,
    .max_retries = 0,
    .posted_recvs = PMIX_LIST_STATIC_INIT(prte_rml_base.posted_recvs),
    .unmatched_msgs = PMIX_LIST_STATIC_INIT(prte_rml_base.unmatched_msgs),
    .radix = 64,
    .static_ports = false,
    .cur_node = { .rank = PMIX_RANK_INVALID },
    .children = PMIX_DATA_ARRAY_STATIC_INIT,
    .n_children = 0,
    .ancestors = PMIX_DATA_ARRAY_STATIC_INIT,
    .lifeline = PMIX_RANK_INVALID,
    .n_dmns = 0,
    .failed_dmns = { .super = PMIX_OBJ_STATIC_INIT(pmix_bitmap_t) },
    .global_failed_dmns = { .super = PMIX_OBJ_STATIC_INIT(pmix_bitmap_t) },
    .dead_dmns = { .super = PMIX_OBJ_STATIC_INIT(pmix_bitmap_t) },
    .absent_dmns = { .super = PMIX_OBJ_STATIC_INIT(pmix_bitmap_t) },
    .revived_dmns = { .super = PMIX_OBJ_STATIC_INIT(pmix_bitmap_t) },
    .lateral_links = { .super = PMIX_OBJ_STATIC_INIT(pmix_bitmap_t) },
    .lateral_lost_cb = NULL,
    .peer_epochs = NULL,
    .peer_epochs_size = 0,
};

uint64_t prte_rml_boot_epoch = 0;

static int verbosity = 0;
/* the departed-rank list handed to us on the command line, if any - owned by
 * the MCA variable system, not by us */
static char *dead_dmns_spec = NULL;

/* The incarnation table is the one piece of RML state a peer's socket handler
 * reaches directly, and that handler runs on whichever base is servicing the
 * peer - one of the process-wide worker threads when any are configured (see
 * prte_num_worker_threads).  Two of them arriving at once would both realloc
 * the array.  The lock is taken only on the bootstrap incarnation check, which
 * is a handful of instructions per message and uncontended whenever the OOB is
 * running on the main thread alone. */
static pmix_mutex_t epoch_lock = PMIX_MUTEX_STATIC_INIT;

/* Grow the per-rank epoch table so index rank is valid, zero-filling new slots
 * (0 = unknown). Ranks are dense and small, so a flat array indexed by rank is
 * ample. Caller must hold epoch_lock. */
static void ensure_epoch_slot(pmix_rank_t rank)
{
    if ((size_t) rank < prte_rml_base.peer_epochs_size) {
        return;
    }
    size_t newsize = (size_t) rank + 8;
    uint64_t *tmp = (uint64_t *) realloc(prte_rml_base.peer_epochs,
                                         newsize * sizeof(uint64_t));
    if (NULL == tmp) {
        return;
    }
    for (size_t i = prte_rml_base.peer_epochs_size; i < newsize; i++) {
        tmp[i] = 0;
    }
    prte_rml_base.peer_epochs = tmp;
    prte_rml_base.peer_epochs_size = newsize;
}

bool prte_rml_epoch_ok(pmix_rank_t rank, uint64_t epoch)
{
    uint64_t known;
    bool ok = true;

    /* an unstamped message (epoch 0) carries no incarnation claim - accept */
    if (0 == epoch) {
        return true;
    }
    pmix_mutex_lock(&epoch_lock);
    ensure_epoch_slot(rank);
    if ((size_t) rank >= prte_rml_base.peer_epochs_size) {
        /* allocation failed - fail open rather than drop live traffic */
        goto done;
    }
    known = prte_rml_base.peer_epochs[rank];
    if (0 == known) {
        /* first time we have seen this rank - learn its epoch */
        prte_rml_base.peer_epochs[rank] = epoch;
        goto done;
    }
    if (epoch < known) {
        /* stale incarnation - drop. A newer epoch passes but does not advance
         * the table here; the arbitrated revival advances it authoritatively. */
        ok = false;
    }

done:
    pmix_mutex_unlock(&epoch_lock);
    return ok;
}

void prte_rml_record_epoch(pmix_rank_t rank, uint64_t epoch)
{
    if (0 == epoch) {
        return;
    }
    pmix_mutex_lock(&epoch_lock);
    ensure_epoch_slot(rank);
    if ((size_t) rank < prte_rml_base.peer_epochs_size) {
        prte_rml_base.peer_epochs[rank] = epoch;
    }
    pmix_mutex_unlock(&epoch_lock);
}

uint64_t prte_rml_get_epoch(pmix_rank_t rank)
{
    if ((size_t) rank < prte_rml_base.peer_epochs_size) {
        return prte_rml_base.peer_epochs[rank];
    }
    return 0;
}

char *prte_rml_render_dead_dmns(void)
{
    char **entries = NULL;
    char *result, *tmp;
    int n, sz, start;

    sz = pmix_bitmap_size(&prte_rml_base.dead_dmns);
    for (n = 0; n < sz; n++) {
        if (!pmix_bitmap_is_set_bit(&prte_rml_base.dead_dmns, n)) {
            continue;
        }
        /* Collapse a run into "first:last". A DVM that has shrunk repeatedly
         * can otherwise put hundreds of numbers onto a command line that is
         * already measured against _SC_ARG_MAX by every launcher.
         *
         * The separator is a colon rather than the dash such a range is
         * usually written with, because this value goes on a command line:
         * every released PMIx that PRRTE accepts (>= 6.1.0) refuses an MCA
         * value whose second character is a dash, reading it as a missing
         * argument, so "2-3" would fail to launch. The fix for that is on PMIx
         * master only and in no tag. */
        start = n;
        while (n + 1 < sz && pmix_bitmap_is_set_bit(&prte_rml_base.dead_dmns, n + 1)) {
            ++n;
        }
        if (start == n) {
            pmix_asprintf(&tmp, "%d", start);
        } else {
            pmix_asprintf(&tmp, "%d:%d", start, n);
        }
        PMIx_Argv_append_nosize(&entries, tmp);
        free(tmp);
    }

    if (NULL == entries) {
        return NULL;
    }
    result = PMIx_Argv_join(entries, ',');
    PMIx_Argv_free(entries);
    return result;
}

void prte_rml_load_dead_dmns(const char *spec)
{
    char **entries;
    int n;
    long r, first, last;
    char *end;
    bool ok;

    if (NULL == spec || '\0' == spec[0]) {
        return;
    }

    entries = PMIx_Argv_split(spec, ',');
    if (NULL == entries) {
        return;
    }
    for (n = 0; NULL != entries[n]; n++) {
        ok = false;
        end = NULL;
        first = strtol(entries[n], &end, 10);
        if (end != entries[n] && 0 <= first) {
            if (':' == *end) {
                char *p = end + 1;
                last = strtol(p, &end, 10);
                ok = (end != p && last >= first);
            } else {
                last = first;
                ok = true;
            }
            /* the whole entry has to be consumed, and a rank outside the DVM's
             * vpid span is not a hole in it */
            if (ok && ('\0' != *end || (long) prte_process_info.num_daemons <= last)) {
                ok = false;
            }
        }
        if (!ok) {
            pmix_output(0, "PRRTE: ignoring malformed departed-daemon rank \"%s\"",
                        entries[n]);
            continue;
        }
        for (r = first; r <= last; r++) {
            pmix_bitmap_set_bit(&prte_rml_base.dead_dmns, (int) r);
        }
    }
    PMIx_Argv_free(entries);
}

void prte_rml_register(void)
{
    int ret;

    prte_rml_base.max_retries = 3;
    pmix_mca_base_var_register("prte", "rml", "base", "max_retries",
                               "Max #times to retry sending a message",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_rml_base.max_retries);

    verbosity = 0;
    pmix_mca_base_var_register("prte", "rml", "base", "verbose",
                               "Debug verbosity of the RML subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_rml_base.rml_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_rml_base.rml_output, verbosity);
    }

    verbosity = 0;
    pmix_mca_base_var_register("prte", "routed", "base", "verbose",
                               "Debug verbosity of the Routed subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_rml_base.routed_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_rml_base.routed_output, verbosity);
    }

    ret = pmix_mca_base_var_register("prte", "rml", "base", "radix",
                                     "Radix to be used for routing tree (minimum 2)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.radix);
    pmix_mca_base_var_register_synonym(ret, "prte", "routed", "radix", NULL,
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    /* The radix-tree math divides by (radix - 1) and takes a logarithm to base
     * radix, so anything below 2 is not a degenerate tree - it is a division by
     * zero the moment a second daemon exists. This is the only place a user
     * value enters, so clamp it here rather than defending every call site. */
    if (2 > prte_rml_base.radix) {
        pmix_output(0, "PRRTE: routing tree radix %d is invalid (minimum is 2)"
                       " - using 2", prte_rml_base.radix);
        prte_rml_base.radix = 2;
    }

    /* The ranks that had already departed the DVM when we were launched. Only
     * a launcher sets this, on the prted command line: a daemon started into a
     * DVM that has lost ranks has to know which they are BEFORE it computes its
     * first routing tree, and no message can tell it - the request would have
     * to travel over the very tree it has not got right. See prte_rml_open. */
    dead_dmns_spec = NULL;
    pmix_mca_base_var_register("prte", "rml", "base", "dead_dmns",
                               "Comma-separated daemon vpids (ranges as first:last, e.g. \"2,7:9\")"
                               " that had permanently departed the DVM when this daemon was"
                               " launched. Set by the launcher; not intended for users",
                               PMIX_MCA_BASE_VAR_TYPE_STRING,
                               &dead_dmns_spec);

    prte_oob_register();

    verbosity = 0;
    pmix_mca_base_var_register("prte", "oob", "base", "verbose",
                               "Debug verbosity of the out-of-band subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_oob_base.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_oob_base.output, verbosity);
    }

    prte_relm_register();
}

void prte_rml_close(void)
{
    prte_relm_close();
    prte_oob_close();
    PMIX_LIST_DESTRUCT(&prte_rml_base.posted_recvs);
    PMIX_LIST_DESTRUCT(&prte_rml_base.unmatched_msgs);
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
    PMIX_DESTRUCT(&prte_rml_base.global_failed_dmns);
    PMIX_DESTRUCT(&prte_rml_base.dead_dmns);
    PMIX_DESTRUCT(&prte_rml_base.absent_dmns);
    PMIX_DESTRUCT(&prte_rml_base.revived_dmns);
    PMIX_DESTRUCT(&prte_rml_base.lateral_links);
    if (NULL != prte_rml_base.peer_epochs) {
        free(prte_rml_base.peer_epochs);
        prte_rml_base.peer_epochs = NULL;
        prte_rml_base.peer_epochs_size = 0;
    }
    PMIx_Data_array_destruct(&prte_rml_base.ancestors);
    PMIx_Data_array_destruct(&prte_rml_base.children);
    if (0 <= prte_rml_base.rml_output) {
        pmix_output_close(prte_rml_base.rml_output);
        prte_rml_base.rml_output = -1;
    }
    if (0 <= prte_rml_base.routed_output) {
        pmix_output_close(prte_rml_base.routed_output);
        prte_rml_base.routed_output = -1;
    }
}

int prte_rml_open(void)
{
    char *uri = NULL;
    pmix_value_t val;
    int ret;

    /* construct object for holding the active plugin modules */
    PMIX_CONSTRUCT(&prte_rml_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.unmatched_msgs, pmix_list_t);

    /* construct objects for holding failure information */
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    PMIX_CONSTRUCT(&prte_rml_base.global_failed_dmns, pmix_bitmap_t);
    /* the permanent departed-daemon set is initialized once here and never
     * re-initialized: the bitmap auto-expands as ranks are marked, and it must
     * persist across the recomputes that DVM grows trigger (#2491) */
    PMIX_CONSTRUCT(&prte_rml_base.dead_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.dead_dmns, prte_process_info.num_daemons);
    /* absent_dmns holds bootstrap daemons that are gone but may return; like
     * dead_dmns it persists across recomputes, but it is cleared when a daemon
     * comes back (the unheal path). Initialized once here for the same reason. */
    PMIX_CONSTRUCT(&prte_rml_base.absent_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.absent_dmns, prte_process_info.num_daemons);
    /* revived_dmns records the ranks that have come back, so that a peer's
     * report of our lineage - which may predate the return - can never be the
     * thing that declares one of them dead again. Constructed once here for
     * the same reason as the two above. */
    PMIX_CONSTRUCT(&prte_rml_base.revived_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.revived_dmns, prte_process_info.num_daemons);
    /* lateral_links records the peers we hold a non-tree connection to. Like
     * the two above it is constructed once and never re-initialized by a
     * routing recompute: a grow reshapes the tree but does not dissolve the
     * exchange partners a collective is midway through talking to. */
    PMIX_CONSTRUCT(&prte_rml_base.lateral_links, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.lateral_links, prte_process_info.num_daemons);

    /* Capture this process's boot epoch (incarnation): a millisecond wall-clock
     * timestamp taken once here. A departed daemon that reboots into the same
     * rank returns with a strictly-greater epoch, letting peers drop late
     * traffic from the stale incarnation. Millisecond granularity keeps the
     * degenerate same-timestamp reboot vanishingly unlikely; the HNP's return
     * validation (strictly-greater epoch) catches it if it ever happens. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    prte_rml_boot_epoch = (uint64_t) tv.tv_sec * 1000 + (uint64_t) tv.tv_usec / 1000;

    /* set up failure notification receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DAEMON_DIED, true,
                  prte_rml_recv_failures_notice, NULL);
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DAEMON_ADOPTED, true,
                  prte_rml_recv_adoption_notice, NULL);

    /* set up return/revival receives for the bootstrap unheal path */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DAEMON_RETURNED, true,
                  prte_rml_recv_return_request, NULL);
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DAEMON_REVIVED, true,
                  prte_rml_recv_revival_notice, NULL);

    /* Adopt the departed ranks our launcher told us about, BEFORE the first
     * tree computation below. A daemon launched into a DVM that has already
     * shrunk starts with an empty dead set, so the raw radix math can hand it a
     * retired vpid as its parent - and then every message it sends upward,
     * including the warm-up that asks for the nidmap that would have corrected
     * the set, is addressed to a rank nothing can contact. The DVM never reuses
     * a daemon vpid, so the hole is permanent and this is the only moment the
     * newcomer can learn of it (#2491). */
    prte_rml_load_dead_dmns(dead_dmns_spec);

    /* compute the routing tree - only thing we need to know is the
     * number of daemons in the DVM */
    prte_rml_compute_routing_tree();

    prte_rml_base.lifeline = PRTE_PROC_MY_PARENT->rank;

    /* Bring up the transport. This fails for real reasons a user can cause -
     * an if_include/if_exclude that leaves no usable interface, or a static
     * port range nothing could bind - and every one of them leaves us with no
     * address to advertise. Ignoring the return meant walking on to
     * get_addr(), which answers NULL, and then strdup()ing it: a segfault
     * where the user should have gotten "no interfaces available". */
    ret = prte_oob_open();
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    /* store our URI for later */
    prte_oob_base_get_addr(&uri);
    if (NULL == uri) {
        /* the listeners came up but produced no advertisable address */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_AVAILABLE);
        return PRTE_ERR_NOT_AVAILABLE;
    }
    PMIX_VALUE_LOAD(&val, uri, PMIX_STRING);
    ret = PMIx_Store_internal(PRTE_PROC_MY_NAME, PMIX_PROC_URI, &val);
    if (PMIX_SUCCESS != ret) {
        PRTE_ERROR_LOG(PRTE_ERROR);
        PMIX_VALUE_DESTRUCT(&val);
        free(uri);
        return PRTE_ERROR;
    }
    PMIX_VALUE_DESTRUCT(&val);
    // add it to our local info
    prte_process_info.my_uri = strdup(uri);

    if (PRTE_PROC_IS_MASTER) {
        prte_process_info.my_hnp_uri = uri;
    } else {
        free(uri);
        if (NULL == prte_process_info.my_hnp_uri) {
            // this is an error
            PRTE_ERROR_LOG(PRTE_ERROR);
            return PRTE_ERROR;
        }
        /* extract the HNP's name so we can update the routing table */
        ret = prte_rml_parse_uris(prte_process_info.my_hnp_uri,
                                  PRTE_PROC_MY_HNP,
                                  NULL);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        /* Set the contact info in the RML - this won't actually establish
         * the connection, but just tells the RML how to reach the HNP
         * if/when we attempt to send to it
         */
        PMIX_VALUE_LOAD(&val, prte_process_info.my_hnp_uri, PMIX_STRING);
        ret = PMIx_Store_internal(PRTE_PROC_MY_HNP, PMIX_PROC_URI, &val);
        if (PMIX_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            PMIX_VALUE_DESTRUCT(&val);
            return ret;
        }
        PMIX_VALUE_DESTRUCT(&val);
    }

    prte_relm_open();
    return PRTE_SUCCESS;
}

void prte_rml_simulate_node_failure(void)
{
    prte_oob_simulate_node_failure();
    raise(SIGKILL);
}

bool prte_rml_is_node_up(pmix_rank_t node){
    return node < prte_rml_base.n_dmns
        && !pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, node);
}

void prte_rml_send_callback(int status, pmix_proc_t *peer,
                            pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tag, void *cbdata)

{
    PRTE_HIDE_UNUSED_PARAMS(cbdata);
    if (NULL != buffer) {
        PMIX_DATA_BUFFER_RELEASE(buffer);
    }

    if (PRTE_SUCCESS != status) {
        pmix_output_verbose(2, prte_rml_base.rml_output,
                            "%s UNABLE TO SEND MESSAGE TO %s TAG %d: %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer), tag,
                            PRTE_ERROR_NAME(status));
        if (PRTE_ERR_NO_PATH_TO_TARGET == status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_NO_PATH_TO_TARGET);
        } else if (PRTE_ERR_ADDRESSEE_UNKNOWN == status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_PEER_UNKNOWN);
        } else if (PRTE_ERR_NODE_DOWN != status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
        }
    }
}

/***   RML CLASS INSTANCES   ***/
static void send_cons(prte_rml_send_t *ptr)
{
    ptr->retries = 0;
    ptr->cbfunc = prte_rml_send_callback;
    ptr->cbdata = NULL;
    ptr->dbuf = NULL;
    /* an ordinary send owns its own buffer - PMIX_NEW mallocs rather than
     * zeroing, so leaving this unset would have the destructor release a
     * garbage pointer as if it were a shared payload */
    ptr->payload = NULL;
    ptr->seq_num = 0xFFFFFFFF;
    /* Default to this process's own epoch: a message built here originates
     * here. The relay path overrides this with the origin's epoch carried in
     * the received wire header so a relayed message keeps its original stamp. */
    ptr->epoch = prte_rml_boot_epoch;
    /* routed unless the caller asks otherwise - PMIX_NEW mallocs, it does
     * not zero, so this field is heap garbage until it is set */
    ptr->direct = false;
}
static void send_des(prte_rml_send_t *ptr)
{
    if (NULL != ptr->payload) {
        /* the buffer belongs to the payload, which may still be in flight to
         * other destinations - drop our reference and let the last one out
         * free it */
        PMIX_RELEASE(ptr->payload);
    } else if (NULL != ptr->dbuf) {
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
    }
}
PMIX_CLASS_INSTANCE(prte_rml_send_t, pmix_list_item_t, send_cons, send_des);

static void payload_cons(prte_rml_payload_t *ptr)
{
    ptr->dbuf = NULL;
}
static void payload_des(prte_rml_payload_t *ptr)
{
    if (NULL != ptr->dbuf) {
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
    }
}
PMIX_CLASS_INSTANCE(prte_rml_payload_t, pmix_object_t, payload_cons, payload_des);

static void send_req_cons(prte_rml_send_request_t *ptr)
{
    PMIX_CONSTRUCT(&ptr->send, prte_rml_send_t);
}
static void send_req_des(prte_rml_send_request_t *ptr)
{
    PMIX_DESTRUCT(&ptr->send);
}
PMIX_CLASS_INSTANCE(prte_rml_send_request_t, pmix_object_t, send_req_cons, send_req_des);

static void recv_cons(prte_rml_recv_t *ptr)
{
    ptr->dbuf = NULL;
}
static void recv_des(prte_rml_recv_t *ptr)
{
    /* the buffer object is ours even when the recv callback unloaded its
     * payload - releasing only when payload remains left the (now empty)
     * buffer itself behind on every message whose handler took the data */
    if (NULL != ptr->dbuf) {
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
    }
}
PMIX_CLASS_INSTANCE(prte_rml_recv_t, pmix_list_item_t, recv_cons, recv_des);

static void rcv_cons(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_CONSTRUCT(&ptr->data);
    ptr->active = false;
}
static void rcv_des(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_DESTRUCT(&ptr->data);
}
PMIX_CLASS_INSTANCE(prte_rml_recv_cb_t, pmix_object_t, rcv_cons, rcv_des);

static void prcv_cons(prte_rml_posted_recv_t *ptr)
{
    ptr->cbdata = NULL;
}
PMIX_CLASS_INSTANCE(prte_rml_posted_recv_t, pmix_list_item_t, prcv_cons, NULL);

static void prq_cons(prte_rml_recv_request_t *ptr)
{
    ptr->cancel = false;
    ptr->post = PMIX_NEW(prte_rml_posted_recv_t);
}
static void prq_des(prte_rml_recv_request_t *ptr)
{
    if (NULL != ptr->post) {
        PMIX_RELEASE(ptr->post);
    }
}
PMIX_CLASS_INSTANCE(prte_rml_recv_request_t, pmix_object_t, prq_cons, prq_des);

static void rscon(prte_rml_recovery_status_t* p){
    p->scope = PRTE_RML_FAULT_SCOPE_LOCAL;
    p->failed_ranks = (pmix_data_array_t) PMIX_DATA_ARRAY_STATIC_INIT;
    p->epoch = 0;
    p->promoted = false;
    p->demoted = false;

    p->ancestors_changed = false;
    p->prev_ancestors = (pmix_data_array_t) PMIX_DATA_ARRAY_STATIC_INIT;
    PMIx_Data_array_construct(
        &p->prev_ancestors, prte_rml_base.ancestors.size, PMIX_PROC_RANK
    );
    for(size_t i = 0; i < p->prev_ancestors.size; i++){
        ((pmix_rank_t*)p->prev_ancestors.array)[i] =
            ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
    }

    p->parent_changed = false;
    p->prev_parent = prte_rml_base.lifeline;

    p->children_changed = false;
    p->prev_children = (pmix_data_array_t) PMIX_DATA_ARRAY_STATIC_INIT;
    PMIx_Data_array_construct(
        &p->prev_children, prte_rml_base.children.size, PMIX_PROC_RANK
    );
    for(size_t i = 0; i < p->prev_children.size; i++){
        ((pmix_rank_t*)p->prev_children.array)[i] =
            ((pmix_rank_t*)prte_rml_base.children.array)[i];
    }
}
static void rsdes(prte_rml_recovery_status_t* p){
    PMIx_Data_array_destruct(&p->failed_ranks);
    PMIx_Data_array_destruct(&p->prev_ancestors);
    PMIx_Data_array_destruct(&p->prev_children);
}
PMIX_CLASS_INSTANCE(prte_rml_recovery_status_t, pmix_object_t, rscon, rsdes);
