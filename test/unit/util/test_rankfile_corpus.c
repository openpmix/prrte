/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A golden corpus for the rankfile parser, built the same way and for the
 * same reason as the hostfile one next to it: the parser is about to stop
 * being a flex scanner feeding a token-driven state machine and start being
 * a line reader, and the only honest way to make that change is to pin the
 * observable answer first and require the new code to reproduce it case for
 * case.  See test_hostfile_corpus.c for the argument in full.
 *
 * A rankfile's whole output is one record per rank -- the node that rank
 * runs on and the cpus it binds to -- so the rendering is just that, indexed
 * by rank, plus the return code and the count of ranks the file named.
 *
 * What is still wrong as things stand:
 *
 *  - "username=" is refused with a message calling it unsupported, while
 *    "user@host" on the same line is accepted and the user half discarded.
 *    The hostfile parser keeps the user as a node attribute; a rankfile
 *    record has nowhere to put one, so the two spellings of the same thing
 *    get two different answers.  Changing that means giving the record
 *    somewhere to keep it.
 *
 *  - "rank" with no number is refused, but a line that names a rank and a
 *    node and then stops is accepted and contributes a record with no slot
 *    list at all.
 *
 * Seven goldens changed when the parser was rewritten to read lines, each
 * a defect this corpus was written to catch:
 *
 *  - naming the same rank twice was caught only when both lines carried a
 *    "slot=", because that is where the duplicate check lived.  Two lines
 *    naming rank 0 and stopping were accepted in silence: the second record
 *    overwrote the first in the map -- and leaked it, the slot being
 *    replaced without releasing what was there -- the rank count was
 *    incremented for both, and the file's answer was quietly the last one.
 *    A duplicate is now refused wherever it appears, and the record already
 *    filed for that rank is left as it was.
 *
 *  - a refused line used to leave a half-built record behind.  The parser
 *    filed a record the moment it read the rank and filled it in as the
 *    rest of the line arrived, so a line refused after that point left the
 *    map holding a record with no node name, or no slot list, counted in
 *    the rank total.  A line now contributes its record only once the whole
 *    line has been accepted.
 *
 *  - a slot list longer than 64 characters was copied into the record's
 *    fixed buffer up to the limit and no further, with nothing said, so a
 *    rank given a long explicit cpu list was bound to a prefix of what was
 *    asked for.  The list is allocated now and there is no limit.
 *
 * Each is argued in the commit that changed it.
 *
 * Failures are rendered as "err<N>", N being the offset from PRTE_ERR_BASE
 * in src/include/constants.h -- err5 is PRTE_ERR_BAD_PARAM, err13 is
 * PRTE_ERR_NOT_FOUND.
 *
 * Setting PRTE_RANKFILE_CORPUS_REGEN=1 prints each case as a "GOLDEN" line
 * instead of comparing it.  That is for capturing a case that does not have
 * a golden yet, never for silencing one that fails.
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_printf.h"
#include "src/util/rankfile/rankfile.h"

int test_rankfile_corpus(void);

typedef struct {
    const char *name;
    const char *body;
    const char *expect;
} corpus_case_t;

/*
 * Render a parse result to a single deterministic line: the return code, the
 * rank count the parser reported, and every record it filed, named by the
 * rank whose slot in the map it occupies.  A record with no node or no slot
 * list still prints, as "-", so a rewrite that stops filing one -- or files
 * it under the wrong rank -- shows as a diff rather than as a missing line
 * nobody looked for.
 */
static int cmp_rank(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;

    return (x < y) ? -1 : (x > y);
}

static char *render(int rc, pmix_hash_table_t *rankmap, int num_ranks)
{
    prte_rankfile_map_t *rfmap;
    char *out = NULL, *tmp;
    uint32_t key, *keys;
    void *node, *next;
    size_t n = 0, i;

    if (PRTE_SUCCESS == rc) {
        pmix_asprintf(&out, "rc=ok nranks=%d", num_ranks);
    } else {
        pmix_asprintf(&out, "rc=err%d nranks=%d", PRTE_ERR_BASE - rc, num_ranks);
    }

    /* the map is keyed, and a hash table has no order of its own - list the
     * records by rank so the rendering is the same whatever the table does */
    keys = (uint32_t *) malloc((pmix_hash_table_get_size(rankmap) + 1) * sizeof(uint32_t));
    rc = pmix_hash_table_get_first_key_uint32(rankmap, &key, (void **) &rfmap, &node);
    while (PMIX_SUCCESS == rc) {
        keys[n++] = key;
        rc = pmix_hash_table_get_next_key_uint32(rankmap, &key, (void **) &rfmap, node, &next);
        node = next;
    }
    qsort(keys, n, sizeof(uint32_t), cmp_rank);

    for (i = 0; i < n; i++) {
        pmix_hash_table_get_value_uint32(rankmap, keys[i], (void **) &rfmap);
        pmix_asprintf(&tmp, "%s | %u:%s/%s", out, keys[i],
                      (NULL == rfmap->node_name) ? "-" : rfmap->node_name,
                      (NULL == rfmap->slot_list) ? "-" : rfmap->slot_list);
        free(out);
        out = tmp;
    }
    free(keys);

    return out;
}

