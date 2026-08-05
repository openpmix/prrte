/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the RML's routing tree, incarnation guard, message purge,
 * and contact-URI parsing.
 *
 * All of this is pure computation over `prte_rml_base` plus a handful of
 * process-info globals: no socket, no progress thread, no DVM.  The tests
 * therefore stand `prte_rml_base` up by hand -- exactly the fields
 * prte_rml_open() would set -- rather than calling prte_rml_open(), which
 * would also open listeners and the OOB.
 *
 * What is deliberately NOT covered here: prte_rml_repair_routing_tree() and
 * prte_rml_revive_routing_tree().  Both end by calling into the grpcomm,
 * filem, and relm fault handlers and by emitting adoption/failure notices
 * over the RML, none of which exist in a unit-test process.  Their effect on
 * the *tree* is reachable another way, and is tested here: a departed rank is
 * recorded in dead_dmns/absent_dmns, and prte_rml_compute_routing_tree()
 * restores those marks and rebuilds around them -- which is precisely the
 * property #2491 was about.  End-to-end fault recovery belongs to the
 * container harness (contrib/dockerswarm).
 *
 * The radix layout under test: rank 0 is the root at depth 0.  A node of rank
 * r at depth d has width W = radix^d and count = 1 + radix + ... + radix^d;
 * its children are r+W, r+2W, ..., r+(radix)W, and its subtree is every rank
 * >= r congruent to r modulo W.  Ranks are dense in [0, n_dmns).
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_bitmap.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
#include "src/util/proc_info.h"

#include "src/rml/radix.h"
#include "src/rml/rml.h"
#include "src/rml/rml_contact.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/*
 * Stand up prte_rml_base as a DVM of ndmns daemons with the given radix,
 * from the point of view of rank myrank, and compute the routing tree.
 * Any rank marked in dead_dmns/absent_dmns beforehand stays marked.
 */
static void build_dvm(int radix, pmix_rank_t ndmns, pmix_rank_t myrank)
{
    prte_rml_base.radix = radix;
    prte_process_info.num_daemons = ndmns;
    PRTE_PROC_MY_NAME->rank = myrank;
    prte_rml_compute_routing_tree();
}

/* construct the four failure bitmaps the way prte_rml_open() does */
static void bitmaps_construct(void)
{
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    PMIX_CONSTRUCT(&prte_rml_base.global_failed_dmns, pmix_bitmap_t);
    PMIX_CONSTRUCT(&prte_rml_base.dead_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.dead_dmns, 64);
    PMIX_CONSTRUCT(&prte_rml_base.absent_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.absent_dmns, 64);
    PMIX_CONSTRUCT(&prte_rml_base.lateral_links, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.lateral_links, 64);
}

/*
 * Lateral links: peers a bandwidth-efficient collective talks to directly,
 * chosen for their position in the exchange rather than in the tree.
 *
 * The property under test is the one whose failure is expensive.  Losing a
 * routing-tree connection means the tree changed shape and must be repaired;
 * losing a lateral link means nothing of the sort, and repairing on the
 * strength of it would mark a live daemon failed and end every in-flight
 * collective in the DVM.  prte_rml_is_lateral_only() is the single test the
 * fault path uses to tell them apart, so it has to be right about the overlap
 * case too: an exchange partner may happen to BE our parent or one of our
 * children, and losing that connection is a genuine tree fault.
 */
