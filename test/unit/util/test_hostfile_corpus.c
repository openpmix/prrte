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
 * "we fixed that" from "we broke that".  What is still wrong as things
 * stand:
 *
 *  - "sockets=", "cores=" and "boards=" are refused, and the refusal
 *    discards the node.  These read like things a hostfile ought to accept
 *    and the lexer once had a dedicated token for each; the parser has
 *    never had a case for any of them.  Refusing is at least honest, and
 *    is what the parser has always done, so it is left alone here.
 *
 * Seven goldens changed when the parser was rewritten to read lines, each
 * of them a refusal or a silence that was an artifact of the scanner rather
 * than anybody's intent: a username containing a dot is now kept instead of
 * silently dropped, a file with CRLF line endings parses, a block comment
 * may sit in the middle of a line, "+N0" is accepted as "+n0" already was,
 * and a stray field after the host entry is now refused by name instead of
 * thrown away -- which is what makes "hostA hostB" on one line say so
 * rather than quietly losing hostB.  Each is argued in the commit that
 * changed it.  The relative forms are now tested where they are
 * resolved, in test/unit/rmaps/test_resize.c.
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
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/pmix_printf.h"

int test_hostfile_corpus(void);

/* Every case goes through prte_util_add_hostfile_nodes(), which collapses
 * repeats of one name into a slot count, applies the exclude list, and
 * refuses the "+n<N>" / "+e" relative forms.  Those are resolved only by the
 * job filter, and test/unit/rmaps/test_resize.c covers them there. */