static int run_case(const corpus_case_t *c, bool regen)
{
    pmix_hash_table_t rankmap;
    char path[256];
    char *got;
    FILE *fp;
    int rc, num_ranks = 0, failed = 0;

    snprintf(path, sizeof(path), "prte_rfcorpus_%lu_%p.txt", (unsigned long) getpid(),
             (void *) c);
    /* a NULL body means the case is about a file that is not there at all */
    if (NULL != c->body) {
        fp = fopen(path, "w");
        if (NULL == fp) {
            fprintf(stderr, "FAIL [rfcorpus:%s]: could not write a temp rankfile\n", c->name);
            return 1;
        }
        fputs(c->body, fp);
        fclose(fp);
    }

    PMIX_CONSTRUCT(&rankmap, pmix_hash_table_t);
    pmix_hash_table_init(&rankmap, 8);

    rc = prte_util_parse_rankfile(path, &rankmap, &num_ranks);
    got = render(rc, &rankmap, num_ranks);

    /* the map is the caller's, on the failure paths as much as the clean one */
    prte_util_rankfile_clear(&rankmap);
    PMIX_DESTRUCT(&rankmap);
    if (NULL != c->body) {
        unlink(path);
    }

    if (regen) {
        fprintf(stdout, "GOLDEN\t%s\t%s\n", c->name, got);
    } else if (0 != strcmp(got, c->expect)) {
        fprintf(stderr, "FAIL [rfcorpus:%s]\n  expected: %s\n  actual:   %s\n", c->name, c->expect,
                got);
        failed = 1;
    }

    free(got);
    return failed;
}

/* ------------------------------------------------------------------ */

