/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The shared argv/string helpers in schizo/base: normalize_argv (deprecated
 * option spellings and the personality hint), strip_quotes, getline, and
 * expose.  All of these run before anything else in a tool's main(), on the
 * raw argv, so a mistake here is a mistake every tool inherits.
 */

#include "test_schizo.h"

#include <stdarg.h>
#include <unistd.h>

#include "src/util/pmix_environ.h"

static char **mkargv(const char *first, ...)
{
    char **argv = NULL;
    va_list ap;
    const char *p;

    PMIx_Argv_append_nosize(&argv, first);
    va_start(ap, first);
    while (NULL != (p = va_arg(ap, const char *))) {
        PMIx_Argv_append_nosize(&argv, p);
    }
    va_end(ap);
    return argv;
}

int test_normalize(void)
{
    int failures = 0;
    char **argv;
    char *personality, *p;
    char *tmpname;
    FILE *fp;

    /*** the space-separated spellings ***/
    argv = mkargv("prterun", "--map-by", "node", "--rank-by", "slot",
                  "--bind-to", "core", "--runtime-options", "donotlaunch",
                  "--personality", "ompi", "hostname", NULL);
    personality = prte_schizo_base_normalize_argv(argv);
    CHECK("normalize:mapby", 0 == strcmp(argv[1], "--mapby"));
    CHECK("normalize:rankby", 0 == strcmp(argv[3], "--rankby"));
    CHECK("normalize:bindto", 0 == strcmp(argv[5], "--bindto"));
    CHECK("normalize:rtos", 0 == strcmp(argv[7], "--rtos"));
    CHECK("normalize:personality",
          NULL != personality && 0 == strcmp(personality, "ompi"));
    /* values must be left strictly alone */
    CHECK("normalize:value-untouched", 0 == strcmp(argv[2], "node"));
    PMIx_Argv_free(argv);

    /*** the "--opt=value" spellings getopt_long equally accepts ***/
    argv = mkargv("prterun", "--map-by=node", "--rank-by=slot",
                  "--bind-to=core", "--runtime-options=donotlaunch",
                  "--personality=ompi", "hostname", NULL);
    personality = prte_schizo_base_normalize_argv(argv);
    CHECK("normalize:eq-mapby", 0 == strcmp(argv[1], "--mapby=node"));
    CHECK("normalize:eq-rankby", 0 == strcmp(argv[2], "--rankby=slot"));
    CHECK("normalize:eq-bindto", 0 == strcmp(argv[3], "--bindto=core"));
    CHECK("normalize:eq-rtos", 0 == strcmp(argv[4], "--rtos=donotlaunch"));
    CHECK("normalize:eq-personality",
          NULL != personality && 0 == strcmp(personality, "ompi"));
    PMIx_Argv_free(argv);

    /*** a bare "--personality=" carries no name ***/
    argv = mkargv("prterun", "--personality=", "hostname", NULL);
    personality = prte_schizo_base_normalize_argv(argv);
    CHECK("normalize:eq-personality-empty", NULL == personality);
    PMIx_Argv_free(argv);

    /*** the canonical spellings, and unrelated options, are left alone ***/
    argv = mkargv("prterun", "--mapby", "node", "--map-by-hand",
                  "--host", "node1", "hostname", NULL);
    personality = prte_schizo_base_normalize_argv(argv);
    CHECK("normalize:canonical-kept", 0 == strcmp(argv[1], "--mapby"));
    CHECK("normalize:no-prefix-match", 0 == strcmp(argv[3], "--map-by-hand"));
    CHECK("normalize:unrelated-kept", 0 == strcmp(argv[4], "--host"));
    CHECK("normalize:no-personality", NULL == personality);
    PMIx_Argv_free(argv);

    /*** --personality as the last token has no value to take ***/
    argv = mkargv("prterun", "--personality", NULL);
    personality = prte_schizo_base_normalize_argv(argv);
    CHECK("normalize:trailing-personality", NULL == personality);
    PMIx_Argv_free(argv);

    /*** every occurrence is renamed - MPMD lines repeat them ***/
    argv = mkargv("prterun", "--map-by", "node", ":", "--map-by", "slot", NULL);
    (void) prte_schizo_base_normalize_argv(argv);
    CHECK("normalize:repeat-1", 0 == strcmp(argv[1], "--mapby"));
    CHECK("normalize:repeat-2", 0 == strcmp(argv[4], "--mapby"));
    PMIx_Argv_free(argv);

    /*** strip_quotes ***/
    p = prte_schizo_base_strip_quotes("\"quoted\"");
    CHECK("strip:both", 0 == strcmp(p, "quoted"));
    free(p);
    p = prte_schizo_base_strip_quotes("bare");
    CHECK("strip:none", 0 == strcmp(p, "bare"));
    free(p);
    /* an empty string has no last character to inspect - this used to read
     * and write one byte in front of the allocation */
    p = prte_schizo_base_strip_quotes("");
    CHECK("strip:empty", 0 == strcmp(p, ""));
    free(p);
    /* and a lone quote leaves an empty string behind */
    p = prte_schizo_base_strip_quotes("\"");
    CHECK("strip:lone-quote", 0 == strcmp(p, ""));
    free(p);

    /*** getline: a file whose last line has no newline keeps its last
     *** character (an MCA param file saved without a trailing newline) ***/
    pmix_asprintf(&tmpname, "prte_schizo_getline_%lu.txt",
                  (unsigned long) getpid());
    fp = fopen(tmpname, "w+");
    if (NULL == fp) {
        fprintf(stderr, "FAIL [getline]: cannot create %s\n", tmpname);
        failures++;
    } else {
        fputs("first=1\nlast=2", fp);
        rewind(fp);
        p = prte_schizo_base_getline(fp);
        CHECK("getline:first", NULL != p && 0 == strcmp(p, "first=1"));
        if (NULL != p) {
            free(p);
        }
        p = prte_schizo_base_getline(fp);
        CHECK("getline:last-no-newline", NULL != p && 0 == strcmp(p, "last=2"));
        if (NULL != p) {
            free(p);
        }
        p = prte_schizo_base_getline(fp);
        CHECK("getline:eof", NULL == p);
        fclose(fp);
        unlink(tmpname);
    }
    free(tmpname);

    /*** expose: split at the '=' and push into the environment under the
     *** given prefix, leaving the caller's string as it found it ***/
    {
        char param[] = "schizo_test_var=a=b";
        prte_schizo_base_expose(param, "PRTE_MCA_");
        p = getenv("PRTE_MCA_schizo_test_var");
        CHECK("expose:value", NULL != p && 0 == strcmp(p, "a=b"));
        CHECK("expose:restored", 0 == strcmp(param, "schizo_test_var=a=b"));
        unsetenv("PRTE_MCA_schizo_test_var");
    }
    {
        /* nothing to expose, and nothing to dereference */
        char param[] = "schizo_test_novalue";
        prte_schizo_base_expose(param, "PRTE_MCA_");
        CHECK("expose:no-equals",
              NULL == getenv("PRTE_MCA_schizo_test_novalue"));
    }

    return failures;
}
