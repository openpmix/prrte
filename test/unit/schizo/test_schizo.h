/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_TEST_SCHIZO_H
#define PRTE_TEST_SCHIZO_H

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/schizo/schizo.h"
#include "src/util/pmix_argv.h"
#include "src/util/prte_cmd_line.h"

#define CHECK(label, cond)                                          \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL [%s]: %s\n", (label), #cond);     \
            failures++;                                             \
        }                                                           \
    } while (0)

/* the individual test entry points */
extern int test_normalize(void);
extern int test_directives(void);
extern int test_sanity(void);
extern int test_output(void);
extern int test_personality(void);

/* shared helpers (test_helpers.c) */

/* Append an option instance to a result set.  Pass the values in order,
 * terminated by NULL; pass no values at all for a boolean-style option. */
extern void schizo_test_add(pmix_cli_result_t *results, const char *key, ...);

/* Look up the module a personality name resolves to, or NULL if that
 * component was not built into this tree. */
extern prte_schizo_base_module_t *schizo_test_module(const char *name);

#endif /* PRTE_TEST_SCHIZO_H */
