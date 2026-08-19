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
 * The state machine is stood up by hand: PMIX_NEW plus the two constructor
 * callbacks the lookup helpers actually call.  prte_relm_base_module's init()
 * would also post two persistent RML receives, which needs an RML that is
 * open.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/rml/rml.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/types.h"
#include "src/rml/relm/base/state_machine.h"

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
    prte_relm_sm->new_rank = prte_relm_base_new_rank;
    prte_relm_sm->new_msg = prte_relm_base_new_msg;
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

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-relm-unit-test");
    PRTE_PROC_MY_NAME->rank = 0;
    prte_rml_base.radix = 64;
    prte_rml_base.n_dmns = NDMNS;

    failures += test_uid_wrap();
    failures += test_signature_identity();
    failures += test_ordering_chain();
    failures += test_release_chain();

    PMIX_RELEASE(prte_relm_sm);
    prte_relm_sm = NULL;
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all relm unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d relm unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
