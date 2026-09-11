/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A golden corpus for the hostfile parser.
 *
 * Every case below is a hostfile body paired with a canonical rendering of
 * what parsing it produces: the return code, and one line per node carrying
 * every field the parser is capable of setting.  Nothing here asserts that
 * the rendering is *desirable* -- it asserts that it does not change.
 *
 * The point is to make the parser's behavior checkable while it is being
 * rewritten.  The current parser is a flex scanner feeding a token-driven
 * state machine; the replacement reads a line at a time.  That rewrite
 * touches every path in this file, and the only honest way to do it is to
 * pin the observable answer first, then require the new code to reproduce
 * it case for case.
 *
 * Several of these goldens record behavior that is wrong.  They are recorded
 * anyway, and deliberately: a corpus holding only the good cases cannot tell
 * "we fixed that" from "we broke that".  The ones known to be wrong when
 * this was captured:
 *
 *  - "username with a dot" parses clean and silently drops the username.
 *    hostfile_parse_string() demands the lexer's STRING token, but a value
 *    containing a "." lexes as HOSTNAME, so it returns NULL and the caller
 *    treats that as "no username given" rather than as a parse failure.
 *
 *  - "sockets=", "cores=" and "boards=" are fatal parse errors that discard
 *    the node.  The lexer has a dedicated rule and token for each of the
 *    three, and hostfile_parse_line()'s switch has a case for none of them,
 *    so they reach its default arm.
 *
 *  - "ordered relative uppercase N" is refused.  The parser tests for both
 *    'n' and 'N' after the '+', but the lexer's rule is \+n[0-9]+ -- lower
 *    case only -- so "+N0" never becomes a RELATIVE token and the parser's
 *    uppercase branch cannot be reached.
 *
 *  - a stray field after the host entry is thrown away in silence.  The
 *    parser ignores any bare word or number where it expects a keyword,
 *    so "hostA hostB" on one line reads hostA and discards hostB without
 *    a word, and "hostA foo" is accepted as plain hostA.
 *
 *  - a hostfile with CRLF line endings is rejected outright.  The lexer's
 *    whitespace class is [\f\t\v ], so a carriage return matches nothing
 *    but the catch-all error rule -- meaning a hostfile written or edited
 *    on Windows fails with a generic parse error that says nothing about
 *    why, which is among the likelier ways a real user reaches this code.
 *
 *  - a block comment is only accepted between entries.  An opening comment
 *    marker ends the current line as far as the parser is concerned, so a
 *    host name followed by a block comment and then "slots=4" refuses the
 *    line, while the same comment placed before the host name parses fine.
 *
 *  - "ordered relative out of range" and "ordered too many empty" return an
 *    error AND leave the unresolved "+n9" / "+e:9" placeholder on the
 *    caller's list, alongside whichever nodes were expanded before the
 *    failure.
 *
 * Each of those is a decision the rewrite has to make on purpose.  When one
 * of these goldens changes, the change belongs in the commit message; it is
 * never a fixture to be quietly re-baselined.
 *
 * Failures are rendered as "err<N>", where N is the offset from
 * PRTE_ERR_BASE in src/include/constants.h -- err43 is PRTE_ERR_SILENT,
 * which is what every refusal here reports after saying its piece through
 * prte_show_help().
 *
 * Setting PRTE_HOSTFILE_CORPUS_REGEN=1 prints each case as a "GOLDEN" line
 * instead of comparing it.  That is for capturing a case that does not have
 * a golden yet, never for silencing one that fails.
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/pmix_printf.h"

int test_hostfile_corpus(void);

/* Which entry point the case goes through.  They differ in ways the parser
 * is responsible for: ADD collapses repeats of one name into a slot count
 * and applies the exclude list, ORDERED keeps every entry in file order and
 * is the only one that resolves the "+n<N>" / "+e" relative forms. */
typedef enum {
    CORPUS_ADD,
    CORPUS_ORDERED
} corpus_mode_t;

typedef struct {
    const char *name;
    corpus_mode_t mode;
    const char *body;
    const char *expect;
} corpus_case_t;

