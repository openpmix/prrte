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
 * Goldens recorded here that are wrong, and are recorded anyway so that the
 * rewrite has to decide about them out loud:
 *
 *  - "username=" is refused with a message that calls it "not supported",
 *    but "user@host" on the same line is accepted and the user half is
 *    silently discarded.  The hostfile parser keeps it as a node attribute;
 *    here it is read, split off and dropped.
 *
 *  - a slot list longer than PRTE_RANKFILE_MAX_SLOTS (64) is truncated into
 *    the record's fixed buffer without a word to the user, so a long cpu
 *    list binds to something other than what it says.
 *
 *  - "rank" with no rank number, and a "slot=" with no preceding rank, are
 *    refused; but a line that names a rank and then stops is accepted and
 *    contributes a record with a node and no slot list at all.
 *
 *  - naming the same rank twice is caught only when both lines carry a
 *    "slot=", because that is where the duplicate check lives.  Two lines
 *    that name rank 0 and stop are accepted silently: the second record
 *    overwrites the first in the map (leaking it -- the slot is replaced
 *    without releasing what was there), the rank count is incremented for
 *    both, and the file's answer is quietly the last one.
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
static char *render(int rc, pmix_pointer_array_t *rankmap, int num_ranks)
{
    prte_rankfile_map_t *rfmap;
    char *out = NULL, *tmp;
    int i;

    if (PRTE_SUCCESS == rc) {
        pmix_asprintf(&out, "rc=ok nranks=%d", num_ranks);
    } else {
        pmix_asprintf(&out, "rc=err%d nranks=%d", PRTE_ERR_BASE - rc, num_ranks);
    }

    for (i = 0; i < rankmap->size; i++) {
        rfmap = (prte_rankfile_map_t *) pmix_pointer_array_get_item(rankmap, i);
        if (NULL == rfmap) {
            continue;
        }
        pmix_asprintf(&tmp, "%s | %d:%s/%s", out, i,
                      (NULL == rfmap->node_name) ? "-" : rfmap->node_name,
                      ('\0' == rfmap->slot_list[0]) ? "-" : rfmap->slot_list);
        free(out);
        out = tmp;
    }

    return out;
}

static int run_case(const corpus_case_t *c, bool regen)
{
    pmix_pointer_array_t rankmap;
    prte_rankfile_map_t *rfmap;
    char path[256];
    char *got;
    FILE *fp;
    int rc, i, num_ranks = 0, failed = 0;

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

    PMIX_CONSTRUCT(&rankmap, pmix_pointer_array_t);
    pmix_pointer_array_init(&rankmap, 8, INT_MAX, 8);

    rc = prte_util_parse_rankfile(path, &rankmap, &num_ranks);
    got = render(rc, &rankmap, num_ranks);

    /* the map is the caller's, on the failure paths as much as the clean one */
    for (i = 0; i < rankmap.size; i++) {
        rfmap = (prte_rankfile_map_t *) pmix_pointer_array_get_item(&rankmap, i);
        if (NULL != rfmap) {
            PMIX_RELEASE(rfmap);
        }
    }
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
    {"a duplicate rank", "rank 0=nodeA slot=0\nrank 0=nodeB slot=1\n", "rc=err5 nranks=2 | 0:nodeB/-"},
    {"a duplicate rank with no slot list", "rank 0=nodeA\nrank 0=nodeB\n", "rc=ok nranks=2 | 0:nodeB/-"},
    {"slot before rank on a line", "slot=0 rank 0=nodeA\n", "rc=err5 nranks=0"},
    {"uppercase rank keyword", "RANK 0=nodeA slot=0\n", "rc=err5 nranks=0"},
    {"a rank line that stops after the node", "rank 0=nodeA\n", "rc=ok nranks=1 | 0:nodeA/-"},
    {"username is refused", "rank 0=nodeA username=bob slot=0\n", "rc=err5 nranks=1 | 0:nodeA/-"},
    {"a quoted string is refused", "rank 0=\"nodeA\" slot=0\n", "rc=err5 nranks=1 | 0:-/-"},
    {"a stray character", "rank 0=nodeA $ slot=0\n", "rc=err5 nranks=1 | 0:nodeA/-"},
    {"a second @", "rank 0=a@b@nodeA slot=0\n", "rc=err5 nranks=1 | 0:-/-"},
    {"an over-long slot list",
     "rank 0=nodeA slot=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25\n",
     "rc=ok nranks=1 | 0:nodeA/0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24"},
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
