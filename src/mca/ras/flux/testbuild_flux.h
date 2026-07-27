/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Stand-in for <flux/core.h>, <flux/hostlist.h> and <flux/idset.h>, used only
 * when --enable-testbuild-launchers is given so this component can be COMPILED
 * on a machine with no Flux installation.  ras/flux is an optional build gated
 * on finding Flux *and* jansson, which in practice means almost nobody
 * compiles it -- so it is exactly the component where an edit can sit broken
 * indefinitely.
 *
 * DECLARATIONS ONLY.  Nothing here is implemented, so a tree configured this
 * way must not be RUN: see the note in src/mca/ras/AGENTS.md.
 *
 * Only the subset this component actually uses is declared; add to it as the
 * component grows.
 */

#ifndef PRTE_RAS_FLUX_TESTBUILD_H
#define PRTE_RAS_FLUX_TESTBUILD_H

#include "prte_config.h"

#include "src/mca/ras/base/testbuild_jansson.h"

BEGIN_C_DECLS

/* --- flux/core.h --- */
typedef struct prte_testbuild_flux_t flux_t;
typedef struct prte_testbuild_flux_future_t flux_future_t;

typedef struct {
    char text[160];
} flux_error_t;

flux_t *flux_open_ex(const char *uri, int flags, flux_error_t *error);
void flux_close(flux_t *h);
const char *flux_attr_get(flux_t *h, const char *name);
flux_future_t *flux_kvs_lookup(flux_t *h, const char *ns, int flags,
                               const char *key);
int flux_kvs_lookup_get(flux_future_t *f, const char **value);
void flux_future_destroy(flux_future_t *f);

/* --- flux/hostlist.h --- */
struct hostlist;
struct hostlist *hostlist_create(void);
void hostlist_destroy(struct hostlist *hl);
int hostlist_append(struct hostlist *hl, const char *hosts);
int hostlist_count(struct hostlist *hl);
const char *hostlist_first(struct hostlist *hl);
const char *hostlist_next(struct hostlist *hl);

/* --- flux/idset.h --- */
#define IDSET_INVALID_ID ((unsigned int) -1)
struct idset;
struct idset *idset_decode(const char *s);
void idset_destroy(struct idset *idset);
int idset_count(const struct idset *idset);
unsigned int idset_first(const struct idset *idset);
unsigned int idset_next(const struct idset *idset, unsigned int prev);

END_C_DECLS

#endif /* PRTE_RAS_FLUX_TESTBUILD_H */