static int test_lateral_links(void)
{
    int failures = 0;
    pmix_rank_t child, other;

    /* radix 2 over 15 daemons from rank 1: a real parent and real children,
     * and plenty of ranks that are neither */
    build_dvm(2, 15, 1);

    CHECK("we have a lifeline to test against",
          PMIX_RANK_INVALID != prte_rml_base.lifeline);
    CHECK("we have children to test against", 0 < prte_rml_base.n_children);
    child = ((pmix_rank_t *) prte_rml_base.children.array)[0];

    /* pick a rank that is neither our parent nor any of our children */
    for (other = 0; other < 15; other++) {
        if (other == PRTE_PROC_MY_NAME->rank || other == prte_rml_base.lifeline) {
            continue;
        }
        pmix_rank_t idx = radix_subtree_index(&prte_rml_base.cur_node, other);
        if (idx < prte_rml_base.children.size
            && other == ((pmix_rank_t *) prte_rml_base.children.array)[idx]) {
            continue;
        }
        break;
    }
    CHECK("found a non-neighbour rank", other < 15);

    /* nothing is lateral until it is registered */
    CHECK("unregistered rank is not lateral", !prte_rml_is_lateral_only(other));
    CHECK("parent is not lateral", !prte_rml_is_lateral_only(prte_rml_base.lifeline));
    CHECK("child is not lateral", !prte_rml_is_lateral_only(child));

    /* a registered non-neighbour is lateral */
    prte_rml_lateral_register(other);
    CHECK("registered non-neighbour is lateral", prte_rml_is_lateral_only(other));

    /* being in the tree wins over being registered - losing that link is a
     * real fault and must take the repair path */
    prte_rml_lateral_register(prte_rml_base.lifeline);
    prte_rml_lateral_register(child);
    CHECK("registered parent is still NOT lateral-only",
          !prte_rml_is_lateral_only(prte_rml_base.lifeline));
    CHECK("registered child is still NOT lateral-only",
          !prte_rml_is_lateral_only(child));

    /* deregistration takes effect */
    prte_rml_lateral_deregister(other);
    CHECK("deregistered rank is no longer lateral", !prte_rml_is_lateral_only(other));

    /* nonsense ranks are answered, not dereferenced */
    CHECK("invalid rank is not lateral", !prte_rml_is_lateral_only(PMIX_RANK_INVALID));
    prte_rml_lateral_register(PMIX_RANK_INVALID);
    prte_rml_lateral_register(PRTE_PROC_MY_NAME->rank);
    CHECK("we are never lateral to ourselves",
          !prte_rml_is_lateral_only(PRTE_PROC_MY_NAME->rank));

    /* A grow recomputes the routing tree.  That must not dissolve the
     * exchange partners a collective is midway through talking to, which is
     * why lateral_links is not re-initialized by compute_routing_tree. */
    prte_rml_lateral_register(other);
    build_dvm(2, 31, 1);
    CHECK("lateral registration survives a routing-tree recompute",
          pmix_bitmap_is_set_bit(&prte_rml_base.lateral_links, other));

    prte_rml_lateral_deregister(other);
    prte_rml_lateral_deregister(child);
    prte_rml_lateral_deregister(prte_rml_base.lifeline);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_lateral_links\n");
    }
    return failures;
}

static void bitmaps_destruct(void)
{
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
    PMIX_DESTRUCT(&prte_rml_base.global_failed_dmns);
    PMIX_DESTRUCT(&prte_rml_base.dead_dmns);
    PMIX_DESTRUCT(&prte_rml_base.absent_dmns);
    PMIX_DESTRUCT(&prte_rml_base.lateral_links);
}

/* forget every failure mark between cases */
static void bitmaps_reset(void)
{
    bitmaps_destruct();
    bitmaps_construct();
}

/*
 * Every rank except the root is the child of exactly one node, radix_parent()
 * inverts radix_child(), and a node's subtree is exactly the set of ranks its
 * children's subtrees cover plus itself.
 */
static int check_shape(int radix, pmix_rank_t ndmns)
{
    int failures = 0;
    pmix_rank_t r, c;
    unsigned char *seen = calloc(ndmns, 1);
    radix_node_t node, child;
    char label[128];

    build_dvm(radix, ndmns, 0);

    for (r = 0; r < ndmns; r++) {
        node = radix_node(r);
        snprintf(label, sizeof(label), "radix %d/%u: rank %u is its own node",
                 radix, ndmns, r);
        CHECK(label, node.rank == r);
        CHECK(label, radix_subtree_contains(&node, r));

        RADIX_CHILD_FOREACH(node, child)
        {
            c = child.rank;
            snprintf(label, sizeof(label), "radix %d/%u: child %u of %u",
                     radix, ndmns, c, r);
            CHECK(label, c < ndmns);
            if (c >= ndmns) {
                continue;
            }
            /* nobody else already claimed it */
            CHECK(label, 0 == seen[c]);
            seen[c] = 1;
            /* the inverse holds */
            CHECK(label, radix_parent(&child).rank == r);
            /* and it is in this node's subtree, at this child index */
            CHECK(label, radix_subtree_contains(&node, c));
            CHECK(label, radix_subtree_index(&node, c) < (pmix_rank_t) radix);
        }
    }

    snprintf(label, sizeof(label), "radix %d/%u: root has no parent", radix, ndmns);
    CHECK(label, 0 == seen[0]);
    for (r = 1; r < ndmns; r++) {
        snprintf(label, sizeof(label), "radix %d/%u: rank %u has a parent",
                 radix, ndmns, r);
        CHECK(label, 1 == seen[r]);
    }

    free(seen);
    return failures;
}

