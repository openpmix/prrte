/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the node re-size bookkeeping the mapper uses when a "-host node:N"
 * asks for more of a node than the node says it has.
 *
 * PRRTE may answer that only in an unmanaged allocation, and only for the
 * job being mapped: "--host node:8" states how many slots THAT job may take
 * on the node, while changing the allocation is what "--add-host" is for.
 * So the grown count has to come back, on a failed map as surely as on a
 * successful one - a job that could not map has no more claim on the extra
 * slots than one that did.  These two functions are the whole of that
 * contract, and neither needs a topology or a DVM to check.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"

int test_resize(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

int test_resize(void)
{
    int failures = 0;
    prte_node_t *n1, *n2;

    n1 = PMIX_NEW(prte_node_t);
    n1->name = strdup("node0");
    n1->slots = 4;
    n2 = PMIX_NEW(prte_node_t);
    n2->name = strdup("node1");
    n2->slots = 2;

    /* nothing recorded: restoring is a no-op, not a crash */
    prte_rmaps_base_restore_resized();
    CHECK("empty: node untouched", 4 == n1->slots);

    /* the ordinary case - two nodes grown for one map, both put back */
    prte_rmaps_base_record_resize(n1, n1->slots);
    n1->slots = 8;
    prte_rmaps_base_record_resize(n2, n2->slots);
    n2->slots = 6;
    CHECK("recorded: both listed",
          2 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();
    CHECK("restored: node0 back to 4", 4 == n1->slots);
    CHECK("restored: node1 back to 2", 2 == n2->slots);
    CHECK("restored: list emptied",
          0 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));

    /* a second app of the same job can grow the same node again. The value
     * to put back is the one it had before the FIRST growth - recording the
     * already-grown count would leave the node permanently enlarged, which
     * is the whole thing this is here to prevent. */
    prte_rmaps_base_record_resize(n1, n1->slots);
    n1->slots = 8;
    prte_rmaps_base_record_resize(n1, n1->slots);
    n1->slots = 12;
    CHECK("regrown: recorded once",
          1 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();
    CHECK("regrown: back to the original 4", 4 == n1->slots);

    /* the "slots were stated, not detected" flag goes back with the count.
     * Growing a node for a "-host node:N" sets it, and a later job reads it
     * to decide whether it may oversubscribe - so a job that only said how
     * many slots it wanted would otherwise leave the node refusing
     * oversubscription to every job that came after it. */
    PRTE_FLAG_UNSET(n1, PRTE_NODE_FLAG_SLOTS_GIVEN);
    prte_rmaps_base_record_resize(n1, n1->slots);
    n1->slots = 8;
    PRTE_FLAG_SET(n1, PRTE_NODE_FLAG_SLOTS_GIVEN);
    prte_rmaps_base_restore_resized();
    CHECK("restored: slots-given cleared again",
          !PRTE_FLAG_TEST(n1, PRTE_NODE_FLAG_SLOTS_GIVEN));

    /* and a node the allocation itself sized keeps the flag it came with */
    PRTE_FLAG_SET(n2, PRTE_NODE_FLAG_SLOTS_GIVEN);
    prte_rmaps_base_record_resize(n2, n2->slots);
    n2->slots = 6;
    prte_rmaps_base_restore_resized();
    CHECK("restored: slots-given kept",
          PRTE_FLAG_TEST(n2, PRTE_NODE_FLAG_SLOTS_GIVEN));

    PMIX_RELEASE(n1);
    PMIX_RELEASE(n2);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_resize\n");
    }
    return failures;
}
