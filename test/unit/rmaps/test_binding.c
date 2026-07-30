/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests prte_rmaps_base_bind_proc() against a synthetic topology.
 *
 * The question this answers is what cpuset a proc ends up owning, which is
 * the one thing about binding a user can see (--display map, and the
 * PMIX_CPUSET the proc itself reads). It needs no DVM: a node carrying a
 * synthetic topology plus a hand-built options struct is enough.
 *
 * The case that brought this file into being: binding to an object WIDER
 * than a core ignored the job's cpu-set entirely. A rank bound to a package
 * came back owning every core of that package, cpu-set or no cpu-set, while
 * --bind-to core honored the set - so the defect hid behind the one binding
 * level that happened to work. The documented intent is in the cpu_list
 * comment in src/hwloc/hwloc.c: a proc bound to a package under a cpu-set
 * gets the cpu-set's cores on that package, and every core of the package
 * only when no cpu-set was given.
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"

int test_binding(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* An 8-core, single-package, no-SMT node - the shape that exposed the
 * defect, and the shape of the dockerswarm containers. */
#define TOPO_DESC "package:1 core:8 pu:1"

typedef struct {
    prte_job_t *jdata;
    prte_node_t *node;
    prte_topology_t *t;
} fixture_t;

static bool fixture_build(fixture_t *f)
{
    hwloc_topology_t topo;

    if (0 != hwloc_topology_init(&topo)) {
        return false;
    }
    if (0 != hwloc_topology_set_synthetic(topo, TOPO_DESC) ||
        0 != hwloc_topology_load(topo)) {
        hwloc_topology_destroy(topo);
        return false;
    }

    f->jdata = PMIX_NEW(prte_job_t);
    f->jdata->map = PMIX_NEW(prte_job_map_t);
    f->t = PMIX_NEW(prte_topology_t);
    f->t->topo = topo;

    f->node = PMIX_NEW(prte_node_t);
    f->node->name = strdup("testnode");
    PMIX_RETAIN(f->t);
    f->node->topology = f->t;
    /* node->available is what the daemon reported minus what is consumed;
     * node->jobcache is that same set as the job first found it. A DVM-wide
     * cpu-set has already been applied to both by
     * prte_hwloc_base_filter_cpus() before the mapper ever sees the node.
     * Note the node constructor leaves "available" NULL - it is filled in
     * when the node is inserted with a topology - so a hand-built node has
     * to allocate it. */
    f->node->available = hwloc_bitmap_alloc();
    hwloc_bitmap_copy(f->node->available, hwloc_topology_get_allowed_cpuset(topo));
    hwloc_bitmap_copy(f->node->jobcache, f->node->available);
    return true;
}

static void fixture_free(fixture_t *f)
{
    PMIX_RELEASE(f->node);
    PMIX_RELEASE(f->t);
    PMIX_RELEASE(f->jdata);
}

/* Narrow the node to a cpu-set, the way filter_cpus() does for a DVM-wide
 * hwloc_default_cpu_list. */
static void fixture_apply_dvm_cpuset(fixture_t *f, const char *list)
{
    hwloc_bitmap_t set = hwloc_bitmap_alloc();

    hwloc_bitmap_list_sscanf(set, list);
    hwloc_bitmap_and(f->node->available, f->node->available, set);
    hwloc_bitmap_copy(f->node->jobcache, f->node->available);
    hwloc_bitmap_free(set);
}

static void opts_init(prte_rmaps_options_t *opts, fixture_t *f,
                      int bind, hwloc_obj_type_t hwb)
{
    memset(opts, 0, sizeof(*opts));
    opts->map = PRTE_MAPPING_BYCORE;
    opts->bind = bind;
    opts->hwb = hwb;
    opts->maptype = HWLOC_OBJ_CORE;
    opts->app_idx = -1;
    /* what check_avail() and get_cpuset() leave behind for the binder */
    opts->target = hwloc_bitmap_dup(f->node->available);
    opts->job_cpuset = hwloc_bitmap_dup(f->node->available);
    PRTE_SET_BINDING_POLICY(f->jdata->map->binding, bind);
}

static void opts_free(prte_rmaps_options_t *opts)
{
    if (NULL != opts->target) {
        hwloc_bitmap_free(opts->target);
    }
    if (NULL != opts->job_cpuset) {
        hwloc_bitmap_free(opts->job_cpuset);
    }
}

/* Bind one fresh proc and hand back its cpuset string (caller frees). */
static char *bind_one(fixture_t *f, prte_rmaps_options_t *opts)
{
    prte_proc_t *proc;
    char *cpuset = NULL;
    int rc;

    proc = PMIX_NEW(prte_proc_t);
    proc->name.rank = 0;
    PMIX_LOAD_NSPACE(proc->name.nspace, "testjob");
    rc = prte_rmaps_base_bind_proc(f->jdata, proc, f->node, NULL, opts);
    if (PRTE_SUCCESS == rc && NULL != proc->cpuset) {
        cpuset = strdup(proc->cpuset);
    }
    PMIX_RELEASE(proc);
    return cpuset;
}

int test_binding(void)
{
    int failures = 0;
    fixture_t f;
    prte_rmaps_options_t opts;
    char *cpuset, *second;

    if (!fixture_build(&f)) {
        fprintf(stdout, "  SKIP test_binding (no synthetic topology support)\n");
        return 0;
    }

    /* --- no cpu-set: binding to the package gets the whole package ----- */
    opts_init(&opts, &f, PRTE_BIND_TO_PACKAGE, HWLOC_OBJ_PACKAGE);
    cpuset = bind_one(&f, &opts);
    CHECK("package binding produced a cpuset", NULL != cpuset);
    CHECK("package binding covers all 8 cores",
          NULL != cpuset && 0 == strcmp(cpuset, "0-7"));
    free(cpuset);
    opts_free(&opts);
    fixture_free(&f);

    /* --- a cpu-set: binding to the package is confined to it ----------- *
     * This is the regression. The cpuset used to come back as the whole
     * package ("0-7"), silently discarding the constraint. */
    if (!fixture_build(&f)) {
        return failures;
    }
    fixture_apply_dvm_cpuset(&f, "0-1");
    opts_init(&opts, &f, PRTE_BIND_TO_PACKAGE, HWLOC_OBJ_PACKAGE);
    cpuset = bind_one(&f, &opts);
    CHECK("package binding under a cpu-set produced a cpuset", NULL != cpuset);
    CHECK("package binding under a cpu-set is confined to it",
          NULL != cpuset && 0 == strcmp(cpuset, "0-1"));
    /* ...and a second proc on the same package gets the SAME answer: the
     * constraint must come from the job's view of the node, which does not
     * shrink as procs are placed, not from node->available, which does */
    second = bind_one(&f, &opts);
    CHECK("a second proc on the package gets the same cpuset",
          NULL != second && NULL != cpuset && 0 == strcmp(cpuset, second));
    free(cpuset);
    free(second);
    opts_free(&opts);
    fixture_free(&f);

    /* --- a non-contiguous cpu-set ------------------------------------- */
    if (!fixture_build(&f)) {
        return failures;
    }
    fixture_apply_dvm_cpuset(&f, "0,2,4");
    opts_init(&opts, &f, PRTE_BIND_TO_PACKAGE, HWLOC_OBJ_PACKAGE);
    cpuset = bind_one(&f, &opts);
    CHECK("a scattered cpu-set is reproduced exactly",
          NULL != cpuset && 0 == strcmp(cpuset, "0,2,4"));
    free(cpuset);
    opts_free(&opts);
    fixture_free(&f);

    /* --- core binding under a cpu-set: unchanged, one core at a time --- *
     * This level always worked, which is exactly why the defect above went
     * unnoticed; pin it so a fix at the object level cannot break it. */
    if (!fixture_build(&f)) {
        return failures;
    }
    fixture_apply_dvm_cpuset(&f, "2-3");
    opts_init(&opts, &f, PRTE_BIND_TO_CORE, HWLOC_OBJ_CORE);
    cpuset = bind_one(&f, &opts);
    CHECK("core binding takes the first core of the cpu-set",
          NULL != cpuset && 0 == strcmp(cpuset, "2"));
    second = bind_one(&f, &opts);
    CHECK("the next proc takes the next core in the cpu-set",
          NULL != second && 0 == strcmp(second, "3"));
    free(cpuset);
    free(second);
    opts_free(&opts);
    fixture_free(&f);

    /* --- bind-to-none records nothing -------------------------------- */
    if (!fixture_build(&f)) {
        return failures;
    }
    opts_init(&opts, &f, PRTE_BIND_TO_NONE, HWLOC_OBJ_CORE);
    cpuset = bind_one(&f, &opts);
    CHECK("bind-to-none leaves the proc unbound", NULL == cpuset);
    free(cpuset);
    opts_free(&opts);
    fixture_free(&f);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_binding\n");
    }
    return failures;
}
