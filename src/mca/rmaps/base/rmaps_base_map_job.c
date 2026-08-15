/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      UT-Battelle, LLC. All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int map_colocate(prte_job_t *jdata,
                        bool daemons, bool pernode,
                        pmix_data_array_t *darray,
                        uint16_t procs_per_target,
                        prte_rmaps_options_t *options);

static void inherit_env_directives(prte_job_t *jdata,
                                   prte_job_t *parent,
                                   pmix_proc_t *proxy);

/* Translate the object half of a ppr pattern ("N:<object>") into the hwloc
 * object type and binding depth the mappers work against. Returns false if
 * the name is not one we map by; the caller reports it, because the job and
 * the app have different things to say about where the bad spelling came
 * from. Names may be abbreviated, which is why each comparison is against
 * the length of what the user actually wrote. */
static bool ppr_object(const char *obj,
                       hwloc_obj_type_t *maptype,
                       prte_binding_policy_t *mapdepth,
                       char **device)
{
    size_t len = strlen(obj);

    if (0 == len) {
        return false;
    }
    /* "device=<class>" names the devices rather than an hwloc level, so the
     * pattern reads "N procs per device of this class".  It is spelled the
     * same way as the --map-by directive deliberately: it is the same
     * resource, asked about a different way round. */
    if (0 == strncasecmp(obj, "device=", 7)) {
        if ('\0' == obj[7]) {
            return false;
        }
        if (NULL != device) {
            if (NULL != *device) {
                free(*device);
            }
            *device = strdup(&obj[7]);
        }
        *maptype = HWLOC_OBJ_OS_DEVICE;
        *mapdepth = PRTE_BIND_TO_NONE;
        return true;
    }
    if (0 == strncasecmp(obj, "node", len)) {
        *maptype = HWLOC_OBJ_MACHINE;
        *mapdepth = PRTE_BIND_TO_NONE;
    } else if (0 == strncasecmp(obj, "hwthread", len) ||
               0 == strncasecmp(obj, "thread", len)) {
        *maptype = HWLOC_OBJ_PU;
        *mapdepth = PRTE_BIND_TO_HWTHREAD;
    } else if (0 == strncasecmp(obj, "core", len)) {
        *maptype = HWLOC_OBJ_CORE;
        *mapdepth = PRTE_BIND_TO_CORE;
    } else if (0 == strncasecmp(obj, "package", len) ||
               0 == strncasecmp(obj, "skt", len) ||
               0 == strncasecmp(obj, "socket", len)) {
        *maptype = HWLOC_OBJ_PACKAGE;
        *mapdepth = PRTE_BIND_TO_PACKAGE;
    } else if (0 == strncasecmp(obj, "numa", len) ||
               0 == strncasecmp(obj, "nm", len)) {
        *maptype = HWLOC_OBJ_NUMANODE;
        *mapdepth = PRTE_BIND_TO_NUMA;
    } else if (0 == strncasecmp(obj, "l1cache", len)) {
        *maptype = HWLOC_OBJ_L1CACHE;
        *mapdepth = PRTE_BIND_TO_L1CACHE;
    } else if (0 == strncasecmp(obj, "l2cache", len)) {
        *maptype = HWLOC_OBJ_L2CACHE;
        *mapdepth = PRTE_BIND_TO_L2CACHE;
    } else if (0 == strncasecmp(obj, "l3cache", len)) {
        *maptype = HWLOC_OBJ_L3CACHE;
        *mapdepth = PRTE_BIND_TO_L3CACHE;
    } else {
        return false;
    }
    return true;
}

/* Override job-level opts with per-app attributes where present.
 * Does not modify jdata->map. */
/* Derive the default ranking policy from a mapping policy, mirroring the
 * NULL-spec path of prte_rmaps_base_set_ranking_policy().  Returns a bare
 * PRTE_RANK_BY_* value with no directive bits.  The full mapping value
 * (including directives) is passed so the SPAN directive can be honored. */
prte_ranking_policy_t prte_rmaps_base_derive_ranking(prte_mapping_policy_t mapping)
{
    prte_mapping_policy_t pol = PRTE_GET_MAPPING_POLICY(mapping);

    if (PRTE_MAPPING_BYNODE == pol) {
        return PRTE_RANK_BY_NODE;
    }
    if (PRTE_MAPPING_BYSLOT == pol) {
        return PRTE_RANK_BY_SLOT;
    }
    if (0 != (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(mapping))) {
        return PRTE_RANK_BY_SPAN;
    }
    if (PRTE_MAPPING_BYNUMA <= pol && PRTE_MAPPING_BYHWTHREAD >= pol) {
        return PRTE_RANK_BY_FILL;
    }
    return PRTE_RANK_BY_SLOT;
}

/* Derive the default binding policy for an app from its resolved mapping,
 * faithfully mirroring prte_hwloc_base_set_default_binding(): an app mapped by
 * a topology object binds to that object; pe-list and pes-per-proc bind to a
 * cpu; ppr binds to its pattern object; and every non-object mapping (by-node,
 * by-slot, dist, seq, ppr-by-node, ...) binds to a cpu for small jobs and to
 * numa for larger ones.  Reads opts->map/maptype/nprocs/cpus_per_rank/
 * use_hwthreads.  Returns a bare PRTE_BIND_TO_* value with no directive bits. */
prte_binding_policy_t prte_rmaps_base_derive_binding(prte_rmaps_options_t *opts)
{
    bool hwt = opts->use_hwthreads || prte_rmaps_base.require_hwtcpus;

    /* pes-per-proc forces binding down to a cpu */
    if (1 < opts->cpus_per_rank) {
        return hwt ? PRTE_BIND_TO_HWTHREAD : PRTE_BIND_TO_CORE;
    }

    switch (PRTE_GET_MAPPING_POLICY(opts->map)) {
        case PRTE_MAPPING_BYHWTHREAD:
            return PRTE_BIND_TO_HWTHREAD;
        case PRTE_MAPPING_BYCORE:
            return PRTE_BIND_TO_CORE;
        case PRTE_MAPPING_BYL1CACHE:
            return PRTE_BIND_TO_L1CACHE;
        case PRTE_MAPPING_BYL2CACHE:
            return PRTE_BIND_TO_L2CACHE;
        case PRTE_MAPPING_BYL3CACHE:
            return PRTE_BIND_TO_L3CACHE;
        case PRTE_MAPPING_BYNUMA:
            return PRTE_BIND_TO_NUMA;
        case PRTE_MAPPING_BYPACKAGE:
            return PRTE_BIND_TO_PACKAGE;
        case PRTE_MAPPING_PELIST:
            /* pe-list follows the cpu designation only (not require_hwtcpus) */
            return opts->use_hwthreads ? PRTE_BIND_TO_HWTHREAD : PRTE_BIND_TO_CORE;
        case PRTE_MAPPING_PPR:
            switch (opts->maptype) {
                case HWLOC_OBJ_PACKAGE:  return PRTE_BIND_TO_PACKAGE;
                case HWLOC_OBJ_NUMANODE: return PRTE_BIND_TO_NUMA;
                case HWLOC_OBJ_L1CACHE:  return PRTE_BIND_TO_L1CACHE;
                case HWLOC_OBJ_L2CACHE:  return PRTE_BIND_TO_L2CACHE;
                case HWLOC_OBJ_L3CACHE:  return PRTE_BIND_TO_L3CACHE;
                case HWLOC_OBJ_CORE:     return PRTE_BIND_TO_CORE;
                case HWLOC_OBJ_PU:       return PRTE_BIND_TO_HWTHREAD;
                default:
                    /* ppr by node/machine: fall through to the nprocs rule */
                    break;
            }
            break;
        default:
            /* by-node, by-slot, dist, seq, user: fall through to nprocs rule */
            break;
    }

    /* non-object mappings: a couple of procs bind to a cpu, more to numa */
    if (opts->nprocs <= 2) {
        return hwt ? PRTE_BIND_TO_HWTHREAD : PRTE_BIND_TO_CORE;
    }
    return PRTE_BIND_TO_NUMA;
}

/* Map a bare binding policy to the hwloc object type the binder binds against
 * (opts->hwb), mirroring the job-level switch in prte_rmaps_base_map_job(). */
static hwloc_obj_type_t bind_to_hwb(prte_binding_policy_t bind)
{
    switch (PRTE_GET_BINDING_POLICY(bind)) {
        case PRTE_BIND_TO_PACKAGE:  return HWLOC_OBJ_PACKAGE;
        case PRTE_BIND_TO_NUMA:     return HWLOC_OBJ_NUMANODE;
        case PRTE_BIND_TO_L3CACHE:  return HWLOC_OBJ_L3CACHE;
        case PRTE_BIND_TO_L2CACHE:  return HWLOC_OBJ_L2CACHE;
        case PRTE_BIND_TO_L1CACHE:  return HWLOC_OBJ_L1CACHE;
        case PRTE_BIND_TO_CORE:     return HWLOC_OBJ_CORE;
        case PRTE_BIND_TO_HWTHREAD: return HWLOC_OBJ_PU;
        default:                    return HWLOC_OBJ_MACHINE;  /* BIND_TO_NONE */
    }
}