static int test_radix_shape(void)
{
    int failures = 0;
    static const int radices[] = {2, 3, 4, 8, 64};
    static const pmix_rank_t sizes[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17,
                                        63, 64, 65, 100, 128, 129};
    size_t i, j;

    for (i = 0; i < sizeof(radices) / sizeof(radices[0]); i++) {
        for (j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
            failures += check_shape(radices[i], sizes[j]);
        }
    }
    bitmaps_reset();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_radix_shape\n");
    }
    return failures;
}

/*
 * radix_to_next() walks the whole tree depth-first, so from the root it must
 * reach every rank in [0, n_dmns) exactly once before reporting INVALID.
 *
 * This is the regression for the boundary bug: the walk stepped in from a
 * node's rightmost child slot while the candidate was "> n_dmns" rather than
 * ">= n_dmns", so whenever a node's rightmost slot landed exactly on n_dmns
 * the walk stopped on the non-existent rank n_dmns.  radix_to_next_living()
 * then gave up, and every rank below that node vanished from the repaired
 * tree -- update_ancestors and the child replacement in update_descendants
 * both rely on this walk to find a dead node's inheritor.  With the default
 * radix of 64 that is the *entire* DVM at exactly 64 daemons: rank 0's
 * rightmost child slot is 0 + 1*64 = 64.
 */
static int check_traversal(int radix, pmix_rank_t ndmns)
{
    int failures = 0;
    unsigned char *seen = calloc(ndmns, 1);
    pmix_rank_t visited = 0;
    radix_node_t node;
    char label[128];

    build_dvm(radix, ndmns, 0);

    node = radix_node(0);
    while (PMIX_RANK_INVALID != node.rank) {
        snprintf(label, sizeof(label), "radix %d/%u: walk stays in range (at %u)",
                 radix, ndmns, node.rank);
        CHECK(label, node.rank < ndmns);
        if (node.rank >= ndmns) {
            break;
        }
        CHECK(label, 0 == seen[node.rank]);
        seen[node.rank] = 1;
        visited++;
        /* a tree of n_dmns ranks cannot take more than n_dmns steps */
        if (visited > ndmns) {
            break;
        }
        radix_to_next(&node);
    }

    snprintf(label, sizeof(label), "radix %d/%u: walk visited every rank", radix, ndmns);
    CHECK(label, visited == ndmns);

    free(seen);
    return failures;
}

static int test_radix_traversal(void)
{
    int failures = 0;
    static const int radices[] = {2, 3, 4, 8, 64};
    static const pmix_rank_t sizes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 15,
                                        16, 17, 63, 64, 65, 72, 100, 128, 129};
    size_t i, j;

    for (i = 0; i < sizeof(radices) / sizeof(radices[0]); i++) {
        for (j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
            failures += check_traversal(radices[i], sizes[j]);
        }
    }
    bitmaps_reset();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_radix_traversal\n");
    }
    return failures;
}

/*
 * The tree a daemon computes for itself: its ancestor chain ends at its
 * parent, its children are the radix children that exist, and get_route
 * answers "me" for me, "my parent" for anything outside my subtree, and the
 * right child for anything inside it.
 */