static const corpus_case_t corpus[] = {
    /* --- the documented shape ------------------------------------- */
    {"the documented form", "rank 0=nodeA slot=10-12\nrank 1=nodeB slot=0,1,4\n", "rc=ok nranks=2 | 0:nodeA/10-12 | 1:nodeB/0,1,4"},
    {"a package-qualified slot list", "rank 0=nodeA slot=1:0-2\nrank 1=nodeB slot=0:0,1,4\n",
     "rc=ok nranks=2 | 0:nodeA/1:0-2 | 1:nodeB/0:0,1,4"},
    {"a single rank", "rank 0=nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"ranks out of order", "rank 2=nodeC slot=2\nrank 0=nodeA slot=0\nrank 1=nodeB slot=1\n",
     "rc=ok nranks=3 | 0:nodeA/0 | 1:nodeB/1 | 2:nodeC/2"},
    {"a gap in the ranks", "rank 0=nodeA slot=0\nrank 3=nodeD slot=3\n", "rc=ok nranks=2 | 0:nodeA/0 | 3:nodeD/3"},
    {"several ranks on one node", "rank 0=nodeA slot=0\nrank 1=nodeA slot=1\n", "rc=ok nranks=2 | 0:nodeA/0 | 1:nodeA/1"},
    {"a wildcard slot list", "rank 0=nodeA slot=*\n", "rc=ok nranks=1 | 0:nodeA/*"},
    {"a numeric slot", "rank 0=nodeA slot=3\n", "rc=ok nranks=1 | 0:nodeA/3"},
    {"no trailing newline", "rank 0=nodeA slot=0", "rc=ok nranks=1 | 0:nodeA/0"},

    /* --- comments and whitespace ---------------------------------- */
    {"hash comment", "# a comment\nrank 0=nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"slash-slash comment", "// a comment\nrank 0=nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"block comment", "/* a comment */\nrank 0=nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"block comment spanning lines", "/* one\n   two */\nrank 0=nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"trailing comment", "rank 0=nodeA slot=0 # here\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"blank lines", "\n\nrank 0=nodeA slot=0\n\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"leading whitespace", "   rank 0=nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"spaces around the equals", "rank 0 = nodeA slot = 0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"no space after rank", "rank0=nodeA slot=0\n", "rc=err5 nranks=0"},
    {"empty file", "", "rc=ok nranks=0"},
    {"only comments", "# nothing\n", "rc=ok nranks=0"},

    /* --- name forms ------------------------------------------------ */
    {"fqdn", "rank 0=node01.example.com slot=0\n", "rc=ok nranks=1 | 0:node01/0"},
    {"ipv4", "rank 0=10.0.0.1 slot=0\n", "rc=ok nranks=1 | 0:10.0.0.1/0"},
    {"ipv6", "rank 0=fe80::1 slot=0\n", "rc=ok nranks=1 | 0:fe80::1/0"},
    {"user@host", "rank 0=someone@nodeA slot=0\n", "rc=ok nranks=1 | 0:nodeA/0"},
    {"a name that is all digits", "rank 0=12345 slot=0\n", "rc=ok nranks=1 | 0:12345/0"},
    {"name with a dash", "rank 0=node-01 slot=0\n", "rc=ok nranks=1 | 0:node-01/0"},
    {"slots is a synonym for slot", "rank 0=nodeA slots=0\n", "rc=ok nranks=1 | 0:nodeA/0"},

    /* --- refusals and edge cases ----------------------------------- */
    {"a missing file is refused", NULL, "rc=err13 nranks=0"},
    {"rank with no number", "rank=nodeA slot=0\n", "rc=err5 nranks=0"},
    {"slot with no rank", "slot=0\n", "rc=err5 nranks=0"},
    {"a duplicate rank", "rank 0=nodeA slot=0\nrank 0=nodeB slot=1\n", "rc=err5 nranks=1 | 0:nodeA/0"},
    {"a duplicate rank with no slot list", "rank 0=nodeA\nrank 0=nodeB\n", "rc=err5 nranks=1 | 0:nodeA/-"},
    {"slot before rank on a line", "slot=0 rank 0=nodeA\n", "rc=err5 nranks=0"},
    {"uppercase rank keyword", "RANK 0=nodeA slot=0\n", "rc=err5 nranks=0"},
    {"a rank line that stops after the node", "rank 0=nodeA\n", "rc=ok nranks=1 | 0:nodeA/-"},
    {"username is refused", "rank 0=nodeA username=bob slot=0\n", "rc=err5 nranks=0"},
    {"a quoted string is refused", "rank 0=\"nodeA\" slot=0\n", "rc=err5 nranks=0"},
    {"a stray character", "rank 0=nodeA $ slot=0\n", "rc=err5 nranks=0"},
    {"a second @", "rank 0=a@b@nodeA slot=0\n", "rc=err5 nranks=0"},
    {"a user@ with no host", "rank 0=someone@ slot=0\n", "rc=err5 nranks=0"},
    {"an @ with no user", "rank 0=@nodeA slot=0\n", "rc=err5 nranks=0"},
    {"a doubled @", "rank 0=someone@@nodeA slot=0\n", "rc=err5 nranks=0"},
    {"a rank past INT_MAX", "rank 2147483648=nodeA slot=0\n", "rc=err5 nranks=0"},
    {"a very large rank", "rank 1000000000=nodeA slot=0\n", "rc=ok nranks=1 | 1000000000:nodeA/0"},
    {"the largest rank", "rank 2147483647=nodeA\n", "rc=ok nranks=1 | 2147483647:nodeA/-"},
    {"an upper-case relative node spec", "rank 0=+N1 slot=0\n", "rc=ok nranks=1 | 0:+N1/0"},
    {"an over-long slot list",
     "rank 0=nodeA slot=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25\n",
     "rc=ok nranks=1 | 0:nodeA/0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25"},
    {"a relative node spec", "rank 0=+n0 slot=0\n", "rc=ok nranks=1 | 0:+n0/0"},
};

/* ------------------------------------------------------------------ */

int test_rankfile_corpus(void)
{
    int failures = 0;
    size_t i;
    bool regen = (NULL != getenv("PRTE_RANKFILE_CORPUS_REGEN"));
    prte_node_t *hnp;

    /* the parser reads prte_node_pool->addr[0] unconditionally to find the
     * name it should substitute for any entry that turns out to be the local
     * host, so entry 0 has to exist before a single case runs */
    hnp = PMIX_NEW(prte_node_t);
    hnp->name = strdup("poolHNP");
    hnp->slots = 4;
    hnp->index = pmix_pointer_array_add(prte_node_pool, hnp);

    for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        failures += run_case(&corpus[i], regen);
    }

    pmix_pointer_array_set_item(prte_node_pool, hnp->index, NULL);
    PMIX_RELEASE(hnp);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_rankfile_corpus (%zu cases)\n",
                sizeof(corpus) / sizeof(corpus[0]));
    }
    return failures;
}