int prte_rmaps_base_resolve_app_options(prte_job_t *jdata,
                                               prte_app_context_t *app,
                                               prte_rmaps_options_t *opts)
{
    uint16_t u16;
    uint16_t *u16ptr = &u16;
    char *str;
    bool have_map, have_rank, have_bind;
    prte_mapping_policy_t appmap = 0;

    PRTE_HIDE_UNUSED_PARAMS(jdata);

    /* 1. PRTE_APP_MAPBY → opts->map plus the object type/depth and span/ordered
     * directives that flow from the mapping policy.  We store the bare policy
     * in opts->map (matching the job-level convention) and capture the full
     * value in appmap so the rank/bind defaults below can read its directives. */
    have_map = prte_get_attribute(&app->attributes, PRTE_APP_MAPBY, (void **)&u16ptr, PMIX_UINT16);
    if (have_map) {
        appmap = u16;
        opts->map = PRTE_GET_MAPPING_POLICY(appmap);
        opts->mapgiven = (0 != (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(appmap)));
        opts->mapspan = (0 != (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(appmap)));
        opts->ordered = (0 != (PRTE_MAPPING_ORDERED & PRTE_GET_MAPPING_DIRECTIVE(appmap)));
        switch (opts->map) {
            case PRTE_MAPPING_BYNODE:
            case PRTE_MAPPING_BYSLOT:
            case PRTE_MAPPING_PELIST:
            case PRTE_MAPPING_COLOCATE:
            case PRTE_MAPPING_BYDEVICE:
                opts->maptype = HWLOC_OBJ_MACHINE;
                opts->mapdepth = PRTE_BIND_TO_NONE;
                break;
            case PRTE_MAPPING_SEQ:
            case PRTE_MAPPING_BYUSER:
                opts->maptype = HWLOC_OBJ_MACHINE;
                opts->mapdepth = PRTE_BIND_TO_NONE;
                opts->userranked = true;
                break;
            case PRTE_MAPPING_BYNUMA:
                opts->maptype = HWLOC_OBJ_NUMANODE;
                opts->mapdepth = PRTE_BIND_TO_NUMA;
                break;
            case PRTE_MAPPING_BYPACKAGE:
                opts->maptype = HWLOC_OBJ_PACKAGE;
                opts->mapdepth = PRTE_BIND_TO_PACKAGE;
                break;
            case PRTE_MAPPING_BYL3CACHE:
                opts->maptype = HWLOC_OBJ_L3CACHE;
                opts->mapdepth = PRTE_BIND_TO_L3CACHE;
                break;
            case PRTE_MAPPING_BYL2CACHE:
                opts->maptype = HWLOC_OBJ_L2CACHE;
                opts->mapdepth = PRTE_BIND_TO_L2CACHE;
                break;
            case PRTE_MAPPING_BYL1CACHE:
                opts->maptype = HWLOC_OBJ_L1CACHE;
                opts->mapdepth = PRTE_BIND_TO_L1CACHE;
                break;
            case PRTE_MAPPING_BYCORE:
                opts->maptype = HWLOC_OBJ_CORE;
                opts->mapdepth = PRTE_BIND_TO_CORE;
                break;
            case PRTE_MAPPING_BYHWTHREAD:
                opts->maptype = HWLOC_OBJ_PU;
                opts->mapdepth = PRTE_BIND_TO_HWTHREAD;
                break;
            default:
                /* PPR and any other policy keep the job-level object/depth */
                break;
        }
    }

    /* 2. PPR pattern: read PRTE_APP_PPR, which carries both halves of what
     * the app asked for - "N:object". Fall back to the job's pprn/maptype
     * when the app named no pattern of its own. */
    if (PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(opts->map)) {
        str = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_PPR, (void **)&str, PMIX_STRING) &&
            NULL != str) {
            char **pk = PMIx_Argv_split(str, ':');
            if (2 != PMIx_Argv_count(pk) ||
                !ppr_object(pk[1], &opts->maptype, &opts->mapdepth, &opts->map_device)) {
                prte_show_help("help-prte-rmaps-ppr.txt", "invalid-ppr", true, str);
                PMIx_Argv_free(pk);
                free(str);
                return PRTE_ERR_SILENT;
            }
            opts->pprn = strtoul(pk[0], NULL, 10);
            PMIx_Argv_free(pk);
            free(str);
        }
    }

    /* 3. PRTE_APP_PES_PER_PROC → opts->cpus_per_rank */
    if (prte_get_attribute(&app->attributes, PRTE_APP_PES_PER_PROC, (void **)&u16ptr, PMIX_UINT16)) {
        opts->cpus_per_rank = u16;
    }

    /* 4. PRTE_APP_HWT_CPUS / PRTE_APP_CORE_CPUS → opts->use_hwthreads */
    if (prte_get_attribute(&app->attributes, PRTE_APP_HWT_CPUS, NULL, PMIX_BOOL)) {
        opts->use_hwthreads = true;
    } else if (prte_get_attribute(&app->attributes, PRTE_APP_CORE_CPUS, NULL, PMIX_BOOL)) {
        opts->use_hwthreads = false;
    }

    /* 5. PRTE_APP_CPUSET → opts->cpuset. The struct owns whatever is in
     * these two fields, so an app that names its own replaces (rather than
     * strands) the value the struct came in carrying. */
    str = NULL;
    if (prte_get_attribute(&app->attributes, PRTE_APP_CPUSET, (void **)&str, PMIX_STRING)) {
        if (NULL != opts->cpuset) {
            free(opts->cpuset);
        }
        opts->cpuset = str;
    }

    /* 6. PRTE_APP_MAP_FILE — read directly by seq/rank_file components via app->attributes */

    /* 7. PRTE_APP_MAP_DEVICE → opts->map_device */
    str = NULL;
    if (prte_get_attribute(&app->attributes, PRTE_APP_MAP_DEVICE, (void **) &str, PMIX_STRING)) {
        if (NULL != opts->map_device) {
            free(opts->map_device);
        }
        opts->map_device = str;
    }

    /* 8. PRTE_APP_MAP_INTERLEAVE → opts->map_interleave */
    str = NULL;
    if (prte_get_attribute(&app->attributes, PRTE_APP_MAP_INTERLEAVE, (void **) &str, PMIX_STRING)) {
        if (NULL != opts->map_interleave) {
            free(opts->map_interleave);
        }
        opts->map_interleave = str;
    }

    /* 9. PRTE_APP_MAP_SHARED → opts->map_shared */
    opts->map_shared = prte_get_attribute(&app->attributes, PRTE_APP_MAP_SHARED,
                                          NULL, PMIX_BOOL);

    /* 10. PRTE_APP_MAP_NDEV → opts->map_ndev */
    u16 = 0;
    if (prte_get_attribute(&app->attributes, PRTE_APP_MAP_NDEV, (void **) &u16ptr, PMIX_UINT16)) {
        opts->map_ndev = u16;
    }

    /* 11. PRTE_APP_BINDING_LIMIT → opts->limit */
    if (prte_get_attribute(&app->attributes, PRTE_APP_BINDING_LIMIT, (void **)&u16ptr, PMIX_UINT16)) {
        opts->limit = u16;
    }

    /* 9. Ranking: an explicit per-app --rank-by wins.  Otherwise, when the app
     * supplied its own mapping policy, derive the ranking default from that
     * policy rather than inheriting the job-level ranking (which followed the
     * job map).  When the app changed neither, the job-level ranking stands. */
    have_rank = prte_get_attribute(&app->attributes, PRTE_APP_RANKBY, (void **)&u16ptr, PMIX_UINT16);
    if (have_rank) {
        opts->rank = PRTE_GET_RANKING_POLICY(u16);
    } else if (have_map) {
        opts->rank = prte_rmaps_base_derive_ranking(appmap);
    }

    /* 10. Binding: an explicit per-app --bind-to wins (carrying its overload
     * directive).  Otherwise, when the app supplied its own mapping policy,
     * recompute the default binding from that policy.  We deliberately do not
     * disable binding here just because oversubscription is permitted: like
     * the job-level path, the derived binding is a default that the mappers
     * reset to BIND_TO_NONE only if this app genuinely oversubscribes a node.
     * When the app changed neither, the job-level binding stands. */
    have_bind = prte_get_attribute(&app->attributes, PRTE_APP_BINDTO, (void **)&u16ptr, PMIX_UINT16);
    opts->appbind = 0;
    if (have_bind) {
        opts->bind = PRTE_GET_BINDING_POLICY(u16);
        opts->overload = (0 != PRTE_BIND_OVERLOAD_ALLOWED(u16));
        /* keep the whole word: the app asked for this binding, so its own
         * directives - IF-SUPPORTED above all - describe it, not the job's */
        opts->appbind = u16;
    } else if (have_map) {
        opts->bind = prte_rmaps_base_derive_binding(opts);
    }

    /* keep the hwloc binding object in sync with the (possibly changed)
     * binding policy - bind_generic() binds against opts->hwb, not opts->bind,
     * so a stale hwb would bind every app to the job-level object */
    opts->hwb = bind_to_hwb(opts->bind);

    return PRTE_SUCCESS;
}

/* return the per-node scratch cpusets a mapper computed into an options
 * struct. The mappers recycle these as they walk the node list, so the
 * final node's pair is still held when the map returns. */
static void free_target(prte_rmaps_options_t *opts)
{
    if (NULL != opts->target) {
        hwloc_bitmap_free(opts->target);
        opts->target = NULL;
    }
}

static void free_cpusets(prte_rmaps_options_t *opts)
{
    if (NULL != opts->job_cpuset) {
        hwloc_bitmap_free(opts->job_cpuset);
        opts->job_cpuset = NULL;
    }
    free_target(opts);
}

/* return the strings an options struct owns. Both are read out of an
 * attribute, which hands back a copy, so whoever filled the struct owns
 * them - and a pe-list mapper consumes and replaces "cpuset" as it goes,
 * so the pointer here is whatever it left behind, not what we read. */