static int test_routing_tree(void)
{
    int failures = 0;
    pmix_rank_t r, target, hop;
    size_t i;

    /* radix 2, 7 daemons: 0 -> {1,2}, 1 -> {3,5}, 2 -> {4,6} */
    bitmaps_reset();
    build_dvm(2, 7, 0);
    CHECK("root has no ancestors", 0 == prte_rml_base.ancestors.size);
    CHECK("root has no lifeline", PMIX_RANK_INVALID == prte_rml_base.lifeline);
    CHECK("root has two children", 2 == prte_rml_base.n_children);
    CHECK("root routes to self", 0 == prte_rml_get_route(0));
    CHECK("root routes 1 via 1", 1 == prte_rml_get_route(1));
    CHECK("root routes 3 via 1", 1 == prte_rml_get_route(3));
    CHECK("root routes 5 via 1", 1 == prte_rml_get_route(5));
    CHECK("root routes 2 via 2", 2 == prte_rml_get_route(2));
    CHECK("root routes 4 via 2", 2 == prte_rml_get_route(4));
    CHECK("root routes 6 via 2", 2 == prte_rml_get_route(6));

    build_dvm(2, 7, 1);
    CHECK("rank 1 has one ancestor", 1 == prte_rml_base.ancestors.size);
    CHECK("rank 1's parent is the root", 0 == prte_rml_base.lifeline);
    CHECK("rank 1 has two children", 2 == prte_rml_base.n_children);
    CHECK("rank 1 routes 3 down", 3 == prte_rml_get_route(3));
    CHECK("rank 1 routes 5 down", 5 == prte_rml_get_route(5));
    CHECK("rank 1 routes 2 up", 0 == prte_rml_get_route(2));
    CHECK("rank 1 routes 4 up", 0 == prte_rml_get_route(4));
    CHECK("rank 1 routes the root up", 0 == prte_rml_get_route(0));

    build_dvm(2, 7, 3);
    CHECK("rank 3 has two ancestors", 2 == prte_rml_base.ancestors.size);
    CHECK("rank 3's ancestors are 0 then 1",
          0 == ((pmix_rank_t *) prte_rml_base.ancestors.array)[0]
              && 1 == ((pmix_rank_t *) prte_rml_base.ancestors.array)[1]);
    CHECK("rank 3's parent is 1", 1 == prte_rml_base.lifeline);
    CHECK("rank 3 is a leaf", 0 == prte_rml_base.n_children);
    for (target = 0; target < 7; target++) {
        if (3 == target) {
            continue;
        }
        CHECK("a leaf routes everything through its parent",
              1 == prte_rml_get_route(target));
    }

    /* every daemon in a larger DVM agrees on who its parent is, and every
     * rank is reachable from every rank in at most a few hops */
    bitmaps_reset();
    for (r = 0; r < 40; r++) {
        build_dvm(4, 40, r);
        for (i = 0; i < prte_rml_base.children.size; i++) {
            pmix_rank_t c = ((pmix_rank_t *) prte_rml_base.children.array)[i];
            if (PMIX_RANK_INVALID == c) {
                continue;
            }
            CHECK("a child is in range", c < 40);
            CHECK("a child routes directly", c == prte_rml_get_route(c));
        }
        for (target = 0; target < 40; target++) {
            hop = prte_rml_get_route(target);
            CHECK("every target has a next hop", PMIX_RANK_INVALID != hop);
            CHECK("the next hop is in range", hop < 40);
        }
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_routing_tree\n");
    }
    return failures;
}

/*
 * Recomputing the tree -- which is what a DVM grow does -- must not resurrect
 * a rank that has permanently departed.  prte_rml_repair_routing_tree()
 * records departures in dead_dmns (a launched/elastic DVM) or absent_dmns (a
 * bootstrapped one whose node may reboot); both survive the recompute and are
 * restored into the freshly-wiped failed set.  Without that the rebuilt tree
 * routes to a dead vpid and wireup on the grow breaks (#2491).
 */
