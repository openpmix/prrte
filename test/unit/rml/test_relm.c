/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for RELM's message identity: the UID generator, the
 * <src,uid,dst> signature and its GUID hash, the find/get lookup helpers,
 * and the prev/next ordering chain.
 *
 * This is the part of RELM that is pure computation over the state machine's
 * hash tables -- no progress thread, no sockets, no peer daemons -- so it is
 * the part a unit test can reach.  Everything else in RELM is about what
 * several daemons do when one of them dies, and lives in the container
 * harness (contrib/dockerswarm).
 *
 * The state machine is stood up by hand with PMIX_NEW rather than through
 * prte_relm_open(), which would also post two persistent RML receives and so
 * would need an RML that is open.
 *
 * The protocol cases at the end drive prte_relm_handle_state_update directly,
 * from a chosen position in a small radix-2 tree.  Whatever they send is only
 * queued on prte_event_base, which the test never runs, so what they can
 * check is the state each message is left in -- and that nothing activated a
 * job state, which RELM only ever does to fail the job.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/event/event-internal.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/rml/rml.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/types.h"
#include "src/rml/relm/util.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

#define NDMNS 8

/* Rebuild the state machine so each test starts from an empty message
 * table and a UID counter at zero */
static void sm_reset(void)
{
    if (NULL != prte_relm_sm) {
        PMIX_RELEASE(prte_relm_sm);
    }
    prte_relm_sm = PMIX_NEW(prte_relm_state_machine_t);
}

static prte_relm_msg_t *get_msg(pmix_rank_t src, prte_relm_uid_t uid, pmix_rank_t dst)
{
    prte_relm_signature_t sig = {.src = src, .uid = uid, .dst = dst};
    return prte_relm_get_msg(&sig);
}

static prte_relm_msg_t *find_msg(pmix_rank_t src, prte_relm_uid_t uid, pmix_rank_t dst)
{
    prte_relm_signature_t sig = {.src = src, .uid = uid, .dst = dst};
    return prte_relm_find_msg(&sig);
}

/* RELM activates a job state only to fail the job, so count them */
static int forced_exits = 0;
static void count_job_state(prte_job_t *jdata, prte_job_state_t state)
{
    PRTE_HIDE_UNUSED_PARAMS(jdata, state);
    forced_exits++;
}

/*
 * The UID counter is documented as wrapping, and the wrap has to land on a
 * UID the rest of the layer will accept.  The top three values of a uint32_t
 * are the UNKNOWN/NONE/INVALID sentinels -- above PRTE_RELM_UID_MAX -- and a
 * bare "next_uid++" handed them out like any other, whereupon
 * prte_relm_get_msg() refuses the signature and the message that was being
 * started has nowhere to live.  Three messages out of every 2^32.
 */
static int test_uid_wrap(void)
{
    int failures = 0;

    sm_reset();

    CHECK("the counter starts at zero", 0 == prte_relm_next_uid());
    CHECK("...and then counts up", 1 == prte_relm_next_uid());
    CHECK("...one at a time", 2 == prte_relm_next_uid());

    /* park the counter just short of the top of the usable range and walk it
     * over the wrap */
    prte_relm_sm->next_uid = PRTE_RELM_UID_MAX - 1;
    CHECK("the last two usable UIDs are handed out",
          PRTE_RELM_UID_MAX - 1 == prte_relm_next_uid());
    CHECK("...both of them", PRTE_RELM_UID_MAX == prte_relm_next_uid());
    CHECK("and then the counter wraps to zero rather than onto a sentinel",
          0 == prte_relm_next_uid());
    CHECK("...and keeps counting from there", 1 == prte_relm_next_uid());

    /* the property that actually matters: whatever the counter is at, it
     * never yields a UID prte_relm_get_msg() will refuse */
    prte_relm_sm->next_uid = PRTE_RELM_UID_MAX - 3;
    for (int i = 0; i < 8; i++) {
        prte_relm_uid_t uid = prte_relm_next_uid();
        CHECK("every UID handed out is usable", uid <= PRTE_RELM_UID_MAX);
        CHECK("...and names a message", NULL != get_msg(0, uid, 1));
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_uid_wrap\n");
    }
    return failures;
}

/*
 * <src,uid> is the message's global identity and dst completes the signature.
 * The GUID packs src into the high half and uid into the low half, so the
 * pairs must not alias each other.
 */