static void free_strings(prte_rmaps_options_t *opts)
{
    if (NULL != opts->cpuset) {
        free(opts->cpuset);
        opts->cpuset = NULL;
    }
    if (NULL != opts->map_device) {
        free(opts->map_device);
        opts->map_device = NULL;
    }
    if (NULL != opts->map_interleave) {
        free(opts->map_interleave);
        opts->map_interleave = NULL;
    }
}

/* Record the effective map/rank/bind policies for one app of a per-app
 * (MPMD) job so the map display can show a policy line per app. The bare
 * resolved policies in opts are overlaid onto snapshots of the job-level
 * policy values (captured before the per-app loop so a prior app's mapper
 * adjustments cannot bleed through), preserving the job-wide directive bits
 * - oversubscribe, span, and so on. The binding policy in opts reflects any
 * reset to BIND_TO_NONE the mapper made for a genuinely oversubscribed node.
 *
 * Binding is the exception to taking the job's directives: an app that gave
 * its own --bind-to described that binding itself, so its own word supplies
 * the directives. Otherwise an explicit "--bind-to numa" was reported as
 * NUMA:IF-SUPPORTED - the job's binding is a derived default whenever the
 * apps each gave their own, and a derived default is best-effort - telling
 * the user their requirement was merely a preference.
 *
 * These are stored as job-local attributes and are never packed or sent
 * off-node. */
static void record_resolved_app_policy(prte_app_context_t *app,
                                       prte_mapping_policy_t jobmap,
                                       prte_ranking_policy_t jobrank,
                                       prte_binding_policy_t jobbind,
                                       prte_rmaps_options_t *opts)
{
    prte_mapping_policy_t emap = jobmap;
    prte_ranking_policy_t erank = jobrank;
    prte_binding_policy_t ebind = (0 == opts->appbind) ? jobbind : opts->appbind;

    PRTE_SET_MAPPING_POLICY(emap, opts->map);
    if (opts->mapspan) {
        PRTE_SET_MAPPING_DIRECTIVE(emap, PRTE_MAPPING_SPAN);
    } else {
        PRTE_UNSET_MAPPING_DIRECTIVE(emap, PRTE_MAPPING_SPAN);
    }
    if (opts->ordered) {
        PRTE_SET_MAPPING_DIRECTIVE(emap, PRTE_MAPPING_ORDERED);
    } else {
        PRTE_UNSET_MAPPING_DIRECTIVE(emap, PRTE_MAPPING_ORDERED);
    }
    prte_set_attribute(&app->attributes, PRTE_APP_RESOLVED_MAPBY,
                       PRTE_ATTR_LOCAL, &emap, PMIX_UINT16);

    PRTE_SET_RANKING_POLICY(erank, opts->rank);
    prte_set_attribute(&app->attributes, PRTE_APP_RESOLVED_RANKBY,
                       PRTE_ATTR_LOCAL, &erank, PMIX_UINT16);

    PRTE_SET_BINDING_POLICY(ebind, opts->bind);
    if (opts->overload) {
        ebind |= PRTE_BIND_ALLOW_OVERLOAD;
    } else {
        ebind &= ~PRTE_BIND_ALLOW_OVERLOAD;
    }
    prte_set_attribute(&app->attributes, PRTE_APP_RESOLVED_BINDTO,
                       PRTE_ATTR_LOCAL, &ebind, PMIX_UINT16);
}

/* Record which mapping component placed an app - app_idx < 0 means the whole
 * job was placed by one of them. This is the only record PRRTE keeps of the
 * choice, and it is deliberately per-app: with each app's own policy deciding
 * which component claims it, two apps of one job can be placed by two
 * different mappers, and a single job-level name could only ever be half the
 * answer. Job-local, never packed - nothing off this daemon has any use for
 * it, and mapping happens nowhere else. */
static void record_mapper(prte_job_t *jdata, int app_idx, const char *mapper)
{
    prte_app_context_t *app;
    int n;

    if (NULL == mapper) {
        return;
    }
    for (n = 0; n < jdata->apps->size; n++) {
        if (0 <= app_idx && n != app_idx) {
            continue;
        }
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
        if (NULL == app) {
            continue;
        }
        prte_set_attribute(&app->attributes, PRTE_APP_LAST_MAPPER,
                           PRTE_ATTR_LOCAL, (void *) mapper, PMIX_STRING);
    }
}

/* Nobody would take this map. Report it - and say so specifically when the
 * framework has been restricted, because that is much the likeliest reason
 * and the generic "none of the available mappers was able to" sends the
 * reader looking at the mapping directive rather than at the MCA parameter
 * that removed the component which would have served it. We cannot refuse
 * the restriction: which components load is settled at framework open, long
 * before any job exists. What we can do is fail the job with the reason. */
static void report_no_mapper(prte_job_t *jdata, prte_app_context_t *app,
                             prte_rmaps_options_t *opts, int rc)
{
    prte_rmaps_base_selected_module_t *mod;
    char **names = NULL, *loaded;
    int nprocs;

    nprocs = (NULL == app) ? (int) jdata->num_procs : (int) app->num_procs;

    if (1 < pmix_list_get_size(&prte_rmaps_base.selected_modules)) {
        /* the full set is loaded, so the request is simply not one any of
         * them implements */
        prte_show_help("help-prte-rmaps-base.txt", "failed-map", true,
                       PRTE_ERROR_NAME(rc),
                       (NULL == app) ? "N/A" : app->app, nprocs,
                       prte_rmaps_base_print_mapping(opts->map),
                       prte_hwloc_base_print_binding(opts->bind));
        return;
    }

    PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules,
                      prte_rmaps_base_selected_module_t) {
        PMIx_Argv_append_nosize(&names, mod->component->pmix_mca_component_name);
    }
    loaded = (NULL == names) ? strdup("none") : PMIx_Argv_join(names, ',');
    prte_show_help("help-prte-rmaps-base.txt", "mapper-restricted", true,
                   prte_rmaps_base_print_mapping(opts->map),
                   prte_hwloc_base_print_binding(opts->bind),
                   (NULL == app) ? "N/A" : app->app, loaded);
    free(loaded);
    PMIx_Argv_free(names);
}