static int test_departed_ranks_survive_recompute(void)
{
    int failures = 0;
    pmix_rank_t hop;

    /* radix 2, 7 daemons, we are the root; rank 1 has departed */
    bitmaps_reset();
    build_dvm(2, 7, 0);
    CHECK("rank 1 starts out up", prte_rml_is_node_up(1));
    pmix_bitmap_set_bit(&prte_rml_base.dead_dmns, 1);

    /* the grow: more daemons, tree recomputed from scratch */
    build_dvm(2, 9, 0);
    CHECK("a dead rank is restored into the failed set",
          pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, 1));
    CHECK("a dead rank is not up", !prte_rml_is_node_up(1));
    CHECK("no child of the root is the dead rank",
          1 != ((pmix_rank_t *) prte_rml_base.children.array)[0]);
    hop = prte_rml_get_route(3);
    CHECK("traffic for the dead rank's subtree does not go through it", 1 != hop);
    CHECK("traffic for the dead rank's subtree still has a hop",
          PMIX_RANK_INVALID != hop);

    /* absent (bootstrap) ranks are restored the same way... */
    bitmaps_reset();
    build_dvm(2, 7, 0);
    pmix_bitmap_set_bit(&prte_rml_base.absent_dmns, 2);
    build_dvm(2, 9, 0);
    CHECK("an absent rank is restored into the failed set",
          pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, 2));
    CHECK("an absent rank is not up", !prte_rml_is_node_up(2));

    /* ...but unlike dead ranks they can be cleared, and the next recompute
     * then has the rank back in the tree */
    pmix_bitmap_clear_bit(&prte_rml_base.absent_dmns, 2);
    build_dvm(2, 9, 0);
    CHECK("a cleared absent rank is live again", prte_rml_is_node_up(2));
    CHECK("a cleared absent rank is routable", 2 == prte_rml_get_route(2));

    bitmaps_reset();
    if (0 == failures) {
        fprintf(stdout, "PASSED test_departed_ranks_survive_recompute\n");
    }
    return failures;
}

/*
 * get_num_contributors counts how many of my child subtrees a set of daemons
 * falls into -- what a collective uses to know how many reports to expect.
 * Failed ranks contribute nothing.
 */
static int test_num_contributors(void)
{
    int failures = 0;
    pmix_rank_t set[4];

    /* radix 2, 7 daemons, we are the root: children 1 and 2 */
    bitmaps_reset();
    build_dvm(2, 7, 0);

    set[0] = 3;
    CHECK("one rank in one subtree", 1 == prte_rml_get_num_contributors(set, 1));
    set[1] = 5; /* also under child 1 */
    CHECK("two ranks in the same subtree", 1 == prte_rml_get_num_contributors(set, 2));
    set[2] = 4; /* under child 2 */
    CHECK("a second subtree counts once more", 2 == prte_rml_get_num_contributors(set, 3));
    set[3] = 6; /* also under child 2 */
    CHECK("still two subtrees", 2 == prte_rml_get_num_contributors(set, 4));

    /* a failed rank drops out of the count */
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 4);
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 6);
    CHECK("failed ranks do not contribute", 1 == prte_rml_get_num_contributors(set, 4));

    bitmaps_reset();
    if (0 == failures) {
        fprintf(stdout, "PASSED test_num_contributors\n");
    }
    return failures;
}

/*
 * The incarnation guard.  A bootstrap daemon that reboots into the same rank
 * comes back with a strictly greater boot epoch, so traffic stamped with an
 * older epoch for that rank is from the stale incarnation and is dropped.
 */
static int test_epoch_guard(void)
{
    int failures = 0;

    CHECK("an unstamped message is always accepted", prte_rml_epoch_ok(3, 0));
    CHECK("an unknown rank starts at epoch 0", 0 == prte_rml_get_epoch(3));

    /* first sighting is learned, not judged */
    CHECK("the first epoch seen is accepted", prte_rml_epoch_ok(3, 1000));
    CHECK("...and recorded", 1000 == prte_rml_get_epoch(3));

    CHECK("the same epoch is still accepted", prte_rml_epoch_ok(3, 1000));
    CHECK("an older epoch is rejected", !prte_rml_epoch_ok(3, 999));
    CHECK("a rejection does not disturb the record", 1000 == prte_rml_get_epoch(3));

    /* a newer epoch passes but does not advance the table on its own -- only
     * the arbitrated revival is allowed to do that */
    CHECK("a newer epoch is accepted", prte_rml_epoch_ok(3, 2000));
    CHECK("...without advancing the record", 1000 == prte_rml_get_epoch(3));
    prte_rml_record_epoch(3, 2000);
    CHECK("recording advances it", 2000 == prte_rml_get_epoch(3));
    CHECK("the previously-current epoch is now stale", !prte_rml_epoch_ok(3, 1000));

    /* recording epoch 0 means "unknown" and must not erase what we know */
    prte_rml_record_epoch(3, 0);
    CHECK("recording 0 is ignored", 2000 == prte_rml_get_epoch(3));

    /* ranks are independent, and the table grows on demand */
    CHECK("an unrelated rank is unaffected", 0 == prte_rml_get_epoch(4));
    CHECK("a far rank can be recorded", (prte_rml_record_epoch(400, 77), true));
    CHECK("...and read back", 77 == prte_rml_get_epoch(400));
    CHECK("a far rank rejects a stale epoch", !prte_rml_epoch_ok(400, 76));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_epoch_guard\n");
    }
    return failures;
}

