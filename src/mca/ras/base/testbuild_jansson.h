/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Stand-in for <jansson.h>, used only when --enable-testbuild-launchers is
 * given so that the JSON-parsing paths of ras/slurm and ras/flux can be
 * COMPILED on a machine that has no jansson installed.  Those are the largest
 * and least-exercised bodies of code in the ras framework -- ras/slurm's
 * "scontrol --json" parser alone is ~1000 lines -- and without this they are
 * silently skipped by every developer build and every CI job, so a typo in
 * them is not caught until someone with jansson tries to build.
 *
 * DECLARATIONS ONLY.  Nothing here is implemented, so a tree configured this
 * way must not be RUN: see the note in src/mca/ras/AGENTS.md.
 *
 * Only the subset actually used by the ras components is declared; add to it
 * as they grow.
 */

#ifndef PRTE_RAS_TESTBUILD_JANSSON_H
#define PRTE_RAS_TESTBUILD_JANSSON_H

#include "prte_config.h"

#include <stddef.h>

BEGIN_C_DECLS

typedef struct prte_testbuild_json_t json_t;
typedef long long json_int_t;
#define JSON_INTEGER_FORMAT "lld"

typedef struct {
    char text[160];
    char source[80];
    int line;
    int column;
    size_t position;
} json_error_t;

/* load flags */
#define JSON_REJECT_DUPLICATES 0x1
#define JSON_DECODE_ANY        0x2

typedef size_t (*json_load_callback_t)(void *buffer, size_t buflen, void *data);

json_t *json_loads(const char *input, size_t flags, json_error_t *error);
json_t *json_load_callback(json_load_callback_t callback, void *data,
                           size_t flags, json_error_t *error);
int json_unpack(json_t *root, const char *fmt, ...);
int json_unpack_ex(json_t *root, json_error_t *error, size_t flags,
                   const char *fmt, ...);

json_t *json_incref(json_t *json);
void json_decref(json_t *json);

json_t *json_object_get(const json_t *object, const char *key);
size_t json_array_size(const json_t *array);
json_t *json_array_get(const json_t *array, size_t index);

int json_is_object(const json_t *json);
int json_is_array(const json_t *json);
int json_is_string(const json_t *json);
int json_is_integer(const json_t *json);
int json_is_boolean(const json_t *json);
int json_is_true(const json_t *json);
int json_is_false(const json_t *json);
int json_is_null(const json_t *json);

const char *json_string_value(const json_t *string);
size_t json_string_length(const json_t *string);
json_int_t json_integer_value(const json_t *integer);

/* jansson defines this as a macro; mirror its shape so the loop bodies in the
 * components compile identically */
#define json_array_foreach(array, index, value)                     \
    for (index = 0;                                                 \
         index < json_array_size(array) &&                          \
             (value = json_array_get(array, index));                \
         index++)

END_C_DECLS

#endif /* PRTE_RAS_TESTBUILD_JANSSON_H */