static int test_signature_identity(void)
{
    int failures = 0;

    sm_reset();

    prte_relm_msg_t *a = get_msg(1, 2, 3);
    prte_relm_msg_t *b = get_msg(2, 1, 3);
    CHECK("a message is created for a fresh signature", NULL != a);
    CHECK("...and for its transposed twin", NULL != b);
    CHECK("<1,2> and <2,1> are different messages", a != b);
    CHECK("...because their GUIDs differ", PRTE_RELM_GUID(a) != PRTE_RELM_GUID(b));

    CHECK("a fresh message starts INVALID", PRTE_RELM_STATE_INVALID == a->state);
    CHECK("...with no data", NULL == a->data.bytes);
    CHECK("...and knows its own signature",
          1 == a->src && 2 == a->uid && 3 == a->dst);

    CHECK("getting the same signature twice finds the same message",
          a == get_msg(1, 2, 3));
    CHECK("...and find agrees", a == find_msg(1, 2, 3));

    /* the same <src,uid> to a different destination is a different message,
     * and lives in a different rank's table */
    prte_relm_msg_t *c = get_msg(1, 2, 4);
    CHECK("the same <src,uid> to another dst is a separate message", a != c);
    CHECK("...and does not shadow the first", a == find_msg(1, 2, 3));

    CHECK("find answers NULL for a signature nobody created",
          NULL == find_msg(1, 5, 3));

    fprintf(stdout, "-- the next cases drive rejected signatures;"
                    " the errors they print are expected --\n");
    fflush(stdout);
    CHECK("a src outside the DVM is refused", NULL == get_msg(NDMNS, 1, 2));
    CHECK("a dst outside the DVM is refused", NULL == get_msg(1, 1, NDMNS));
    CHECK("a UID above the max is refused",
          NULL == get_msg(1, PRTE_RELM_UID_INVALID, 2));
    CHECK("...for every sentinel", NULL == get_msg(1, PRTE_RELM_UID_NONE, 2));
    CHECK("...including UNKNOWN", NULL == get_msg(1, PRTE_RELM_UID_UNKNOWN, 2));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_signature_identity\n");
    }
    return failures;
}

/*
 * Messages to one destination are chained by prev_uid/next_uid so ordering is
 * preserved and an ACK implicitly acks everything before it.  The chain is
 * per <src,dst>: walking it must not stray into another destination's
 * messages, and it must terminate on the sentinels rather than walking off
 * the end.
 */
static int test_ordering_chain(void)
{
    int failures = 0;

    sm_reset();

    prte_relm_msg_t *first = get_msg(1, 10, 3);
    prte_relm_msg_t *second = get_msg(1, 11, 3);
    prte_relm_msg_t *third = get_msg(1, 12, 3);
    if (NULL == first || NULL == second || NULL == third) {
        fprintf(stderr, "FAIL [ordering]: could not create the chain\n");
        return failures + 1;
    }
    second->prev_uid = first->uid;
    first->next_uid = second->uid;
    third->prev_uid = second->uid;
    second->next_uid = third->uid;

    CHECK("the chain walks back", first == prte_relm_find_prev_msg(second));
    CHECK("...and forward", third == prte_relm_find_next_msg(second));

    CHECK("the head has no predecessor", NULL == prte_relm_find_prev_msg(first));
    CHECK("the tail has no successor", NULL == prte_relm_find_next_msg(third));

    /* a UID that was never created is not a predecessor, even though it is
     * inside the usable range -- find must answer NULL rather than inventing
     * one, which is what get is for */
    third->prev_uid = 99;
    CHECK("an absent predecessor is not found", NULL == prte_relm_find_prev_msg(third));
    prte_relm_msg_t *made = prte_relm_get_prev_msg(third);
    CHECK("...but get creates it", NULL != made);
    CHECK("...under this message's own src and dst",
          NULL != made && 1 == made->src && 3 == made->dst && 99 == made->uid);

    /* the chain is per-destination: the same UIDs to another dst are a
     * separate chain */
    prte_relm_msg_t *other = get_msg(1, 11, 4);
    CHECK("another dst gets its own message", NULL != other && other != second);
    other->prev_uid = 10;
    CHECK("...and its chain does not reach into dst 3's",
          NULL == prte_relm_find_prev_msg(other));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_ordering_chain\n");
    }
    return failures;
}

/*
 * Releasing a message takes its predecessors with it -- they are implicitly
 * acked -- and unhooks its successor so nothing is left pointing at freed
 * memory.
 */