void prte_rmaps_base_map_job(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_node_t *node;
    pmix_proc_t *pptr;
    int rc = PRTE_SUCCESS;
    int n;
    bool did_map, pernode = false;
    bool bind_inherited = false;
    prte_rmaps_base_selected_module_t *mod;
    prte_job_t *parent = NULL;
    prte_job_t *iof_parent = NULL;
    bool iof_declined = false;
    prte_app_context_t *app;
    bool inherit = false;
    pmix_proc_t *nptr = NULL, *target_proc;
    char *tmp, **ck, **env;
    uint16_t u16 = 0, procs_per_target = 0;
    uint16_t *u16ptr = &u16;
    bool colocate_daemons = false;
    bool any_per_app = false;
    uint32_t next_vpid;
    bool colocate = false;
    prte_schizo_base_module_t *schizo;
    prte_rmaps_options_t options;
    /* per-app scratch for the MPMD dispatch path - at function scope so the
     * cleanup label can reclaim the cpusets a mapper left in it */
    prte_rmaps_options_t app_options;
    pmix_data_array_t *darray = NULL;
    pmix_list_t nodes;
    int slots, len;
    bool flag, *fptr;
    bool map_succeeded = false;
    prte_mapping_policy_t job_oversub = 0;

    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    // init options
    memset(&options, 0, sizeof(prte_rmaps_options_t));
    memset(&app_options, 0, sizeof(prte_rmaps_options_t));
    options.app_idx = -1;   /* -1 = map all apps (default) */
    options.stream = prte_rmaps_base_framework.framework_output;
    options.verbosity = 5;  // usual value for base-level functions
    // set and check convenience vars
    jdata = caddy->jdata;
    schizo = (prte_schizo_base_module_t*)jdata->schizo;
    if (NULL == schizo) {
        prte_show_help("help-prte-rmaps-base.txt", "missing-personality", true,
                       PRTE_JOBID_PRINT(jdata->nspace));
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }
    if (NULL == jdata->map) {
        jdata->map = PMIX_NEW(prte_job_map_t);
    }
    jdata->state = PRTE_JOB_STATE_MAP;
    fptr = &flag;

    /* check and set some general options */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        options.donotlaunch = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        options.dobind = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_BINDING_LIMIT, (void**) &u16ptr, PMIX_UINT16)) {
        options.limit = u16;
        // reset any prior counters
        prte_hwloc_base_reset_counters();
    }

    /* an app's mapping spec may carry a qualifier that describes the whole
     * job - take those off the apps now, while the job's own directives are
     * still as the user gave them, and hold the apps to agreeing about them.
     * The oversubscription answer is applied further down, once the job's
     * mapping policy has been resolved (that resolution assigns the whole
     * policy word and would otherwise overwrite it) */
    rc = prte_rmaps_base_hoist_job_directives(jdata, &job_oversub);
    if (PRTE_SUCCESS != rc) {
        // the error message has been printed
        jdata->exit_code = rc;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /*
     * Check for Colaunch
     */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_NODE, (void **) &u16ptr, PMIX_UINT16)) {
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_DEBUG_DAEMONS_PER_NODE value %u == 0\n", procs_per_target);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = true;
        colocate_daemons = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_PROC, (void **) &u16ptr, PMIX_UINT16)) {
        if (procs_per_target > 0) {
            pmix_output(0, "Error: Both PRTE_JOB_DEBUG_DAEMONS_PER_PROC and "
                           "PRTE_JOB_DEBUG_DAEMONS_PER_NODE provided.");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_DEBUG_DAEMONS_PER_PROC value %u == 0\n", procs_per_target);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = false;
        colocate_daemons = true;
    }
    if (colocate_daemons) {
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DEBUG_TARGET, (void **) &target_proc, PMIX_PROC)) {
            pmix_output(0, "Error: PRTE_JOB_DEBUG_DAEMONS_PER_PROC/NODE provided without a Debug Target\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* store the target as a pmix_data_array_t */
        PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_PROC);
        pptr = (pmix_proc_t*)darray->array;
        PMIX_XFER_PROCID(&pptr[0], target_proc);
    }

    /* asking for the colocation targets is what allocates them, so a job
     * that also asked to colocate daemons has to be refused before the
     * request is made - reading it into "darray" first would strand the
     * array built just above */
    if (colocate_daemons &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_PROCS, NULL, PMIX_DATA_ARRAY)) {
        pmix_output(0, "Error: Both colocate daemons and colocate procs were provided\n");
        jdata->exit_code = PRTE_ERR_BAD_PARAM;
        PRTE_ERROR_LOG(jdata->exit_code);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }
    if (!colocate_daemons &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_PROCS, (void**)&darray, PMIX_DATA_ARRAY)) {
        if (NULL == darray) {
            pmix_output(0, "Error: Colocate failed to provide procs\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        colocate = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_NPERNODE, (void **) &u16ptr, PMIX_UINT16)) {
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_COLOCATE_NUM_PROC WITH ZERO PROCS/TARGET\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_NPERPROC, (void **) &u16ptr, PMIX_UINT16)) {
        if (procs_per_target > 0) {
            pmix_output(0, "Error: Both PRTE_JOB_COLOCATE_NUM_PROC and "
                        "PRTE_JOB_COLOCATE_NUM_NODE provided.");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_COLOCATE_NUM_PROC WITH ZERO PROCS/TARGET\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = false;
    }

    if (colocate || colocate_daemons) {
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_COLOCATE);
        goto ranking;
    }

    /* if this is a dynamic job launch and they didn't explicitly
     * request inheritance, then don't inherit the launch directives */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &nptr, PMIX_PROC)) {
        if (NULL == nptr) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* if the launch proxy is me, then this is the initial launch from
         * a proxy scenario, so we don't really have a parent */
        if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, nptr->nspace)) {
            parent = NULL;
            /* we do allow inheritance of the defaults */
            inherit = true;
        } else if (NULL != (parent = prte_get_job_data_object(nptr->nspace))) {
            /* Output forwarding asks the same question with a DIFFERENT
             * default, so it cannot key off the `inherit` the arms below
             * resolve. prte_rmaps_base.inherit is false unless the user
             * says otherwise - a spawned job does not pick up mapping,
             * ranking and binding unless asked - whereas output
             * forwarding inherits unless it is REFUSED, on both sides of
             * the PMIx interface. Keying off the resolved value silenced
             * every spawned job.
             *
             * So capture the explicit refusal, and the parent, here:
             * two of the arms below null the parent out. */
            iof_parent = parent;
            iof_declined = prte_get_attribute(&jdata->attributes, PRTE_JOB_NOINHERIT,
                                              NULL, PMIX_BOOL) ||
                           prte_get_attribute(&parent->attributes, PRTE_JOB_NOINHERIT,
                                              NULL, PMIX_BOOL);
            if (PRTE_FLAG_TEST(parent, PRTE_JOB_FLAG_TOOL)) {
                // we don't inherit anything from tools as they were not
                // mapped by us
                inherit = false;
                parent = NULL;

            } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_INHERIT, NULL, PMIX_BOOL)) {
                inherit = true;
                // if they didn't specifically direct it not inherit, then pass this on to the child
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_NOINHERIT, NULL, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_INHERIT, NULL, PMIX_BOOL);
                }

            } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_NOINHERIT, NULL, PMIX_BOOL)) {
                inherit = false;
                parent = NULL;

            } else {
                inherit = prte_rmaps_base.inherit;
            }
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps: dynamic job %s %s inherit launch directives - parent %s",
                                PRTE_JOBID_PRINT(jdata->nspace), inherit ? "will" : "will not",
                                (NULL == parent) ? "N/A" : PRTE_JOBID_PRINT((parent->nspace)));

            /* Who receives this job's output. A spawned job is treated
             * the way its parent is being treated, so the daemons
             * interested in the parent's output are interested in this
             * one's - unless inheritance was explicitly refused.
             *
             * This is the routing half of that behavior; PMIx supplies
             * the matching half, deciding that a subscription covering
             * the parent job also covers its children. Both are needed:
             * this puts the bytes in front of the right PMIx server, and
             * that gets them from there to the tool. */
            if (iof_declined) {
                /* PMIx asks this again on the server the child's output
                 * ARRIVES at, which need not be this one, and it cannot
                 * reach the answer itself - so record it where the job's
                 * PMIx information is built and can carry it there. Set
                 * only to say NO: absence of the attribute, like absence
                 * of PMIX_IOF_INHERIT, means the job inherits. */
                prte_set_attribute(&jdata->attributes, PRTE_JOB_NO_IOF_INHERIT,
                                   PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
            } else if (NULL != iof_parent) {
                rc = pmix_bitmap_copy(&jdata->iof_daemons, &iof_parent->iof_daemons);
                if (PMIX_SUCCESS != rc) {
                    /* not fatal to the launch - the job simply starts with
                     * no inherited watchers, which is what it had before */
                    PMIX_ERROR_LOG(rc);
                }
            }
        } else {
            inherit = true;
        }
    } else {
        /* initial launch always takes on default MCA params for non-specified policies */
        inherit = true;
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: setting mapping policies for job %s inherit %s hwtcpus %s",
                        PRTE_JOBID_PRINT(jdata->nspace),
                        inherit ? "TRUE" : "FALSE",
                        options.use_hwthreads ? "TRUE" : "FALSE");

    /* Adopt an *explicitly given* inherited binding before the mapping
     * policy is derived, because deriving the mapping reads it: a binding
     * given with no mapping given means "map by the binding object" (see
     * prte_rmaps_base_set_default_mapping), and that test looks at
     * jdata->map->binding.
     *
     * A binding that came from the parent job or from the DVM-wide "bindto"
     * MCA parameter was not copied onto the job until long after the mapping
     * had been settled, so the test saw nothing and picked BYCORE. The
     * bind-upwards sanity check further down then refused the job outright,
     * because binding to a package while mapping by core is not allowed.
     * That made five of the eight values "bindto" documents - l1cache,
     * l2cache, l3cache, numa, package - unusable at DVM scope, while the
     * command-line spelling of the same request ("--bind-to package") mapped
     * BYPACKAGE and worked.
     *
     * Only the two arms that adopt a binding somebody actually *asked for*
     * belong up here. Deriving a binding from the mapping stays below, where
     * the mapping is known. */
    bind_inherited = false;
    if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding) && inherit) {
        if (NULL != parent) {
            jdata->map->binding = parent->map->binding;
            bind_inherited = true;
        } else if (PRTE_BINDING_POLICY_IS_SET(prte_hwloc_default_binding_policy)) {
            /* the user specified a default binding policy via MCA param, so
             * we use it - this can include a directive to overload */
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps[%d] default binding policy given", __LINE__);
            jdata->map->binding = prte_hwloc_default_binding_policy;
            bind_inherited = true;
        }
    }

    /* set the default mapping policy IFF it wasn't provided */
    if (!PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
        if (inherit) {
            if (NULL != parent) {
                // copy across the mapping policy
                jdata->map->mapping = parent->map->mapping;

                /* if not already assigned, inherit the parent's ppr */
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, NULL, PMIX_STRING)) {
                    /* get the parent job's ppr, if it had one */
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_PPR, (void **) &tmp, PMIX_STRING)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, tmp, PMIX_STRING);
                        free(tmp);
                    } else if (NULL != prte_rmaps_base.ppr) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, prte_rmaps_base.ppr, PMIX_STRING);
                    }
                }
                /* if not already assigned, inherit the parent's pes/proc */
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16)) {
                    /* get the parent job's pes/proc, if it had one */
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr, PMIX_UINT16)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, u16ptr, PMIX_UINT16);
                    } else if (0 < prte_rmaps_base.default_pes) {
                        u16 = prte_rmaps_base.default_pes;
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, u16ptr, PMIX_UINT16);
                    }
                }
                /* if not already assigned, inherit the parent's cpu designation */
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL) &&
                    !prte_get_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                    /* get the parent job's designation, if it had one */
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                    } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                    } else {
                        /* default */
                        if (prte_rmaps_base.hwthread_cpus) {
                            prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                        } else {
                            prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                        }
                    }
                }
                /* if not already assigned, inherit the parent's GPU support directive */
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_GPU_SUPPORT, NULL, PMIX_BOOL)) {
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_GPU_SUPPORT, (void **) &fptr, PMIX_BOOL)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_GPU_SUPPORT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                    }
                }
                /* if not already assigned, inherit the parent's output directives */
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, NULL, PMIX_BOOL)) {
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_TAG_OUTPUT, (void **) &fptr, PMIX_BOOL)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                    }
                }
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, NULL, PMIX_BOOL)) {
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, (void **) &fptr, PMIX_BOOL)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                    }
                }
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, NULL, PMIX_BOOL)) {
                    if (prte_get_attribute(&parent->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, (void **) &fptr, PMIX_BOOL)) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                    }
                }

                // copy over any env directives, but do not overwrite anything already specified
                inherit_env_directives(jdata, parent, nptr);

            } else {
                // bring over the MCA param defaults, where set and not already specified for this job
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, NULL, PMIX_STRING) &&
                    NULL != prte_rmaps_base.ppr) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, prte_rmaps_base.ppr, PMIX_STRING);
                }
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16) &&
                    0 < prte_rmaps_base.default_pes) {
                    u16 = prte_rmaps_base.default_pes;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, u16ptr, PMIX_UINT16);
                    options.cpus_per_rank = u16;
                }
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL) &&
                    !prte_get_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                    if (prte_rmaps_base.hwthread_cpus) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                    } else {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                    }
                }

                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, NULL, PMIX_STRING) &&
                    NULL != prte_rmaps_base.file) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_FILE, PRTE_ATTR_GLOBAL,
                                       prte_rmaps_base.file, PMIX_STRING);
                }

                if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
                    jdata->map->mapping = prte_rmaps_base.mapping;
                } else {
                    // let the job's personality set the default mapping behavior
                    if (NULL != schizo->set_default_mapping) {
                        rc = schizo->set_default_mapping(jdata, &options);
                    } else {
                        rc = prte_rmaps_base_set_default_mapping(jdata, &options);
                    }
                    if (PRTE_SUCCESS != rc) {
                        // the error message should have been printed
                        jdata->exit_code = rc;
                        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                        goto cleanup;
                    }
                }
            }
        } else {
            // let the job's personality set the default mapping behavior
            if (NULL != schizo->set_default_mapping) {
                rc = schizo->set_default_mapping(jdata, &options);
            } else {
                rc = prte_rmaps_base_set_default_mapping(jdata, &options);
            }
            if (PRTE_SUCCESS != rc) {
                // the error message should have been printed
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
        }
    }
    if (NULL != nptr) {
        PMIX_PROC_RELEASE(nptr);
        nptr = NULL;
    }

    /* apply the oversubscription answer hoisted off the apps. It goes on
     * before the parent's flag is considered below: the user saying so on
     * this command line outranks whatever the parent job was given */
    if (0 != job_oversub) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & job_oversub) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        }
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
    }

    /* we always inherit a parent's oversubscribe flag unless the job assigned it */
    if (NULL != parent &&
        !(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(parent->map->mapping)) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }

    // forward the environment if requested to do so
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, (void **) &fptr, PMIX_BOOL)) {
        if (flag) {
            // forward the environment
            for (n = 0; n < jdata->apps->size; n++) {
                app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
                if (NULL == app) {
                    continue;
                }
                env = pmix_environ_merge(prte_launch_environ, app->env);
                PMIx_Argv_free(app->env);
                app->env = env;
            }
        }
    } else if (NULL != parent) {
        /* we always inherit a parent's fwd environment directive unless the job assigned it */
        if (prte_get_attribute(&parent->attributes, PRTE_JOB_FWD_ENVIRONMENT, (void **) &fptr, PMIX_BOOL)) {
            if (flag) {
                // update the child's flag so any subsequent children can inherit it
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                // forward the environment
                for (n = 0; n < jdata->apps->size; n++) {
                    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
                    if (NULL == app) {
                        continue;
                    }
                    env = pmix_environ_merge(prte_launch_environ, app->env);
                    PMIx_Argv_free(app->env);
                    app->env = env;
                }
            }
        }
    }

    /* set some convenience params */
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void**)&options.cpuset, PMIX_STRING);
    prte_get_attribute(&jdata->attributes, PRTE_JOB_MAP_DEVICE, (void**)&options.map_device, PMIX_STRING);
    prte_get_attribute(&jdata->attributes, PRTE_JOB_MAP_INTERLEAVE, (void**)&options.map_interleave, PMIX_STRING);
    options.map_shared = prte_get_attribute(&jdata->attributes, PRTE_JOB_MAP_SHARED, NULL, PMIX_BOOL);
    {
        uint16_t nd = 0, *ndptr = &nd;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_MAP_NDEV, (void**)&ndptr, PMIX_UINT16)) {
            options.map_ndev = nd;
        }
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr, PMIX_UINT16)) {
        options.cpus_per_rank = u16;
    } else {
        options.cpus_per_rank = 1;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        options.use_hwthreads = true;
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PROCESSORS, (void*)&tmp, PMIX_STRING)) {
        prte_ras_base_display_cpus(jdata, tmp);
        free(tmp);
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, (void **) &tmp, PMIX_STRING)) {
        ck = PMIx_Argv_split(tmp, ':');
        if (2 != PMIx_Argv_count(ck)) {
            /* must provide a specification */
            prte_show_help("help-prte-rmaps-ppr.txt", "invalid-ppr", true, tmp);
            PMIx_Argv_free(ck);
            free(tmp);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* compute the #procs per resource */
        options.pprn = strtoul(ck[0], NULL, 10);
        if (!ppr_object(ck[1], &options.maptype, &options.mapdepth, &options.map_device)) {
            /* unknown spec */
            prte_show_help("help-prte-rmaps-ppr.txt", "unrecognized-ppr-option", true,
                           ck[1], tmp);
            free(tmp);
            PMIx_Argv_free(ck);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        free(tmp);
        PMIx_Argv_free(ck);
    }

    /* add up all the expected procs */
    for (n = 0; n < jdata->apps->size; n++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
        if (NULL == app ) {
            continue;
        }
        if (0 < app->num_procs) {
            options.nprocs += app->num_procs;
            continue;
        }
        PRTE_FLAG_SET(app, PRTE_APP_FLAG_COMPUTED);

        if (PRTE_MAPPING_SEQ == PRTE_GET_MAPPING_POLICY(jdata->map->mapping) ||
            PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(jdata->map->mapping) ||
            PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            // these mappers compute their #procs as they go
            continue;
        }

       if (NULL != options.cpuset) {
            ck = PMIx_Argv_split(options.cpuset, ',');
            app->num_procs = PMIx_Argv_count(ck);
            PMIx_Argv_free(ck);
        } else {
            /*
             * get the target nodes for this app - the base function
             * will take any host or hostfile directive into account
             */
            PMIX_CONSTRUCT(&nodes, pmix_list_t);
            rc = prte_rmaps_base_get_target_nodes(&nodes, &slots,
                                                  jdata, app, jdata->map->mapping,
                                                  true, true, false);
            if (PRTE_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&nodes);
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            if (1 < options.cpus_per_rank) {
                // compute the number of cpus on each node
                len = 0;
                PMIX_LIST_FOREACH (node, &nodes, prte_node_t) {
                    if (options.use_hwthreads) {
                        len += prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                  HWLOC_OBJ_PU) / options.cpus_per_rank;
                    } else {
                        len += prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                  HWLOC_OBJ_CORE) / options.cpus_per_rank;
                    }
                }
                app->num_procs = len;
                // ensure we always wind up with at least one proc
                if (0 == app->num_procs) {
                    app->num_procs = 1;
                } else if (slots < app->num_procs) {
                    app->num_procs = slots;
                }
            } else {
                app->num_procs = slots;
            }
            PMIX_LIST_DESTRUCT(&nodes);
        }
        options.nprocs += app->num_procs;
    }

    /* check for oversubscribe directives */
    if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                   & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }
    if (!(PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        options.oversubscribe = true;
    }

    /* check for no-use-local directive */
    if (prte_ras_base.launch_orted_on_hn) {
        /* must override any setting */
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
    } else if (!(PRTE_MAPPING_LOCAL_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (inherit
            && (PRTE_MAPPING_NO_USE_LOCAL & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
        }
    }

ranking:
    options.map = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
    if (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        options.mapspan = true;
    }
    if (PRTE_MAPPING_ORDERED & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        options.ordered = true;
    }

    switch (options.map) {
        case PRTE_MAPPING_BYNODE:
        case PRTE_MAPPING_BYSLOT:
        case PRTE_MAPPING_PELIST:
        case PRTE_MAPPING_COLOCATE:
            options.mapdepth = PRTE_BIND_TO_NONE;
            options.maptype = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_MAPPING_BYDEVICE:
            /* A device's locality is not known until the node is - it is
             * NUMA-sized on one machine and package-sized on another, and is
             * frequently an hwloc Group, which has no position in the
             * PRTE_BIND_TO_* ladder at all. So there is no value mapdepth
             * could take that would make the fixed "cannot bind above the
             * map" check below mean the right thing. Leave it at NONE so
             * that check does not fire on a basis nobody knows yet; the
             * device mapper applies the real ceiling per node, against the
             * locality it actually found. */
            options.mapdepth = PRTE_BIND_TO_NONE;
            options.maptype = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_MAPPING_BYUSER:
        case PRTE_MAPPING_SEQ:
            options.mapdepth = PRTE_BIND_TO_NONE;
            options.userranked = true;
            options.maptype = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_MAPPING_BYNUMA:
            options.mapdepth = PRTE_BIND_TO_NUMA;
            options.maptype = HWLOC_OBJ_NUMANODE;
            break;
        case PRTE_MAPPING_BYPACKAGE:
            options.mapdepth = PRTE_BIND_TO_PACKAGE;
            options.maptype = HWLOC_OBJ_PACKAGE;
            break;
        case PRTE_MAPPING_BYL3CACHE:
            options.mapdepth = PRTE_BIND_TO_L3CACHE;
            options.maptype = HWLOC_OBJ_L3CACHE;
            break;
        case PRTE_MAPPING_BYL2CACHE:
            options.mapdepth = PRTE_BIND_TO_L2CACHE;
            options.maptype = HWLOC_OBJ_L2CACHE;
            break;
        case PRTE_MAPPING_BYL1CACHE:
            options.mapdepth = PRTE_BIND_TO_L1CACHE;
            options.maptype = HWLOC_OBJ_L1CACHE;
            break;
        case PRTE_MAPPING_BYCORE:
            if (1 < options.cpus_per_rank &&
                !options.use_hwthreads) {
                /* we cannot support this operation as there is only one
                 * cpu in a core */
                prte_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true,
                               options.cpus_per_rank, 1,
                               prte_rmaps_base_print_mapping(options.map));
                jdata->exit_code = PRTE_ERR_SILENT;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            options.mapdepth = PRTE_BIND_TO_CORE;
            options.maptype = HWLOC_OBJ_CORE;
            break;
        case PRTE_MAPPING_BYHWTHREAD:
            if (1 < options.cpus_per_rank) {
                /* we cannot support this operation as there is only one
                 * cpu in a core */
                prte_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true,
                               options.cpus_per_rank, 1,
                               prte_rmaps_base_print_mapping(options.map));
                jdata->exit_code = PRTE_ERR_SILENT;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            options.mapdepth = PRTE_BIND_TO_HWTHREAD;
            options.maptype = HWLOC_OBJ_PU;
            break;
        case PRTE_MAPPING_PPR:
            break;
        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
    }

    if (options.userranked) {
        /* must rank by user */
        PRTE_SET_RANKING_POLICY(jdata->map->ranking, PRTE_RANKING_BYUSER);
    } else {
        /* set the default ranking policy IFF it wasn't provided */
        if (!PRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
            did_map = false;
            if (inherit) {
                if (NULL != parent) {
                    jdata->map->ranking = parent->map->ranking;
                    did_map = true;
                } else if (PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(prte_rmaps_base.ranking)) {
                    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps ranking given by MCA param");
                    jdata->map->ranking = prte_rmaps_base.ranking;
                    did_map = true;
                }
            }
            if (!did_map) {
                // let the job's personality set the default ranking behavior
                if (NULL != schizo->set_default_ranking) {
                    rc = schizo->set_default_ranking(jdata, &options);
                } else {
                    rc = prte_rmaps_base_set_default_ranking(jdata, &options);
                }
                if (PRTE_SUCCESS != rc) {
                    // the error message should have been printed
                    jdata->exit_code = rc;
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                    goto cleanup;
                }
            }
        }
    }
    options.rank = PRTE_GET_RANKING_POLICY(jdata->map->ranking);
    /* if we are ranking by FILL or SPAN, then we must map by an object */
    if ((PRTE_RANK_BY_SPAN == options.rank ||
         PRTE_RANK_BY_FILL == options.rank) &&
        PRTE_MAPPING_PPR != options.map) {
        if (options.map < PRTE_MAPPING_BYNUMA ||
            options.map > PRTE_MAPPING_BYHWTHREAD) {
            prte_show_help("help-prte-rmaps-base.txt", "must-map-by-obj",
                           true, prte_rmaps_base_print_mapping(options.map),
                           prte_rmaps_base_print_ranking(options.rank));
            jdata->exit_code = PRTE_ERR_SILENT;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }
    /* define the binding policy for this job - if the user specified one
     * already (e.g., during the call to comm_spawn), then we don't
     * override it.
     *
     * Note: we do NOT disable binding here merely because oversubscription
     * is permitted (options.oversubscribe). That flag only records that the
     * job is *allowed* to oversubscribe, not that any node actually will be -
     * a job that fits comfortably within its slots must still bind. The
     * mappers detect genuine oversubscription/overloading as they place procs
     * and reset an unset (default) binding to BIND_TO_NONE for the affected
     * node(s) at that point. Forcing NONE here off the permission alone broke
     * binding for non-oversubscribed jobs whenever a default oversubscribe
     * policy was in effect.
     *
     * The two inheritance arms that used to live here now run before the
     * mapping policy is derived - see "bind_inherited" above - because that
     * derivation reads the binding. What is left is the case that genuinely
     * cannot move: deriving a binding *from* the mapping. */
    if (!bind_inherited && !PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
        // let the job's personality set the default binding behavior
        if (NULL != schizo->set_default_binding) {
            rc = schizo->set_default_binding(jdata, &options);
        } else {
            rc = prte_hwloc_base_set_default_binding(jdata, &options);
        }
        if (PRTE_SUCCESS != rc) {
            // the error message should have been printed
            jdata->exit_code = rc;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }
    options.overload = PRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding);
    options.bind = PRTE_GET_BINDING_POLICY(jdata->map->binding);
    /* sanity check */
    if (options.mapdepth > options.bind &&
        PRTE_BIND_TO_NONE != options.bind) {
        /* we cannot bind to objects higher in the
         * topology than where we mapped */
        prte_show_help("help-prte-hwloc-base.txt", "bind-upwards", true,
                       prte_rmaps_base_print_mapping(options.map),
                       prte_hwloc_base_print_binding(options.bind));
        /* the message is out - rc still holds the SUCCESS of the last call
         * that ran, so do not pass it off as this failure's code */
        jdata->exit_code = PRTE_ERR_SILENT;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }
    switch (options.bind) {
        case PRTE_BIND_TO_NONE:
            options.hwb = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_BIND_TO_PACKAGE:
            options.hwb = HWLOC_OBJ_PACKAGE;
            break;
        case PRTE_BIND_TO_NUMA:
            options.hwb = HWLOC_OBJ_NUMANODE;
            break;
        case PRTE_BIND_TO_L3CACHE:
            options.hwb = HWLOC_OBJ_L3CACHE;
            break;
        case PRTE_BIND_TO_L2CACHE:
            options.hwb = HWLOC_OBJ_L2CACHE;
            break;
        case PRTE_BIND_TO_L1CACHE:
            options.hwb = HWLOC_OBJ_L1CACHE;
            break;
        case PRTE_BIND_TO_CORE:
            options.hwb = HWLOC_OBJ_CORE;
            break;
        case PRTE_BIND_TO_HWTHREAD:
            options.hwb = HWLOC_OBJ_PU;
            break;
        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
    }
    if (1 < options.cpus_per_rank ||
        options.ordered) {
        /* REQUIRES binding to cpu */
        if (PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            if (PRTE_BIND_TO_CORE != options.bind &&
                PRTE_BIND_TO_HWTHREAD != options.bind) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-combination", true,
                               "binding", prte_hwloc_base_print_binding(options.bind));
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                jdata->exit_code = PRTE_ERR_BAD_PARAM;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            /* ensure the cpu usage setting matches the provided bind directive */
            if (PRTE_BIND_TO_HWTHREAD == options.bind) {
                options.use_hwthreads = true;
            } else {
                options.use_hwthreads = false;
            }
        } else {
            if (options.use_hwthreads) {
                PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
                options.bind = PRTE_BIND_TO_HWTHREAD;
            } else {
                PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
                options.bind = PRTE_BIND_TO_CORE;
            }
        }
    }

    /* if we are not going to launch, then we need to set any
     * undefined topologies to match our own so the mapper
     * can operate
     */
    if (options.donotlaunch) {
        prte_topology_t *t0;
        if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            /* no PMIX_RELEASE(caddy) here - the cleanup label below owns it,
             * and releasing twice dropped a live event caddy */
            jdata->exit_code = PRTE_ERR_NOT_FOUND;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        t0 = node->topology;
        for (int i = 1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                /* the node holds a counted reference to the topology */
                PMIX_RETAIN(t0);
                node->topology = t0;
            }
        }
    }

    /* record whether the mapping policy is the user's or one we derived -
     * a mapper that only claims a job nobody described has to be able to
     * tell, and once dispatch is per-app the question is per-app too */
    options.mapgiven = (0 != (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)));

    /* scan for per-app mapping directives */
    for (n = 0; n < jdata->apps->size; n++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
        if (NULL == app) {
            continue;
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_MAPBY, NULL, PMIX_UINT16) ||
            prte_get_attribute(&app->attributes, PRTE_APP_RANKBY, NULL, PMIX_UINT16) ||
            prte_get_attribute(&app->attributes, PRTE_APP_BINDTO, NULL, PMIX_UINT16)) {
            any_per_app = true;
            break;
        }
    }

    if (colocate_daemons || colocate) {
        /* This is a colocation request, so we don't run any mapping modules */
        if (procs_per_target == 0) {
            pmix_output(0, "Error: COLOCATION REQUESTED WITH ZERO PROCS/TARGET\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        rc = map_colocate(jdata, colocate_daemons, pernode, darray, procs_per_target, &options);
        if (PRTE_SUCCESS != rc) {
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        did_map = true;
    } else if (!any_per_app) {
        /* cycle thru the available mappers until one agrees to map
         * the job
         */
        did_map = false;
        PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules, prte_rmaps_base_selected_module_t)
        {
            if (PRTE_SUCCESS == (rc = mod->module->map_job(jdata, &options)) ||
                PRTE_ERR_RESOURCE_BUSY == rc) {
                /* the base records who did the mapping, not the mapper - a
                 * mapper that stamps itself on entry cannot know it will
                 * still be the answer. Every app of a whole-job map was
                 * placed by the same component, so they all get the name */
                record_mapper(jdata, -1, mod->component->pmix_mca_component_name);
                did_map = true;
                break;
            }
            /* mappers return "next option" if they didn't attempt to
             * map the job. anything else is a true error.
             */
            if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
        }
        if (!did_map) {
            report_no_mapper(jdata, NULL, &options, rc);
            jdata->exit_code = PRTE_ERR_SILENT;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    } else {
        /* per-app dispatch: call mappers once per app with per-app options */
        did_map = false;
        next_vpid = 0;
        /* snapshot the job-level policies before any per-app mapper runs so the
         * recorded per-app policies carry the job-wide directives without a
         * prior app's mapper adjustments bleeding through */
        prte_mapping_policy_t job_map = jdata->map->mapping;
        prte_ranking_policy_t job_rank = jdata->map->ranking;
        prte_binding_policy_t job_bind = jdata->map->binding;
        for (n = 0; n < jdata->apps->size; n++) {
            app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
            if (NULL == app) {
                continue;
            }

            /* hand back whatever the previous app's map left behind before
             * this app overwrites it */
            free_cpusets(&app_options);
            free_strings(&app_options);
            app_options = options;   /* shallow copy of job defaults */
            /* the copy must not inherit ownership of the job-level cpusets -
             * the mappers compute their own per node */
            app_options.job_cpuset = NULL;
            app_options.target = NULL;
            /* nor of the job-level string: a pe-list mapper frees and
             * rewrites "cpuset" as it places procs, so each app needs its
             * own copy rather than a second pointer to the job's */
            app_options.cpuset = (NULL == options.cpuset) ? NULL : strdup(options.cpuset);
            app_options.map_device = (NULL == options.map_device) ? NULL
                                                                  : strdup(options.map_device);
            app_options.map_interleave = (NULL == options.map_interleave) ? NULL
                                                                          : strdup(options.map_interleave);
            app_options.map_shared = options.map_shared;
            app_options.map_ndev = options.map_ndev;
            app_options.app_idx = n;
            /* where this app's ranks start: the mappers that number their
             * own procs need the cursor the base is threading, or every app
             * numbers itself from zero */
            app_options.start_vpid = next_vpid;
            /* the default-binding nprocs rule keys off this app's own proc
             * count, not the job-wide total inherited from options.nprocs.
             * (the mappers overwrite options.nprocs per node as they run, so
             * this only feeds the pre-map binding-default derivation.) */
            app_options.nprocs = app->num_procs;

            rc = prte_rmaps_base_resolve_app_options(jdata, app, &app_options);
            if (PRTE_SUCCESS != rc) {
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }

            bool app_did_map = false;
            const char *app_mapper = NULL;
            PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules,
                              prte_rmaps_base_selected_module_t) {
                rc = mod->module->map_job(jdata, &app_options);
                if (PRTE_SUCCESS == rc) {
                    app_did_map = true;
                    app_mapper = mod->component->pmix_mca_component_name;
                    break;
                }
                if (PRTE_ERR_RESOURCE_BUSY == rc) {
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                    goto cleanup;
                }
                if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                    jdata->exit_code = rc;
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                    goto cleanup;
                }
            }
            if (!app_did_map) {
                report_no_mapper(jdata, app, &app_options, rc);
                jdata->exit_code = PRTE_ERR_SILENT;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            /* rank this app's procs */
            rc = prte_rmaps_base_compute_vpids(jdata, &app_options, n, &next_vpid);
            if (PRTE_SUCCESS != rc) {
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            /* record this app's effective policies, and who applied them,
             * for the map display */
            record_resolved_app_policy(app, job_map, job_rank, job_bind, &app_options);
            record_mapper(jdata, n, app_mapper);
            did_map = true;
        }
    }

    if (did_map && PRTE_ERR_RESOURCE_BUSY == rc) {
        /* the map was done but nothing could be mapped
         * for launch as all the resources were busy
         */
        prte_show_help("help-prte-rmaps-base.txt", "cannot-launch", true);
        jdata->exit_code = rc;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* if we get here without doing the map, or with zero procs in
     * the map, then that's an error
     */
    if (!did_map || 0 == jdata->num_procs || 0 == jdata->map->num_nodes) {
        prte_show_help("help-prte-rmaps-base.txt", "failed-map", true,
                       PRTE_ERROR_NAME(rc),
                       "N/A",
                       jdata->num_procs,
                       prte_rmaps_base_print_mapping(options.map),
                       prte_hwloc_base_print_binding(options.bind));
        jdata->exit_code = -PRTE_JOB_STATE_MAP_FAILED;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* set the offset so shared memory components can potentially
     * connect to any spawned jobs
     */
    jdata->offset = prte_total_procs;
    /* track the total number of procs launched by us */
    prte_total_procs += jdata->num_procs;

    /* if it is a dynamic spawn, save the bookmark on the parent's job too */
    if (!PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        if (NULL != (parent = prte_get_job_data_object(jdata->originator.nspace))) {
            parent->bookmark = jdata->bookmark;
        }
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        /* display the map */
        prte_rmaps_base_display_map(jdata);
    } else if (options.donotlaunch &&
               prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_BINDINGS, NULL, PMIX_BOOL)) {
        prte_rmaps_base_report_bindings(jdata, &options);
    }

    /* set the job state to the next position */
    map_succeeded = true;
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_COMPLETE);

cleanup:
    /* Every failure path above reaches this label via "goto cleanup" after
     * activating PRTE_JOB_STATE_MAP_FAILED, but only some of them set an exit
     * code first. Because the job state is activated asynchronously, we cannot
     * read jdata->state here to tell success from failure - so we rely on the
     * map_succeeded flag, which is only set on the success fall-through. Ensure
     * a failed map always leaves a non-zero exit_code so the eventual
     * PRTE_UPDATE_EXIT_STATUS() reflects the failure rather than defaulting
     * to 0 (success). Do not overwrite a more specific code already set. */
    if (!map_succeeded && 0 == jdata->exit_code) {
        jdata->exit_code = -PRTE_JOB_STATE_MAP_FAILED;
    }
    /* release the colocation target array if one was provided/created */
    PMIX_DATA_ARRAY_FREE(darray);
    /* reset any node map flags we used so the next job will start clean */
    for (int i = 0; i < jdata->map->nodes->size; i++) {
        if (NULL != (node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, i))) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }
    free_cpusets(&options);
    free_cpusets(&app_options);
    free_strings(&options);
    free_strings(&app_options);
    if (NULL != nptr) {
        PMIX_PROC_RELEASE(nptr);
        nptr = NULL;
    }
    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_rmaps_base_display_map(prte_job_t *jdata)
{
    pmix_proc_t source;
    char *tmp;

    prte_map_print(&tmp, jdata);
    PMIX_LOAD_PROCID(&source, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, tmp);
}

void prte_rmaps_base_report_bindings(prte_job_t *jdata,
                                     prte_rmaps_options_t *options)
{
    int n;
    prte_proc_t *proc;
    char **cache = NULL;
    char *out, *tmp;
    pmix_proc_t source;
    bool physical;

    // see if we are to report physical (vs logical) cpu IDs
    physical = prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL, PMIX_BOOL);
    for (n=0; n < jdata->procs->size; n++) {
        proc = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, n);
        if (NULL == proc) {
            continue;
        }
        if (NULL == proc->cpuset) {
            pmix_asprintf(&out, "Proc %s Node %s is UNBOUND",
                          PRTE_NAME_PRINT(&proc->name), proc->node->name);
        } else {
            hwloc_bitmap_list_sscanf(prte_rmaps_base.available, proc->cpuset);
            tmp = prte_hwloc_base_cset2str(prte_rmaps_base.available,
                                           options->use_hwthreads,
                                           physical,
                                           proc->node->topology->topo);
            pmix_asprintf(&out, "Proc %s Node %s bound to %s",
                          PRTE_NAME_PRINT(&proc->name),
                          proc->node->name, tmp);
            free(tmp);
        }
        PMIx_Argv_append_nosize(&cache, out);
        free(out);
    }

    if (NULL == cache) {
        out = strdup("Error: job has no procs");
    } else {
        /* add a blank line with \n on it so IOF will output the last line */
        PMIx_Argv_append_nosize(&cache, "");
        out = PMIx_Argv_join(cache, '\n');
        PMIx_Argv_free(cache);
    }
    PMIX_LOAD_PROCID(&source, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, out);
}