typedef struct {
    const char *name;
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
    rc = prte_util_add_hostfile_nodes(&nodes, path);
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
    {"plain names", "hostA\nhostB\nhostC\n", "rc=ok | hostA slots=1 max=0 given=0 | hostB slots=1 max=0 given=0 | hostC slots=1 max=0 given=0"},
    {"slots", "hostA slots=4\nhostB slots=2\n", "rc=ok | hostA slots=4 max=0 given=1 | hostB slots=2 max=0 given=1"},
    {"slots and max", "hostA slots=2 max_slots=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max alone", "hostA max_slots=8\n", "rc=ok | hostA slots=8 max=8 given=1"},
    {"user@host", "someone@hostA\n", "rc=ok | hostA slots=1 max=0 given=0 user=someone"},
    {"port", "hostA port=2222\n", "rc=ok | hostA slots=1 max=0 given=0 port=2222"},
    {"repeat accumulates slots", "hostA\nhostA\nhostA\n", "rc=ok | hostA slots=3 max=0 given=1"},
    {"exclusion", "hostA\nhostB\n^hostA\n", "rc=ok | hostB slots=1 max=0 given=0"},
    {"exclusion of a name not present", "hostA\n^hostZ\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"exclusion carrying a user@", "hostA\nhostB\n^someone@hostA\n", "rc=ok | hostB slots=1 max=0 given=0"},

    /* --- comments and whitespace ---------------------------------- */
    {"hash comment", "# comment\nhostA\n# another\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"slash-slash comment", "// comment\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"block comment on its own line", "/* comment */\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"block comment spanning lines", "/* one\n   two */\nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"block comment mid-line", "hostA /* x */ slots=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"block comment before an entry", "/* x */ hostA slots=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"unterminated block comment", "hostA\n/* never closed\nhostB\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"a hash inside a name", "host#A\n", "rc=ok | host slots=1 max=0 given=0"},
    {"a line of only whitespace", "   \nhostA\n", "rc=ok | hostA slots=1 max=0 given=0"},
    {"tab separated", "hostA\tslots=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"crlf line endings", "hostA slots=4\r\nhostB\r\n", "rc=ok | hostA slots=4 max=0 given=1 | hostB slots=1 max=0 given=0"},
    {"a lone carriage return", "hostA slots=4\rhostB\n", "rc=err43"},
    {"trailing comment after an entry", "hostA slots=4 # four of them\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"blank lines", "\n\nhostA\n\n\nhostB\n\n", "rc=ok | hostA slots=1 max=0 given=0 | hostB slots=1 max=0 given=0"},
    {"leading whitespace", "   hostA slots=4\n\thostB\n", "rc=ok | hostA slots=4 max=0 given=1 | hostB slots=1 max=0 given=0"},
    {"spaces around the equals", "hostA slots = 4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"no trailing newline", "hostA slots=4", "rc=ok | hostA slots=4 max=0 given=1"},
    {"empty file", "", "rc=ok"},
    {"only comments", "# nothing here\n", "rc=ok"},

    /* --- the many spellings of the slot-count keywords ------------- */
    {"cpu= is slots", "hostA cpu=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"count= is slots", "hostA count=4\n", "rc=ok | hostA slots=4 max=0 given=1"},
    {"slots-max", "hostA slots=2 slots-max=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max-slots", "hostA slots=2 max-slots=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max_cpu", "hostA slots=2 max_cpu=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"cpu-max", "hostA slots=2 cpu-max=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"max_count", "hostA slots=2 max_count=8\n", "rc=ok | hostA slots=2 max=8 given=1"},
    {"count-max", "hostA slots=2 count-max=8\n", "rc=ok | hostA slots=2 max=8 given=1"},

    /* --- name forms ------------------------------------------------ */
    {"fqdn", "node01.example.com slots=2\n", "rc=ok | node01.example.com slots=2 max=0 given=1"},
    {"ipv4", "10.0.0.1 slots=2\n", "rc=ok | 10.0.0.1 slots=2 max=0 given=1"},
    {"user@ipv4", "someone@10.0.0.1\n", "rc=ok | 10.0.0.1 slots=1 max=0 given=0 user=someone"},
    {"user@fqdn", "someone@node01.example.com\n", "rc=ok | node01.example.com slots=1 max=0 given=0 user=someone"},
    {"ipv6", "fe80::1 slots=2\n", "rc=ok | fe80::1 slots=2 max=0 given=1"},
    {"excluded fqdn", "node01.example.com\nhostB\n^node01.example.com\n", "rc=ok | hostB slots=1 max=0 given=0"},
    {"a name that is all digits", "12345 slots=2\n", "rc=ok | 12345 slots=2 max=0 given=1"},
    {"name with a dash", "host-01 slots=2\n", "rc=ok | host-01 slots=2 max=0 given=1"},
    {"name with an underscore", "host_01 slots=2\n", "rc=ok | host_01 slots=2 max=0 given=1"},

    /* --- the username= and port= keyword forms ---------------------- */
    {"username keyword", "hostA username=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},
    {"user-name keyword", "hostA user-name=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},
    {"user_name keyword", "hostA user_name=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},
    {"username with a dot", "hostA username=bob.smith\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob.smith"},
    {"username keyword beats user@", "alice@hostA username=bob\n", "rc=ok | hostA slots=1 max=0 given=0 user=bob"},

    /* --- a rankfile read as a hostfile ------------------------------ */
    {"rank lines", "rank 0=hostA slot=0-1\nrank 1=hostB slot=2-3\n", "rc=ok | hostA slots=1 max=0 given=1 | hostB slots=1 max=0 given=1"},
    {"rank lines repeating a host", "rank 0=hostA slot=0\nrank 1=hostA slot=1\n",
     "rc=ok | hostA slots=2 max=0 given=1"},
    {"rank line with user@", "rank 0=someone@hostA slot=0\n", "rc=ok | hostA slots=1 max=0 given=1 user=someone"},

    /* --- refusals --------------------------------------------------- */
    {"a second @", "a@b@hostA\n", "rc=err43"},
    {"a user@ with no host", "someone@\n", "rc=err43"},
    {"an @ with no user", "@hostA\n", "rc=err43"},
    {"a doubled @", "someone@@hostA\n", "rc=err43"},
    {"a ^ after the @", "someone@^hostA\n", "rc=err43"},
    {"a non-numeric slot count", "hostA slots=many\n", "rc=err43"},
    {"a negative slot count", "hostA slots=-1\n", "rc=err43"},
    {"slots given twice", "hostA slots=2 slots=4\n", "rc=err43"},
    {"max below slots", "hostA slots=8 max_slots=2\n", "rc=err43"},
    {"a bare word after the entry", "hostA foo\n", "rc=err43"},
    {"a bare number after the entry", "hostA 42\n", "rc=err43"},
    {"two hosts on one line", "hostA hostB\n", "rc=err43"},
    {"a key with no value", "hostA slots=\n", "rc=err43"},
    {"a value with no key", "hostA = 4\n", "rc=err43"},
    {"slots given as a word", "hostA slots=four\n", "rc=err43"},
    {"an unknown keyword", "hostA nosuchkey=4\n", "rc=err43"},
    {"sockets=", "hostA sockets=2\n", "rc=err43"},
    {"cores=", "hostA cores=2\n", "rc=err43"},
    {"boards=", "hostA boards=2\n", "rc=err43"},
    {"a quoted string", "\"hostA\"\n", "rc=err43"},
    {"a stray character", "hostA $\n", "rc=err43"},
    {"a bare equals", "hostA =\n", "rc=err43"},
    {"rank with no equals", "rank 0\n", "rc=err43"},
    {"a relative spec", "+n0\n", "rc=err43"},

};

/* ------------------------------------------------------------------ */

int test_hostfile_corpus(void)
{
    int failures = 0;
    size_t i;
    bool regen = (NULL != getenv("PRTE_HOSTFILE_CORPUS_REGEN"));

    for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        failures += run_case(&corpus[i], regen);
    }

    if (0 == failures) {
        fprintf(stdout, "  PASS test_hostfile_corpus (%zu cases)\n",
                sizeof(corpus) / sizeof(corpus[0]));
    }
    return failures;
}