static int test_release_chain(void)
{
    int failures = 0;

    sm_reset();

    prte_relm_msg_t *first = get_msg(1, 20, 3);
    prte_relm_msg_t *second = get_msg(1, 21, 3);
    prte_relm_msg_t *third = get_msg(1, 22, 3);
    if (NULL == first || NULL == second || NULL == third) {
        fprintf(stderr, "FAIL [release]: could not create the chain\n");
        return failures + 1;
    }
    second->prev_uid = first->uid;
    first->next_uid = second->uid;
    third->prev_uid = second->uid;
    second->next_uid = third->uid;

    prte_relm_release_msg(second);

    CHECK("the released message is gone", NULL == find_msg(1, 21, 3));
    CHECK("...and so is its predecessor", NULL == find_msg(1, 20, 3));
    CHECK("its successor survives", third == find_msg(1, 22, 3));
    CHECK("...no longer pointing at freed memory",
          PRTE_RELM_UID_NONE == third->prev_uid);

    /* releasing the last message for a destination drops the destination's
     * table too */
    prte_relm_release_msg(third);
    CHECK("an emptied destination is dropped", NULL == prte_relm_find_rank(3));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_release_chain\n");
    }
    return failures;
}

/*
 * Stand the routing tree up from rank me's point of view.  At radix 2 over 8
 * daemons the tree is 0 -> {1,2}, 1 -> {3,5}, 2 -> {4,6}, 3 -> {7}, so a
 * message from 0 to 3 travels 0 -> 1 -> 3.
 */
static void build_tree(pmix_rank_t me)
{
    prte_rml_base.radix = 2;
    prte_process_info.num_daemons = NDMNS;
    PRTE_PROC_MY_NAME->rank = me;
    prte_rml_compute_routing_tree();
    sm_reset();
    forced_exits = 0;
}

/* Give msg a payload framed the way prte_relm_start_msg frames one, so that
 * posting it can unpack the tag and the bytes */
static void load_payload(prte_relm_msg_t *msg, prte_relm_uid_t uid)
{
    pmix_data_buffer_t *data = PMIx_Data_buffer_create();
    prte_rml_tag_t tag = PRTE_RML_TAG_RELM_STATE;
    pmix_byte_object_t bo;

    bo.bytes = (char *) &uid;
    bo.size = sizeof(uid);
    PMIx_Data_pack(NULL, data, &tag, 1, PRTE_RML_TAG);
    PMIx_Data_pack(NULL, data, &bo, 1, PMIX_BYTE_OBJECT);
    PMIx_Data_unload(data, &msg->data);
    PMIx_Data_buffer_release(data);
}

/*
 * A link update from a new upstream neighbour says SENT for every message it
 * has passed on.  A daemon with no record of one never received it: the only
 * copy died with the daemon that used to sit between them.  It has to ask for
 * a replay, and nobody else will -- below it nothing can have the message,
 * and a link update is only sent to links that changed.  The destination
 * used to reject the update as an error (a SENT is something it never does)
 * and an intermediate recorded SENT and waited, and either way the message
 * was never delivered.
 */
static int test_link_update_sent_requests_replay(void)
{
    int failures = 0;
    prte_relm_msg_t *msg;

    /* the destination, hearing from its new parent */
    build_tree(3);
    msg = get_msg(0, 5, 3);
    prte_relm_handle_state_update(NULL, msg, PRTE_RELM_STATE_SENT, 1);
    CHECK("the destination asks for a message it never received",
          PRTE_RELM_STATE_REQUESTED == msg->state);

    /* the destination already has it: the ACK may have died with the same
     * daemon, so it goes up again -- and the message stays where it is */
    msg = get_msg(0, 6, 3);
    msg->state = PRTE_RELM_STATE_ACKED;
    prte_relm_handle_state_update(NULL, msg, PRTE_RELM_STATE_SENT, 1);
    CHECK("a message already posted is left ACKED", PRTE_RELM_STATE_ACKED == msg->state);

    /* an intermediate daemon, hearing from its new parent */
    build_tree(1);
    msg = get_msg(0, 7, 3);
    prte_relm_handle_state_update(NULL, msg, PRTE_RELM_STATE_SENT, 0);
    CHECK("an intermediate asks for a message it never received",
          PRTE_RELM_STATE_REQUESTED == msg->state);

    CHECK("...and none of it fails the job", 0 == forced_exits);

    /* A request for a message whose source has died is addressed to a rank
     * that cannot answer, and the route toward it leads to that dead rank or
     * to none.  Every daemon purges such a message once it hears of the
     * death, but a daemon that has not heard yet can still ask a neighbour
     * that already has -- and refusing that send used to fail the job. */
    build_tree(3);
    msg = get_msg(1, 8, 3);
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 1);
    prte_relm_update_state(msg, PRTE_RELM_STATE_REQUESTED);
    pmix_bitmap_clear_bit(&prte_rml_base.failed_dmns, 1);
    CHECK("asking a dead source for a replay does not fail the job", 0 == forced_exits);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_link_update_sent_requests_replay\n");
    }
    return failures;
}