/* count how many entries a list holds */
static size_t list_len(pmix_list_t *l)
{
    return pmix_list_get_size(l);
}

/*
 * prte_rml_purge drops the posted recvs and the held (unmatched) messages
 * belonging to a departed peer, and leaves everyone else's alone.  The
 * held-message half is the regression: it compared the two pmix_proc_t
 * nspace *arrays* with "!=", which is a comparison of their addresses and so
 * was always true -- every held message survived a purge, and a message from
 * a dead daemon stayed on the list for the life of the DVM.
 */
static int test_purge(void)
{
    int failures = 0;
    prte_rml_posted_recv_t *post;
    prte_rml_recv_t *msg;
    pmix_proc_t gone;

    PMIX_CONSTRUCT(&prte_rml_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.unmatched_msgs, pmix_list_t);

    PMIX_LOAD_PROCID(&gone, PRTE_PROC_MY_NAME->nspace, 7);

    /* two recvs for the departing peer, one for a survivor, one wildcard */
    post = PMIX_NEW(prte_rml_posted_recv_t);
    PMIX_LOAD_PROCID(&post->peer, PRTE_PROC_MY_NAME->nspace, 7);
    post->tag = PRTE_RML_TAG_DAEMON;
    pmix_list_append(&prte_rml_base.posted_recvs, &post->super);

    post = PMIX_NEW(prte_rml_posted_recv_t);
    PMIX_LOAD_PROCID(&post->peer, PRTE_PROC_MY_NAME->nspace, 7);
    post->tag = PRTE_RML_TAG_PLM;
    pmix_list_append(&prte_rml_base.posted_recvs, &post->super);

    post = PMIX_NEW(prte_rml_posted_recv_t);
    PMIX_LOAD_PROCID(&post->peer, PRTE_PROC_MY_NAME->nspace, 8);
    post->tag = PRTE_RML_TAG_DAEMON;
    pmix_list_append(&prte_rml_base.posted_recvs, &post->super);

    post = PMIX_NEW(prte_rml_posted_recv_t);
    PMIX_XFER_PROCID(&post->peer, PRTE_NAME_WILDCARD);
    post->tag = PRTE_RML_TAG_DAEMON;
    pmix_list_append(&prte_rml_base.posted_recvs, &post->super);

    /* two held messages from the departing peer, one from a survivor */
    msg = PMIX_NEW(prte_rml_recv_t);
    PMIX_LOAD_PROCID(&msg->sender, PRTE_PROC_MY_NAME->nspace, 7);
    msg->tag = PRTE_RML_TAG_DAEMON;
    pmix_list_append(&prte_rml_base.unmatched_msgs, &msg->super);

    msg = PMIX_NEW(prte_rml_recv_t);
    PMIX_LOAD_PROCID(&msg->sender, PRTE_PROC_MY_NAME->nspace, 7);
    msg->tag = PRTE_RML_TAG_PLM;
    pmix_list_append(&prte_rml_base.unmatched_msgs, &msg->super);

    msg = PMIX_NEW(prte_rml_recv_t);
    PMIX_LOAD_PROCID(&msg->sender, PRTE_PROC_MY_NAME->nspace, 8);
    msg->tag = PRTE_RML_TAG_DAEMON;
    pmix_list_append(&prte_rml_base.unmatched_msgs, &msg->super);

    CHECK("four recvs posted", 4 == list_len(&prte_rml_base.posted_recvs));
    CHECK("three messages held", 3 == list_len(&prte_rml_base.unmatched_msgs));

    prte_rml_purge(&gone);

    CHECK("the departed peer's recvs are gone",
          2 == list_len(&prte_rml_base.posted_recvs));
    CHECK("the departed peer's held messages are gone",
          1 == list_len(&prte_rml_base.unmatched_msgs));

    PMIX_LIST_FOREACH(post, &prte_rml_base.posted_recvs, prte_rml_posted_recv_t)
    {
        CHECK("no surviving recv names the departed peer", 7 != post->peer.rank);
    }
    PMIX_LIST_FOREACH(msg, &prte_rml_base.unmatched_msgs, prte_rml_recv_t)
    {
        CHECK("no surviving message is from the departed peer", 7 != msg->sender.rank);
    }

    /* purging a peer with nothing outstanding changes nothing */
    PMIX_LOAD_PROCID(&gone, PRTE_PROC_MY_NAME->nspace, 9);
    prte_rml_purge(&gone);
    CHECK("purging an unrelated peer left the recvs",
          2 == list_len(&prte_rml_base.posted_recvs));
    CHECK("purging an unrelated peer left the messages",
          1 == list_len(&prte_rml_base.unmatched_msgs));

    PMIX_LIST_DESTRUCT(&prte_rml_base.posted_recvs);
    PMIX_LIST_DESTRUCT(&prte_rml_base.unmatched_msgs);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_purge\n");
    }
    return failures;
}

