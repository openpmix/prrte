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
 *
 * The same contract binds the other direction.  A hostfile "slots=N" smaller
 * than the node makes the node smaller for the job that named the hostfile,
 * and the nodes the mapper narrows are the node pool's own objects - so a
 * shrink that was not recorded left every later job in the DVM looking at a
 * node that had lost slots it never gave up, and the allocation could only
 * ever get smaller.  test_hostfile_cap() below covers that path.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/util/hostfile/hostfile.h"

int test_resize(void);
int test_hostfile_cap(void);

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

/* write a hostfile into the current directory; returns path or NULL */
static char *write_hostfile(const char *body, char *path, size_t pathlen)
{
    FILE *fp;

    snprintf(path, pathlen, "prte_test_resize_hostfile_%lu.txt",
             (unsigned long) getpid());
    fp = fopen(path, "w");
    if (NULL == fp) {
        return NULL;
    }
    fputs(body, fp);
    fclose(fp);
    return path;
}

static prte_node_t *one_node(pmix_list_t *nodes, const char *name, int32_t slots)
{
    prte_node_t *nd;

    nd = PMIX_NEW(prte_node_t);
    nd->name = strdup(name);
    nd->slots = slots;
    pmix_list_append(nodes, &nd->super);
    return nd;
}

int test_hostfile_cap(void)
{
    int failures = 0;
    int rc;
    char path[512];
    pmix_list_t nodes;
    prte_node_t *nd;

    if (NULL == write_hostfile("hostA slots=1\n", path, sizeof(path))) {
        fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
        return 1;
    }

    /* selecting the nodes a job will map onto: the smaller count applies to
     * the job, and is recorded so the node goes back to its own size */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = one_node(&nodes, "hostA", 4);
    rc = prte_util_filter_hostfile_nodes(&nodes, path, true);
    CHECK("cap: filter succeeded", PRTE_SUCCESS == rc);
    CHECK("cap: node capped for this job", 1 == nd->slots);
    CHECK("cap: the shrink was recorded",
          1 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    prte_rmaps_base_restore_resized();
    CHECK("cap: node back to its own size", 4 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);

    /* marking which nodes are to host a daemon (remove == false) is not a
     * map: it reads no slot count, and nothing puts back what it changes,
     * because it runs before the map that would have restored it */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = one_node(&nodes, "hostA", 4);
    rc = prte_util_filter_hostfile_nodes(&nodes, path, false);
    CHECK("mark: filter succeeded", PRTE_SUCCESS == rc);
    CHECK("mark: node left alone", 4 == nd->slots);
    CHECK("mark: nothing recorded",
          0 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    PMIX_LIST_DESTRUCT(&nodes);

    /* a count larger than the node is not a cap - a hostfile can only
     * subdivide an allocation, never enlarge it */
    if (NULL == write_hostfile("hostA slots=8\n", path, sizeof(path))) {
        fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = one_node(&nodes, "hostA", 4);
    rc = prte_util_filter_hostfile_nodes(&nodes, path, true);
    CHECK("grow: filter succeeded", PRTE_SUCCESS == rc);
    CHECK("grow: node not enlarged", 4 == nd->slots);
    CHECK("grow: nothing recorded",
          0 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
    PMIX_LIST_DESTRUCT(&nodes);

    /* a node named by position takes a count the same way a node named
     * outright does.  The filter used to resolve "+n<K>" and "+e" and drop
     * the "slots=" on the floor - the only reader of that count was the
     * ordered host list, which nothing called.  prte_hnp_is_allocated is
     * false here, so "+n0" is pool slot 1. */
    {
        bool made_pool = (NULL == prte_node_pool);
        prte_node_t *pool[3];
        int j;

        if (made_pool) {
            prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
            pmix_pointer_array_init(prte_node_pool, 8, INT_MAX, 8);
        }
        for (j = 0; j < 3; j++) {
            pool[j] = PMIX_NEW(prte_node_t);
            pool[j]->name = (0 == j) ? strdup("poolHNP") : (1 == j) ? strdup("poolX") : strdup("poolY");
            pool[j]->slots = 4;
            pool[j]->index = pmix_pointer_array_add(prte_node_pool, pool[j]);
        }

        if (NULL == write_hostfile("+n0 slots=1\n", path, sizeof(path))) {
            fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
            return failures + 1;
        }
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        PMIX_RETAIN(pool[1]);
        pmix_list_append(&nodes, &pool[1]->super);
        rc = prte_util_filter_hostfile_nodes(&nodes, path, true);
        CHECK("relative: filter succeeded", PRTE_SUCCESS == rc);
        CHECK("relative: node capped for this job", 1 == pool[1]->slots);
        CHECK("relative: the shrink was recorded",
              1 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
        prte_rmaps_base_restore_resized();
        CHECK("relative: node back to its own size", 4 == pool[1]->slots);
        PMIX_LIST_DESTRUCT(&nodes);

        if (NULL == write_hostfile("+e slots=2\n", path, sizeof(path))) {
            fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
            return failures + 1;
        }
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        PMIX_RETAIN(pool[1]);
        pmix_list_append(&nodes, &pool[1]->super);
        PMIX_RETAIN(pool[2]);
        pmix_list_append(&nodes, &pool[2]->super);
        rc = prte_util_filter_hostfile_nodes(&nodes, path, true);
        CHECK("empty: filter succeeded", PRTE_SUCCESS == rc);
        CHECK("empty: every node it took is capped",
              2 == pool[1]->slots && 2 == pool[2]->slots);
        CHECK("empty: both shrinks were recorded",
              2 == pmix_list_get_size(&prte_rmaps_base.resized_nodes));
        prte_rmaps_base_restore_resized();
        CHECK("empty: nodes back to their own size", 4 == pool[1]->slots && 4 == pool[2]->slots);
        PMIX_LIST_DESTRUCT(&nodes);

        /* the rest of the relative forms, as the filter resolves them */
        {
            static const struct {
                const char *body;
                int rc;
                const char *kept; /* NULL: nothing checked beyond rc */
            } rel[] = {
                {"+N0\n", PRTE_SUCCESS, "poolX"},        /* either case of the letter */
                {"+n1\n", PRTE_SUCCESS, "poolY"},
                {"+n9\n", PRTE_ERR_SILENT, NULL},        /* past the end of the pool */
                {"+n2147483647\n", PRTE_ERR_SILENT, NULL}, /* too large to step over the HNP */
                {"+x0\n", PRTE_ERR_SILENT, NULL},
                {"+e:1\n", PRTE_SUCCESS, "poolX"},       /* the first empty node */
                {"+e:9\n", PRTE_ERR_SILENT, NULL},       /* more empty nodes than there are */
            };
            size_t r;

            for (r = 0; r < sizeof(rel) / sizeof(rel[0]); r++) {
                if (NULL == write_hostfile(rel[r].body, path, sizeof(path))) {
                    fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
                    return failures + 1;
                }
                PMIX_CONSTRUCT(&nodes, pmix_list_t);
                PMIX_RETAIN(pool[1]);
                pmix_list_append(&nodes, &pool[1]->super);
                PMIX_RETAIN(pool[2]);
                pmix_list_append(&nodes, &pool[2]->super);
                rc = prte_util_filter_hostfile_nodes(&nodes, path, true);
                if (rel[r].rc != rc) {
                    fprintf(stderr, "FAIL [hostfile-cap]: \"%s\" returned %d, expected %d\n",
                            rel[r].body, rc, rel[r].rc);
                    failures++;
                } else if (NULL != rel[r].kept
                           && (1 != pmix_list_get_size(&nodes)
                               || 0 != strcmp(rel[r].kept,
                                              ((prte_node_t *) pmix_list_get_first(&nodes))->name))) {
                    fprintf(stderr, "FAIL [hostfile-cap]: \"%s\" did not select only %s\n",
                            rel[r].body, rel[r].kept);
                    failures++;
                }
                PMIX_LIST_DESTRUCT(&nodes);
            }
        }

        /* a count too large for an int is refused rather than truncated */
        if (NULL == write_hostfile("+e:99999999999\n", path, sizeof(path))) {
            fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
            return failures + 1;
        }
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        PMIX_RETAIN(pool[1]);
        pmix_list_append(&nodes, &pool[1]->super);
        rc = prte_util_filter_hostfile_nodes(&nodes, path, true);
        CHECK("empty: an over-large count is refused", PRTE_ERR_SILENT == rc);
        PMIX_LIST_DESTRUCT(&nodes);

        for (j = 0; j < 3; j++) {
            pmix_pointer_array_set_item(prte_node_pool, pool[j]->index, NULL);
            PMIX_RELEASE(pool[j]);
        }
        if (made_pool) {
            PMIX_RELEASE(prte_node_pool);
            prte_node_pool = NULL;
        }
    }

    /* an empty hostfile filters nothing, and must not stop a -host given
     * alongside it from filtering either.  The hostfile's "take next option"
     * used to be returned straight out of prte_rmaps_base_filter_nodes() -
     * logged as a PRTE ERROR on the way - before the -host was looked at */
    if (NULL == write_hostfile("# nothing but a comment\n", path, sizeof(path))) {
        fprintf(stderr, "FAIL [hostfile-cap]: could not write a temp hostfile\n");
        return failures + 1;
    }
    {
        prte_app_context_t *app = PMIX_NEW(prte_app_context_t);

        prte_set_attribute(&app->attributes, PRTE_APP_HOSTFILE, PRTE_ATTR_GLOBAL, path,
                           PMIX_STRING);
        prte_set_attribute(&app->attributes, PRTE_APP_DASH_HOST, PRTE_ATTR_GLOBAL, "hostB",
                           PMIX_STRING);
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        one_node(&nodes, "hostA", 4);
        nd = one_node(&nodes, "hostB", 4);
        rc = prte_rmaps_base_filter_nodes(app, &nodes, true);
        CHECK("empty hostfile: the -host still filtered", PRTE_SUCCESS == rc);
        CHECK("empty hostfile: only the -host node is left",
              1 == pmix_list_get_size(&nodes)
                  && nd == (prte_node_t *) pmix_list_get_first(&nodes));
        PMIX_LIST_DESTRUCT(&nodes);
        PMIX_RELEASE(app);
    }

    unlink(path);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_hostfile_cap\n");
    }
    return failures;
}