/* Pack one state update the way a neighbour's link update carries it */
static void pack_update(pmix_data_buffer_t *buf, pmix_rank_t src, prte_relm_uid_t uid,
                        pmix_rank_t dst, prte_relm_state_t state)
{
    prte_relm_msg_t *msg = PMIX_NEW(prte_relm_msg_t);

    msg->src = src;
    msg->uid = uid;
    msg->dst = dst;
    msg->prev_uid = PRTE_RELM_UID_NONE;
    msg->state = state;
    if (PRTE_RELM_STATE_SENDING == state) {
        load_payload(msg, uid);
    }
    prte_relm_pack_state_update(buf, msg);
    PMIX_RELEASE(msg);
}

/*
 * An update from a rank that is no longer a link on the message's path is
 * ignored -- but it used to be ignored only after the message had been looked
 * up, and the lookup creates a message it has no record of.  Every lingering
 * update left an INVALID message (and its predecessor) in the table that
 * nothing ever released.  The same goes for an update about a message whose
 * source has died, which the fault handler has already purged.  Skipping one
 * must also consume all of it, data included, or the next update in the same
 * link update is read from the wrong place.
 */
static int test_lingering_updates_create_nothing(void)
{
    int failures = 0;
    pmix_data_buffer_t *buf;
    prte_relm_msg_t *msg;

    /* rank 3: parent 1, child 7 */
    build_tree(3);
    buf = PMIx_Data_buffer_create();
    /* a message from our child 7 to us has no business arriving from our
     * parent - and it carries data, to prove the skip consumes it */
    pack_update(buf, 7, 40, 3, PRTE_RELM_STATE_SENDING);
    /* a message from 5, which the parent is the way to, but 5 has died */
    pack_update(buf, 5, 41, 3, PRTE_RELM_STATE_SENT);
    /* and one the parent really is upstream of */
    pack_update(buf, 0, 42, 3, PRTE_RELM_STATE_SENT);

    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 5);
    prte_relm_message_handler(1, buf);
    prte_relm_message_handler(1, buf);
    prte_relm_message_handler(1, buf);
    pmix_bitmap_clear_bit(&prte_rml_base.failed_dmns, 5);

    CHECK("an update from a link off the path creates no message",
          NULL == find_msg(7, 40, 3));
    CHECK("...nor does one for a dead source", NULL == find_msg(5, 41, 3));
    msg = find_msg(0, 42, 3);
    CHECK("the update after them is read in step and acted on",
          NULL != msg && PRTE_RELM_STATE_REQUESTED == msg->state);
    CHECK("...and the buffer is used up exactly",
          buf->unpack_ptr == buf->base_ptr + buf->bytes_used);
    CHECK("...with nothing failing the job", 0 == forced_exits);
    PMIx_Data_buffer_release(buf);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_lingering_updates_create_nothing\n");
    }
    return failures;
}

/*
 * ACKACKED is an ephemeral state, but prte_relm_send_state_downstream packs
 * whatever msg->state holds, so it is stored for the length of that call.  It
 * used to be left there, on a message that can outlive its release: a send
 * still in flight holds a reference, and a cached message is evicted from its
 * destructor through prte_relm_update_state -- which refuses a message in an
 * ephemeral state and fails the job, leaving the freed message on the cache
 * list.
 */
static int test_ackacked_leaves_no_ephemeral_state(void)
{
    int failures = 0;
    prte_relm_msg_t *msg;

    /* rank 1, between the source 0 and the destination 3 */
    build_tree(1);
    msg = get_msg(0, 9, 3);
    load_payload(msg, 9);
    msg->state = PRTE_RELM_STATE_SENT;
    prte_relm_update_state(msg, PRTE_RELM_STATE_CACHED);
    CHECK("the forwarded message is cached", msg->cached);
    /* ACKED without the eviction a local ACK performs, as a link update from
     * upstream leaves it */
    prte_relm_handle_state_update(NULL, msg, PRTE_RELM_STATE_ACKED, 0);
    CHECK("...and still cached once upstream says ACKED", msg->cached);

    /* stand in for a send that has not completed */
    PMIX_RETAIN(msg);
    prte_relm_handle_state_update(NULL, msg, PRTE_RELM_STATE_ACKACKED, 0);
    CHECK("the ACKACK releases the message", NULL == find_msg(0, 9, 3));
    CHECK("...leaving no ephemeral state on what survives",
          PRTE_RELM_EPHEMERAL_STATES_START > msg->state);

    PMIX_RELEASE(msg);
    CHECK("the last release evicts it from the cache",
          0 == pmix_list_get_size(&prte_relm_sm->cached_messages));
    CHECK("...and nothing failed the job", 0 == forced_exits);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_ackacked_leaves_no_ephemeral_state\n");
    }
    return failures;
}