/*
 * prte_rml_parse_uris splits a contact URI -- "<name>;<addr>;<addr>..." --
 * into the peer's process name and the list of addresses.  This is what turns
 * the HNP URI a daemon was handed on its command line into something the OOB
 * can connect to, so a malformed one has to be rejected rather than half
 * parsed.
 */
static int test_parse_uris(void)
{
    int failures = 0;
    pmix_proc_t peer;
    char **uris = NULL;
    int rc;

    rc = prte_rml_parse_uris("prterun-node-1234@0.1;tcp://10.0.0.1:1024:24",
                             &peer, &uris);
    CHECK("a well-formed uri parses", PRTE_SUCCESS == rc);
    if (PRTE_SUCCESS == rc) {
        CHECK("the rank is recovered", 1 == peer.rank);
        CHECK("one address recovered", 1 == PMIx_Argv_count(uris));
        CHECK("the address is intact",
              NULL != uris && 0 == strcmp("tcp://10.0.0.1:1024:24", uris[0]));
        PMIx_Argv_free(uris);
        uris = NULL;
    }

    /* several addresses */
    rc = prte_rml_parse_uris("prterun-node-1234@0.2;tcp://10.0.0.1:1024:24;"
                             "tcp://192.168.1.1:1025:16",
                             &peer, &uris);
    CHECK("a multi-address uri parses", PRTE_SUCCESS == rc);
    if (PRTE_SUCCESS == rc) {
        CHECK("the rank is recovered", 2 == peer.rank);
        CHECK("both addresses recovered", 2 == PMIx_Argv_count(uris));
        PMIx_Argv_free(uris);
        uris = NULL;
    }

    /* the caller may not want the addresses at all -- rml_open() does this
     * just to learn the HNP's name */
    rc = prte_rml_parse_uris("prterun-node-1234@0.3;tcp://10.0.0.1:1024:24",
                             &peer, NULL);
    CHECK("a NULL address list is allowed", PRTE_SUCCESS == rc);
    CHECK("...and the name still comes back", 3 == peer.rank);

    /* no separator at all: there is no address section, so this is not a
     * contact uri */
    fprintf(stdout, "-- the next case drives a malformed uri;"
                    " the error it prints is expected --\n");
    rc = prte_rml_parse_uris("prterun-node-1234@0.4", &peer, &uris);
    CHECK("a uri with no address section is rejected", PRTE_SUCCESS != rc);
    CHECK("...and produces no address list", NULL == uris);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_parse_uris\n");
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
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-rml-unit-test");
    bitmaps_construct();

    failures += test_radix_shape();
    failures += test_radix_traversal();
    failures += test_routing_tree();
    failures += test_departed_ranks_survive_recompute();
    failures += test_num_contributors();
    failures += test_lateral_links();
    failures += test_epoch_guard();
    failures += test_purge();
    failures += test_parse_uris();

    bitmaps_destruct();
    PMIx_Data_array_destruct(&prte_rml_base.ancestors);
    PMIx_Data_array_destruct(&prte_rml_base.children);
    if (NULL != prte_rml_base.peer_epochs) {
        free(prte_rml_base.peer_epochs);
        prte_rml_base.peer_epochs = NULL;
        prte_rml_base.peer_epochs_size = 0;
    }
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rml routing unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rml routing unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