/* ------------------------------------------------------------------ */

/*
 * Render a parse result to a single deterministic line.  Every field the
 * hostfile parser can set on a node appears here, so a rewrite that drops
 * one -- or sets it on the wrong node -- shows up as a diff rather than as
 * a test that still passes because it never looked.
 */
static char *render(int rc, pmix_list_t *nodes)
{
    prte_node_t *nd;
    char *out = NULL, *tmp;
    char buf[512];
    char *uname;
    int port, *portptr;
    int i;

    /* see the note on "err<N>" in the file comment above */
    if (PRTE_SUCCESS == rc) {
        pmix_asprintf(&out, "rc=ok");
    } else {
        pmix_asprintf(&out, "rc=err%d", PRTE_ERR_BASE - rc);
    }

    PMIX_LIST_FOREACH(nd, nodes, prte_node_t)
    {
        snprintf(buf, sizeof(buf), " | %s slots=%d max=%d given=%d", nd->name, nd->slots,
                 nd->slots_max, PRTE_FLAG_TEST(nd, PRTE_NODE_FLAG_SLOTS_GIVEN) ? 1 : 0);
        pmix_asprintf(&tmp, "%s%s", out, buf);
        free(out);
        out = tmp;

        uname = NULL;
        if (prte_get_attribute(&nd->attributes, PRTE_NODE_USERNAME, (void **) &uname, PMIX_STRING)
            && NULL != uname) {
            pmix_asprintf(&tmp, "%s user=%s", out, uname);
            free(out);
            out = tmp;
        }
        if (NULL != uname) {
            free(uname);
        }

        /* a scalar attribute is fetched through a pointer the caller
         * already aimed at its own storage -- see plm_ssh_module.c */
        port = 0;
        portptr = &port;
        if (prte_get_attribute(&nd->attributes, PRTE_NODE_PORT, (void **) &portptr, PMIX_INT)) {
            pmix_asprintf(&tmp, "%s port=%d", out, port);
            free(out);
            out = tmp;
        }

        if (NULL != nd->aliases) {
            for (i = 0; NULL != nd->aliases[i]; i++) {
                pmix_asprintf(&tmp, "%s alias=%s", out, nd->aliases[i]);
                free(out);
                out = tmp;
            }
        }
    }

    return out;
}

static int run_case(const corpus_case_t *c, bool regen)
{
    pmix_list_t nodes;
    char path[256];
    char *got;
    FILE *fp;
    int rc, failed = 0;

    snprintf(path, sizeof(path), "prte_corpus_%lu_%p.txt", (unsigned long) getpid(),
             (void *) c);
    fp = fopen(path, "w");
    if (NULL == fp) {
        fprintf(stderr, "FAIL [corpus:%s]: could not write a temp hostfile\n", c->name);
        return 1;
    }
    fputs(c->body, fp);
    fclose(fp);

    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    if (CORPUS_ADD == c->mode) {
        rc = prte_util_add_hostfile_nodes(&nodes, path);
    } else {
        rc = prte_util_get_ordered_host_list(&nodes, path);
    }
    got = render(rc, &nodes);
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(path);

    if (regen) {
        fprintf(stdout, "GOLDEN\t%s\t%s\n", c->name, got);
    } else if (0 != strcmp(got, c->expect)) {
        fprintf(stderr, "FAIL [corpus:%s]\n  expected: %s\n  actual:   %s\n", c->name, c->expect,
                got);
        failed = 1;
    }

    free(got);
    return failed;
}

/* ------------------------------------------------------------------ */