static int map_colocate(prte_job_t *jdata,
                        bool daemons, bool pernode,
                        pmix_data_array_t *darray,
                        uint16_t procs_per_target,
                        prte_rmaps_options_t *options)
{
    char *tmp;
    pmix_status_t rc;
    size_t n, nprocs;
    pmix_proc_t *procs;
    prte_job_t *target_jdata;
    prte_job_map_t *target_map, *map;
    prte_app_context_t *app;
    int i, j, ret, cnt;
    pmix_list_t targets;
    prte_proc_t *proc;
    prte_node_t *node, *nptr, *n2;

    if (4 < pmix_output_get_verbosity(prte_rmaps_base_framework.framework_output)) {
        rc = PMIx_Data_print(&tmp, NULL, darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            pmix_output(0, "%s rmaps: mapping job %s: Colocate with UNPRINTABLE (%s)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_JOBID_PRINT(jdata->nspace),
                        PMIx_Error_string(rc));
        } else {
            pmix_output(0, "%s rmaps: mapping job %s: Colocate with\n  %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_JOBID_PRINT(jdata->nspace), tmp);
        }
        free(tmp);
    }
    procs = (pmix_proc_t*)darray->array;
    nprocs = darray->size;
    map = jdata->map;
    if (daemons) {
        /* daemons are never bound and always rank by-slot */
        PRTE_SET_BINDING_POLICY(map->binding, PRTE_BIND_TO_NONE);
        PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
    }
    jdata->num_procs = 0;

    /* create a list of the target nodes */
    PMIX_CONSTRUCT(&targets, pmix_list_t);
    /* need to ensure each node only appears _once_ on the list */
    for (n=0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD == procs[n].rank) {
            target_jdata = prte_get_job_data_object(procs[n].nspace);
            if (NULL == target_jdata) {
                pmix_output(0, "Unable to find app job %s\n", procs[n].nspace);
                ret = PRTE_ERR_BAD_PARAM;
                goto done;
            }
            target_map = target_jdata->map;
            for (i = 0; i < target_map->nodes->size; i++) {
                node = (prte_node_t*)pmix_pointer_array_get_item(target_map->nodes, i);
                if (NULL == node) {
                    continue;
                }
                if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                    PMIX_RETAIN(node);
                    pmix_list_append(&targets, &node->super);
                }
            }
            continue;
        }
        /* not a wildcard rank */
        proc = prte_get_proc_object(&procs[n]);
        if (NULL == proc) {
            pmix_output(0, "Unable to find target process %s\n", PMIX_NAME_PRINT(&procs[n]));
            ret = PRTE_ERR_BAD_PARAM;
            goto done;
        }
        if (NULL == proc->node) {
            pmix_output(0, "Target process %s has not been mapped to a node\n", PMIX_NAME_PRINT(&procs[n]));
            ret = PRTE_ERR_BAD_PARAM;
            goto done;
        }
        node = proc->node;
        if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
            /* add this node to the list */
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
            PMIX_RETAIN(node);
            pmix_list_append(&targets, &node->super);
        }
    }

    // clear the flags
    PMIX_LIST_FOREACH(nptr, &targets, prte_node_t) {
        PRTE_FLAG_UNSET(nptr, PRTE_NODE_FLAG_MAPPED);
    }


    if (pernode) {
        /* cycle across the target nodes and place the specified
         * number of procs on each one */
        PMIX_LIST_FOREACH_SAFE(nptr, n2, &targets, prte_node_t) {
            // setup the mapping options
            options->ncpus = prte_rmaps_base_get_ncpus(nptr, NULL, options);
            /* the available cpus are in the scratch location */
            free_target(options);
            options->target = hwloc_bitmap_dup(prte_rmaps_base.available);
            options->nprocs = procs_per_target;
           // Assign N procs per node for each app_context
            for (i=0; i < jdata->apps->size; i++) {
                app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, i);
                if (NULL == app) {
                    continue;
                }
                // is there room on this node? daemons don't count
                if (!daemons) {
                    cnt = nptr->slots_inuse + procs_per_target;
                    // first check the absolute limit
                    if (0 != nptr->slots_max && nptr->slots_max < cnt) {
                        // violates the max limit
                        ret = PRTE_ERR_OUT_OF_RESOURCE;
                        goto done;
                    }
                    if (nptr->slots < cnt) {
                        // oversubscribed - we can still fit if they allow oversubscription
                        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(map->mapping)) {
                            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                           app->num_procs, app->app, prte_process_info.nodename);
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                            ret = PRTE_ERR_SILENT;
                            goto done;
                        }
                        // we can use it oversubscribed - so mark it
                        PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                        PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                    }
                }
                // Map the node to this job
                if (!PRTE_FLAG_TEST(nptr, PRTE_NODE_FLAG_MAPPED)) {
                    PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_MAPPED);
                    PMIX_RETAIN(nptr);
                    pmix_pointer_array_add(map->nodes, nptr);
                    map->num_nodes += 1;
                    options->nnodes++;
                }
                // map the procs
                for (j = 0; j < procs_per_target; ++j) {
                    proc = prte_rmaps_base_setup_proc(jdata, app->idx, nptr, NULL, options);
                    if (NULL == proc) {
                        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                        ret = PRTE_ERR_OUT_OF_RESOURCE;
                        goto done;
                    }
                    jdata->num_procs += 1;
                    app->num_procs += 1;
                    PMIX_RELEASE(proc);
                }
            }
        }

        /* calculate the ranks for this job */
        ret = prte_rmaps_base_compute_vpids(jdata, options, -1, NULL);
        goto done;
    }

    /* handle the case of colocate by process */
    PMIX_LIST_FOREACH_SAFE(nptr, n2, &targets, prte_node_t) {
        // setup the mapping options
        options->ncpus = prte_rmaps_base_get_ncpus(nptr, NULL, options);
        /* the available cpus are in the scratch location */
        free_target(options);
        options->target = hwloc_bitmap_dup(prte_rmaps_base.available);
        // count the number of target procs on this node
        cnt = 0;
        for (i=0; i < nptr->procs->size; i++) {
            proc = (prte_proc_t*)pmix_pointer_array_get_item(nptr->procs, i);
            if (NULL == proc) {
                continue;
            }
            for (n=0; n < nprocs; n++) {
                if (PMIX_CHECK_PROCID(&procs[n], &proc->name)) {
                    ++cnt;
                    break;
                }
            }
        }
        if (0 == cnt) {
            // should not happen
            continue;
        }
        // assign the number of procs to be placed on this node
        options->nprocs = cnt * procs_per_target; // total number of procs to place on this node;
        // Assign procs for each app_context
        for (i=0; i < jdata->apps->size; i++) {
            app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, i);
            if (NULL == app) {
                continue;
            }
            // is there room on this node? daemons don't count
            if (!daemons) {
                cnt = nptr->slots_inuse + options->nprocs;
                // first check the absolute limit
                if (0 != nptr->slots_max && nptr->slots_max < cnt) {
                    // violates the max limit
                    ret = PRTE_ERR_OUT_OF_RESOURCE;
                    goto done;
                }
                // check oversubscribed
                if (nptr->slots < cnt) {
                    // oversubscribed - we can still fit if they allow oversubscription
                    if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(map->mapping)) {
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                       app->num_procs, app->app, prte_process_info.nodename);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        ret = PRTE_ERR_SILENT;
                        goto done;
                    }
                    // we can use it oversubscribed - so mark it
                    PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                }
            }
            // Map the node to this job
            if (!PRTE_FLAG_TEST(nptr, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_MAPPED);
                PMIX_RETAIN(nptr);
                pmix_pointer_array_add(map->nodes, nptr);
                map->num_nodes += 1;
                options->nnodes++;
            }
            // map the procs
            for (j = 0; j < options->nprocs; ++j) {
                proc = prte_rmaps_base_setup_proc(jdata, i, nptr, NULL, options);
                if (NULL == proc) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    ret = PRTE_ERR_OUT_OF_RESOURCE;
                    goto done;
                }
                jdata->num_procs += 1;
                app->num_procs += 1;
                PMIX_RELEASE(proc);
            }
        }
    }
    ret = prte_rmaps_base_compute_vpids(jdata, options, -1, NULL);