/*
 * Messages that arrive ahead of a lost predecessor wait PENDING, and its
 * replay releases the whole backlog at once.  That drain used to recurse --
 * posting each message ACKed it, and the ACK posted the next -- several stack
 * frames per message, so the stack a replay needed grew with the backlog.
 * The count here is well past what that recursion survives on an 8 MB stack.
 */
#define BACKLOG 50000

static int test_pending_backlog_drains(void)
{
    int failures = 0;
    prte_relm_msg_t *msg, *head;
    prte_relm_uid_t uid;
    bool all_acked = true;

    /* the destination */
    build_tree(3);
    head = get_msg(0, 0, 3);
    if (NULL == head) {
        fprintf(stderr, "FAIL [backlog]: could not create the head\n");
        return failures + 1;
    }
    head->prev_uid = PRTE_RELM_UID_NONE;
    load_payload(head, 0);

    for (uid = 1; uid < BACKLOG; uid++) {
        msg = get_msg(0, uid, 3);
        if (NULL == msg) {
            fprintf(stderr, "FAIL [backlog]: could not create message %u\n", uid);
            return failures + 1;
        }
        msg->prev_uid = uid - 1;
        find_msg(0, uid - 1, 3)->next_uid = uid;
        load_payload(msg, uid);
        msg->state = PRTE_RELM_STATE_PENDING;
    }

    /* the replay of the head arrives */
    prte_relm_handle_state_update(NULL, head, PRTE_RELM_STATE_SENDING, 3);

    for (uid = 0; uid < BACKLOG; uid++) {
        msg = find_msg(0, uid, 3);
        if (NULL == msg || PRTE_RELM_STATE_ACKED != msg->state) {
            all_acked = false;
            break;
        }
    }
    CHECK("every message behind the replay is posted and ACKED", all_acked);
    CHECK("...and nothing failed the job", 0 == forced_exits);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_pending_backlog_drains\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;
    pmix_status_t prc;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }
    /* the protocol cases pack, and PMIx_Data_pack refuses to run until PMIx
     * itself is up.  A daemon reaches that state through PMIx_server_init, so
     * do the same */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(prc));
        prte_finalize();
        return 1;
    }
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_event_base_open failed: %d\n", rc);
        return 1;
    }
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-relm-unit-test");
    PRTE_PROC_MY_NAME->rank = 0;
    prte_rml_base.radix = 64;
    prte_rml_base.n_dmns = NDMNS;
    prte_state.activate_job_state = count_job_state;

    failures += test_uid_wrap();
    failures += test_signature_identity();
    failures += test_ordering_chain();
    failures += test_release_chain();

    /* the protocol cases route, so they need the failure bitmaps
     * prte_rml_open() would construct */
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    PMIX_CONSTRUCT(&prte_rml_base.global_failed_dmns, pmix_bitmap_t);
    PMIX_CONSTRUCT(&prte_rml_base.dead_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.dead_dmns, 64);
    PMIX_CONSTRUCT(&prte_rml_base.absent_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.absent_dmns, 64);

    failures += test_link_update_sent_requests_replay();
    failures += test_lingering_updates_create_nothing();
    failures += test_ackacked_leaves_no_ephemeral_state();
    failures += test_pending_backlog_drains();

    PMIX_RELEASE(prte_relm_sm);
    prte_relm_sm = NULL;
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
    PMIX_DESTRUCT(&prte_rml_base.global_failed_dmns);
    PMIX_DESTRUCT(&prte_rml_base.dead_dmns);
    PMIX_DESTRUCT(&prte_rml_base.absent_dmns);
    PMIx_Data_array_destruct(&prte_rml_base.ancestors);
    PMIx_Data_array_destruct(&prte_rml_base.children);
    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all relm unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d relm unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