static const corpus_case_t corpus[] = {
    /* --- the documented shape ------------------------------------- */
    {"plain names", CORPUS_ADD, "hostA\nhostB\nhostC\n", "rc=ok | hostA slots=1 max=0 given=0 | hostB slots=1 max=0 given=0 | hostC slots=1 max=0 given=0"},
    {"slots", CORPUS_ADD, "hostA slots=4\nhostB slots=2\n", "rc=ok | hostA slots=4 max=0 given=1 | hostB slots=2 max=0 given=1"},
    {"slots and max", CORPUS_ADD, "hostA slots=2 max_slots=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max alone", CORPUS_ADD, "hostA max_slots=8\n", "rc=ok | hostA slots=8 max=8 given=1"},
    {"user@host", CORPUS_ADD, "someone@hostA\n", "rc=ok | hostA slots=1 max=0 given=0 user=someone"},
    {"port", CORPUS_ADD, "hostA port=2222\n", "rc=ok | hostA slots=1 max=0 given=0 port=2222"},
    {"repeat accumulates slots", CORPUS_ADD, "hostA\nhostA\nhostA\n", "rc=ok | hostA slots=3 max=0 given=1"},
    {"exclusion", CORPUS_ADD, "hostA\nhostB\n^hostA\n", "rc=ok | hostB slots=1 max=0 given=0"},
    {"exclusion of a name not present", CORPUS_ADD, "hostA\n^hostZ\n", "rc=ok | hostA slots=1 max=0 given=0"},

    /* --- comments and whitespace ---------------------------------- */
    {"hash comment", CORPUS_ADD, "# comment\nhostA\n# another\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"slash-slash comment", CORPUS_ADD, "// comment\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"block comment on its own line", CORPUS_ADD, "/* comment */\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"block comment spanning lines", CORPUS_ADD, "/* one\n   two */\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"block comment mid-line", CORPUS_ADD, "hostA /* x */ slots=4\n", "rc=err43"},
    {"block comment before an entry", CORPUS_ADD, "/* x */ hostA slots=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"unterminated block comment", CORPUS_ADD, "hostA\n/* never closed\nhostB\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"a hash inside a name", CORPUS_ADD, "host#A\n", "rc=ok | host slots=1 max=0 given=0"},
    {"a line of only whitespace", CORPUS_ADD, "   \nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"tab separated", CORPUS_ADD, "hostA\tslots=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"crlf line endings", CORPUS_ADD, "hostA slots=4\r\nhostB\r\n", "rc=err43"},
    {"a lone carriage return", CORPUS_ADD, "hostA slots=4\rhostB\n", "rc=err43"},
    {"trailing comment after an entry", CORPUS_ADD, "hostA slots=4 # four of them\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"blank lines", CORPUS_ADD, "\n\nhostA\n\n\nhostB\n\n", "rc=ok | hostA slots=1 max=0 given=0 | hostB slots=1 max=0 given=0"},
    {"leading whitespace", CORPUS_ADD, "   hostA slots=4\n\thostB\n", "rc=ok | hostA slots=4 max=0 given=1 | hostB slots=1 max=0 given=0"},
    {"spaces around the equals", CORPUS_ADD, "hostA slots = 4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"no trailing newline", CORPUS_ADD, "hostA slots=4", "rc=ok | hostA slots=4 max=0 given=1"},
    {"empty file", CORPUS_ADD, "", "rc=ok"},
    {"only comments", CORPUS_ADD, "# nothing here\n", "rc=ok"},

    /* --- the many spellings of the slot-count keywords ------------- */
    {"cpu= is slots", CORPUS_ADD, "hostA cpu=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"count= is slots", CORPUS_ADD, "hostA count=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"slots-max", CORPUS_ADD, "hostA slots=2 slots-max=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max-slots", CORPUS_ADD, "hostA slots=2 max-slots=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max_cpu", CORPUS_ADD, "hostA slots=2 max_cpu=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"cpu-max", CORPUS_ADD, "hostA slots=2 cpu-max=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max_count", CORPUS_ADD, "hostA slots=2 max_count=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"count-max", CORPUS_ADD, "hostA slots=2 count-max=8\n", "rc=ok | hostA slots=2 max=8 given=1"},

    /* --- name forms ------------------------------------------------ */
    {"fqdn", CORPUS_ADD, "node01.example.com slots=2\n", "rc=ok | node01.example.com slots=2 max=0 given=1"},
    {"ipv4", CORPUS_ADD, "10.0.0.1 slots=2\n", "rc=ok | 10.0.0.1 slots=2 max=0 given=1"},
    {"user@ipv4", CORPUS_ADD, "someone@10.0.0.1\n", "rc=ok | 10.0.0.1 slots=1 max=0 given=0 user=someone"},
    {"user@fqdn", CORPUS_ADD, "someone@node01.example.com\n", "rc=ok | node01.example.com slots=1 max=0 given=0 user=someone"},
    {"ipv6", CORPUS_ADD, "fe80::1 slots=2\n", "rc=ok | fe80::1 slots=2 max=0 given=1"},
    {"excluded fqdn", CORPUS_ADD, "node01.example.com\nhostB\n^node01.example.com\n", "rc=ok | hostB slots=1 max=0 given=0"},
    {"a name that is all digits", CORPUS_ADD, "12345 slots=2\n", "rc=ok | 12345 slots=2 max=0 given=1"},
    {"name with a dash", CORPUS_ADD, "host-01 slots=2\n", "rc=ok | host-01 slots=2 max=0 given=1"},
    {"name with an underscore", CORPUS_ADD, "host_01 slots=2\n", "rc=ok | host_01 slots=2 max=0 given=1"},

    /* --- the username= and port= keyword forms ---------------------- */
    {"username keyword", CORPUS_ADD, "hostA username=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},
    {"user-name keyword", CORPUS_ADD, "hostA user-name=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},
    {"user_name keyword", CORPUS_ADD, "hostA user_name=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},
    {"username with a dot", CORPUS_ADD, "hostA username=bob.smith\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"username keyword beats user@", CORPUS_ADD, "alice@hostA username=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},

    /* --- a rankfile read as a hostfile ------------------------------ */
    {"rank lines", CORPUS_ADD, "rank 0=hostA slot=0-1\nrank 1=hostB slot=2-3\n", "rc=ok | hostA slots=1 max=0 given=1 | hostB slots=1 max=0 given=1"},
    {"rank lines repeating a host", CORPUS_ADD, "rank 0=hostA slot=0\nrank 1=hostA slot=1\n",
     "rc=ok | hostA slots=2 max=0 given=1"},
    {"rank line with user@", CORPUS_ADD, "rank 0=someone@hostA slot=0\n", "rc=ok | hostA slots=1 max=0 given=1 user=someone"},

    /* --- refusals --------------------------------------------------- */
    {"a second @", CORPUS_ADD, "a@b@hostA\n", "rc=err43"},
    {"a non-numeric slot count", CORPUS_ADD, "hostA slots=many\n", "rc=err43"},
    {"a negative slot count", CORPUS_ADD, "hostA slots=-1\n", "rc=err43"},
    {"slots given twice", CORPUS_ADD, "hostA slots=2 slots=4\n", "rc=err43"},
    {"max below slots", CORPUS_ADD, "hostA slots=8 max_slots=2\n", "rc=err43"},
    {"a bare word after the entry", CORPUS_ADD, "hostA foo\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"a bare number after the entry", CORPUS_ADD, "hostA 42\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"two hosts on one line", CORPUS_ADD, "hostA hostB\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"a key with no value", CORPUS_ADD, "hostA slots=\n", "rc=err43"},
    {"a value with no key", CORPUS_ADD, "hostA = 4\n", "rc=err43"},
    {"slots given as a word", CORPUS_ADD, "hostA slots=four\n", "rc=err43"},
    {"an unknown keyword", CORPUS_ADD, "hostA nosuchkey=4\n", "rc=err43"},
    {"sockets=", CORPUS_ADD, "hostA sockets=2\n", "rc=err43"},
    {"cores=", CORPUS_ADD, "hostA cores=2\n", "rc=err43"},
    {"boards=", CORPUS_ADD, "hostA boards=2\n", "rc=err43"},
    {"a quoted string", CORPUS_ADD, "\"hostA\"\n", "rc=err43"},
    {"a stray character", CORPUS_ADD, "hostA $\n", "rc=err43"},
    {"a bare equals", CORPUS_ADD, "hostA =\n", "rc=err43"},
    {"rank with no equals", CORPUS_ADD, "rank 0\n", "rc=err43"},
    {"a relative spec", CORPUS_ADD, "+n0\n", "rc=err43"},

    /* --- ordered-list mode ------------------------------------------ */
    {"ordered keeps duplicates", CORPUS_ORDERED, "hostA\nhostB\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0 | hostB slots=1 max=0 given=0 | hostA slots=1 max=0 given=0"},
    {"ordered applies exclusion", CORPUS_ORDERED, "hostA\nhostB\nhostA\n^hostA\n", "rc=ok | hostB slots=1 max=0 given=0"},
    {"ordered relative by index", CORPUS_ORDERED, "+n0\n", "rc=ok | poolB slots=6 max=0 given=1"},
    {"ordered relative by index, second", CORPUS_ORDERED, "+n1\n", "rc=ok | poolC slots=8 max=0 given=1"},
    {"ordered relative out of range", CORPUS_ORDERED, "+n9\n", "rc=err43 | +n9 slots=0 max=0 given=0"},
    {"ordered relative with a slot count", CORPUS_ORDERED, "+n0 slots=2\n", "rc=ok | poolB slots=2 max=0 given=1"},
    {"ordered relative uppercase N", CORPUS_ORDERED, "+N0\n", "rc=err43"},
    {"ordered relative bad letter", CORPUS_ORDERED, "+x0\n", "rc=err43"},
    {"ordered all empty", CORPUS_ORDERED, "+e\n", "rc=ok | poolB slots=6 max=0 given=1 | poolC slots=8 max=0 given=1"},
    {"ordered n empty", CORPUS_ORDERED, "+e:1\n", "rc=ok | poolB slots=6 max=0 given=1"},
    {"ordered too many empty", CORPUS_ORDERED, "+e:9\n", "rc=err43 | +e:9 slots=0 max=0 given=0 | poolB slots=6 max=0 given=1 | poolC slots=8 max=0 given=1"},
    {"ordered mixes names and relatives", CORPUS_ORDERED, "hostA\n+n0\nhostB\n", "rc=ok | hostA slots=1 max=0 given=0 | poolB slots=6 max=0 given=1 | hostB slots=1 max=0 given=0"},
    {"ordered with slots", CORPUS_ORDERED, "hostA slots=4\nhostA slots=2\n", "rc=ok | hostA slots=4 max=0 given=1 | hostA slots=2 max=0 given=1"},
};

/* ------------------------------------------------------------------ */

int test_hostfile_corpus(void)
{
    int failures = 0;
    size_t i;
    bool regen = (NULL != getenv("PRTE_HOSTFILE_CORPUS_REGEN"));
    prte_node_t *pool[3];
    static const char *poolnames[3] = {"poolA", "poolB", "poolC"};
    int j;

    /*
     * The relative forms ("+n<N>", "+e") are resolved against the node pool,
     * so the corpus has to supply one or every relative case records nothing
     * but "there were no nodes to pick from".
     *
     * Entry 0 stands in for the HNP's own node.  prte_hnp_is_allocated is
     * false here, as it is whenever the HNP is not part of the allocation,
     * and the parser adds one to every "+n<N>" index to step over that entry
     * -- so "+n0" resolves to poolB, not poolA.  That is the intended
     * numbering, not an off-by-one.
     */
    for (j = 0; j < 3; j++) {
        pool[j] = PMIX_NEW(prte_node_t);
        pool[j]->name = strdup(poolnames[j]);
        pool[j]->slots = 4 + (2 * j);
        pool[j]->index = pmix_pointer_array_add(prte_node_pool, pool[j]);
    }

    for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        failures += run_case(&corpus[i], regen);
    }

    for (j = 0; j < 3; j++) {
        pmix_pointer_array_set_item(prte_node_pool, pool[j]->index, NULL);
        PMIX_RELEASE(pool[j]);
    }

    if (0 == failures) {
        fprintf(stdout, "  PASS test_hostfile_corpus (%zu cases)\n",
                sizeof(corpus) / sizeof(corpus[0]));
    }
    return failures;
}