done:
    // ensure all the nodes are marked as not mapped
    for (i=0; i < map->nodes->size; i++) {
        node = (prte_node_t*)pmix_pointer_array_get_item(map->nodes, i);
        if (NULL != node) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }
    PMIX_LIST_DESTRUCT(&targets);
    return ret;
}

static void inherit_env_directives(prte_job_t *jdata,
                                   prte_job_t *parent,
                                   pmix_proc_t *proxy)
{
    prte_app_context_t *app, *app2;
    prte_proc_t *p;
    prte_attribute_t *attr, *attr2;
    pmix_value_t *val, *val2;
    pmix_envar_t *envar, *envar2;
    int n;
    bool exists;

    // deal with job-level attributes first
    PMIX_LIST_FOREACH(attr, &parent->attributes, prte_attribute_t) {
        if (PMIX_ENVAR != attr->data.type) {
            continue;
        }
        val = &attr->data;
        envar = &val->data.envar;

        // do we have a matching attribute in the new job?
        exists = false;
        PMIX_LIST_FOREACH(attr2, &jdata->attributes, prte_attribute_t) {
            if (PMIX_ENVAR != attr->data.type) {
                continue;
            }
            val2 = &attr2->data;
            envar2 = &val2->data.envar;

            if (attr->key == attr2->key) {
                // operation is same - check if the target envars match
                if (0 == strcmp(envar->envar, envar2->envar)) {
                    // these match, so don't overwrite it
                    exists = true;
                    break;
                }
            }
        }

        if (exists) {
            // leave this alone
            continue;
        }

        // if it doesn't exist, then inherit it
        prte_set_attribute(&jdata->attributes, attr->key, PRTE_ATTR_GLOBAL,
                           envar, PMIX_ENVAR);
    }

    /* There is no one-to-one correlation between the apps, but we can
     * inherit the directives from the proc that called spawn, so do that
     * much here */
    p = prte_get_proc_object(proxy);
    if (NULL == p) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    app = (prte_app_context_t*)pmix_pointer_array_get_item(parent->apps, p->app_idx);
    if (NULL == app) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    for (n=0; n < jdata->apps->size; n++) {
        app2 = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, n);
        if (NULL == app2) {
            continue;
        }
        PMIX_LIST_FOREACH(attr, &app->attributes, prte_attribute_t) {
            if (PMIX_ENVAR != attr->data.type) {
                continue;
            }
            val = &attr->data;
            envar = &val->data.envar;

            exists = false;
            PMIX_LIST_FOREACH(attr2, &app2->attributes, prte_attribute_t) {
                if (PMIX_ENVAR != attr->data.type) {
                    continue;
                }
                val2 = &attr2->data;
                envar2 = &val2->data.envar;

                if (attr->key == attr2->key) {
                    // operation is same - check if the target envars match
                    if (0 == strcmp(envar->envar, envar2->envar)) {
                        // these match, so don't overwrite it
                        exists = true;
                        break;
                    }
                }
            }

            if (exists) {
                // leave this alone
                continue;
            }

            // if it doesn't exist, then inherit it
            prte_set_attribute(&app2->attributes, attr->key, PRTE_ATTR_GLOBAL,
                               envar, PMIX_ENVAR);
        }
    }

}
