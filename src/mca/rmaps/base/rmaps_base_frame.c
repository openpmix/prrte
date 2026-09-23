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
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <limits.h>
#include "constants.h"

#include <string.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/prte_cmd_line.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public pmix_mca_base_component_t struct.
 */

#include "src/mca/rmaps/base/static-components.h"

/*
 * Global variables
 */
prte_rmaps_base_t prte_rmaps_base = {
    .selected_modules = PMIX_LIST_STATIC_INIT(prte_rmaps_base.selected_modules),
    .mapping = 0,
    .ranking = 0,
    .ppr = NULL,
    .inherit = false,
    .hwthread_cpus = false,
    .file = NULL,
    .default_pes = 0,
    .available = NULL,
    .baseset = NULL,
    .default_mapping_policy = NULL,
    .default_ranking_policy = NULL,
    .require_hwtcpus = false,
    .have_cores = true,
    .resized_nodes = PMIX_LIST_STATIC_INIT(prte_rmaps_base.resized_nodes)
};

static void rsz_con(prte_rmaps_base_resize_t *p)
{
    p->node = NULL;
    p->slots = 0;
    p->slots_given = false;
}
PMIX_CLASS_INSTANCE(prte_rmaps_base_resize_t, pmix_list_item_t, rsz_con, NULL);

static int prte_rmaps_base_register(pmix_mca_base_register_flag_t flags)
{
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(flags);

    /* define default mapping policy */
    prte_rmaps_base.default_mapping_policy = NULL;
    ret = pmix_mca_base_var_register("prte", NULL, NULL, "mapby",
                                     "Default mapping Policy [slot | hwthread | core | l1cache | "
                                      "l2cache | l3cache | numa | package | node | seq | ppr | "
                                      "device=<class|name> (gpu, network (aka nic, fabric,\n"
                                      " openfabrics), block, or a device name such as mlx5_0) | "
                                      "rankfile | pe-list=a,b (comma-delimited ranges of cpus to use for this job)],"
                                      " with supported colon-delimited modifiers: PE=y (for multiple cpus/proc), "
                                      "SPAN, OVERSUBSCRIBE, NOOVERSUBSCRIBE, NOLOCAL, HWTCPUS, CORECPUS, "
                                      "INHERIT, NOINHERIT, ORDERED, FILE=%s (path to file containing sequential "
                                      "or rankfile entries). For more details, see \"prterun --help map-by\". "
                                      "The full directive need not be provided — "
                                      "only enough characters are required to uniquely identify the "
                                      "directive. Directive values are case insensitive",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_rmaps_base.default_mapping_policy);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", NULL, NULL, "map_by",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "rmaps", "default", "mapping_policy",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* define default ranking policy */
    prte_rmaps_base.default_ranking_policy = NULL;
    ret = pmix_mca_base_var_register("prte", NULL, NULL, "rankby",
                                     "Default ranking Policy [slot | node | span | fill]. "
                                     "For more details, see \"prterun --help rank-by\". The full "
                                     "directive need not be provided — only enough characters are "
                                     "required to uniquely identify the directive. Directive values "
                                     "are case insensitive",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_rmaps_base.default_ranking_policy);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", NULL, NULL, "rank_by",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "rmaps", "default", "ranking_policy",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rmaps_base.inherit = false;
    (void) pmix_mca_base_var_register("prte", "rmaps", "default", "inherit",
                                      "Whether child jobs shall inherit mapping/ranking/binding "
                                      "directives from their parent by default",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_rmaps_base.inherit);

    return PRTE_SUCCESS;
}

static int prte_rmaps_base_close(void)
{
    pmix_list_item_t *item;

    /* cleanup globals */
    while (NULL != (item = pmix_list_remove_first(&prte_rmaps_base.selected_modules))) {
        PMIX_RELEASE(item);
    }
    PMIX_DESTRUCT(&prte_rmaps_base.selected_modules);
    /* a map always drains this, but a teardown in the middle of one must
     * not leak what it left */
    PMIX_LIST_DESTRUCT(&prte_rmaps_base.resized_nodes);
    hwloc_bitmap_free(prte_rmaps_base.available);
    prte_rmaps_base.available = NULL;
    hwloc_bitmap_free(prte_rmaps_base.baseset);
    prte_rmaps_base.baseset = NULL;
    /* the MCA variable system owns default_mapping_policy and
     * default_ranking_policy; these two the framework allocated itself as it
     * parsed them, so they are ours to release */
    if (NULL != prte_rmaps_base.ppr) {
        free(prte_rmaps_base.ppr);
        prte_rmaps_base.ppr = NULL;
    }
    if (NULL != prte_rmaps_base.file) {
        free(prte_rmaps_base.file);
        prte_rmaps_base.file = NULL;
    }

    return pmix_mca_base_framework_components_close(&prte_rmaps_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_rmaps_base_open(pmix_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PMIX_CONSTRUCT(&prte_rmaps_base.selected_modules, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rmaps_base.resized_nodes, pmix_list_t);
    prte_rmaps_base.available = hwloc_bitmap_alloc();
    prte_rmaps_base.baseset = hwloc_bitmap_alloc();

    /* set the default mapping and ranking policies */
    if (NULL != prte_rmaps_base.default_mapping_policy) {
        rc = prte_rmaps_base_set_mapping_policy(NULL, prte_rmaps_base.default_mapping_policy);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    if (NULL != prte_rmaps_base.default_ranking_policy) {
        rc = prte_rmaps_base_set_ranking_policy(NULL, prte_rmaps_base.default_ranking_policy);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&prte_rmaps_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(rmaps, "PRTE Mapping Subsystem", prte_rmaps_base_register,
                                prte_rmaps_base_open, prte_rmaps_base_close,
                                prte_rmaps_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

PMIX_CLASS_INSTANCE(prte_rmaps_base_selected_module_t,
                    pmix_list_item_t,
                    NULL, NULL);

/*
 * Parse the qualifiers of a mapping specification.
 *
 * One parser serves all three levels at which a mapping policy can be
 * given, because the syntax is identical at each; only where the result
 * is recorded differs, and whether a qualifier is allowed at all:
 *
 *   both NULL   the MCA default policy - recorded in prte_rmaps_base
 *   jdata       a job-level policy     - recorded on the job
 *   app         a per-app policy       - recorded on the app context
 *
 * A qualifier whose effect necessarily spans the whole job (OVERSUBSCRIBE,
 * NOOVERSUBSCRIBE, INHERIT, NOINHERIT) describes the job no matter which
 * app's spec it was written in - one app cannot oversubscribe its nodes
 * while its siblings do not. That is a reason to hoist it to the job, not a
 * reason to refuse it, and refusing it left a multi-app command line with no
 * way to ask for oversubscription at all: it takes two mapping directives to
 * make the mapping per-app, and by then there is no job-level directive left
 * to hang the qualifier on. So record it here wherever it was given and let
 * prte_rmaps_base_hoist_job_directives() move the app-held copies onto the
 * job once every app has been parsed. Only the case with no meaning - two
 * apps asking for opposite things - is refused, and it is refused there,
 * where the whole job is in view.
 *
 * Keeping this in one place is deliberate. It used to be written twice -
 * once here and once inline in prte_rmaps_base_set_app_mapping_policy() -
 * and the copies drifted, so "--map-by package:span" and ":ordered" were
 * accepted for a job and silently rejected (a bare PRTE_ERR_BAD_PARAM, no
 * message) for an app.
 */
/*
 * The levels a device list may be interleaved across.
 *
 * "node" is deliberately absent: interleaving across nodes is what SPAN
 * already expresses, and giving one behavior two names that then compose
 * with each other produces nonsense.  SPAN owns the cross-node dimension,
 * interleave the within-node one.
 */
static const pmix_cli_choice_t interleave_levels[] = {
    PMIX_CLI_CHOICE(PRTE_CLI_PACKAGE, HWLOC_OBJ_PACKAGE, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE(PRTE_CLI_NUMA, HWLOC_OBJ_NUMANODE, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE(PRTE_CLI_L3CACHE, HWLOC_OBJ_L3CACHE, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE(PRTE_CLI_L2CACHE, HWLOC_OBJ_L2CACHE, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE(PRTE_CLI_L1CACHE, HWLOC_OBJ_L1CACHE, PMIX_CLI_VALUE_NONE),
    PMIX_CLI_CHOICE_END
};

/* Is this a level a device list may be interleaved across?  "l" fits all
 * three caches and is not one, however the list happens to be ordered. */
bool prte_rmaps_base_interleave_level(const char *name, hwloc_obj_type_t *type)
{
    int tag;

    if (PMIX_CLI_MATCH_FOUND != pmix_cli_match(name, interleave_levels, &tag)) {
        return false;
    }
    if (NULL != type) {
        *type = (hwloc_obj_type_t) tag;
    }
    return true;
}

/*
 * Validate a pe-list value: a comma-delimited list of cpu ids and id ranges,
 * "0-3,6".
 *
 * Non-destructive, deliberately. The job-level parser used to run strtok
 * over the value - which cuts it into pieces - and then had to store a
 * second, untouched pointer to the same text to have anything left to
 * record. Splitting a copy lets both callers validate the string they are
 * about to keep.
 */
static int check_pe_list(const char *spec)
{
    char **entries, **range;
    char *endp;
    int i, n, rc = PRTE_SUCCESS;

    entries = PMIx_Argv_split(spec, ',');
    if (NULL == entries) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "invalid-value", true,
                       "mapping", "PE-LIST", spec);
        return PRTE_ERR_SILENT;
    }
    for (i = 0; NULL != entries[i] && PRTE_SUCCESS == rc; i++) {
        range = PMIx_Argv_split(entries[i], '-');
        if (NULL == range || 2 < PMIx_Argv_count(range)) {
            /* can only have one '-' delimiter - and an entry that is nothing
             * but delimiters comes back NULL rather than empty, because
             * PMIx_Argv_split() does not keep empty tokens. Walking it as an
             * array is a NULL dereference: "--map-by pe-list=-" segfaulted
             * before the job was ever described. */
            rc = PRTE_ERR_SILENT;
        } else {
            for (n = 0; NULL != range[n]; n++) {
                (void) strtol(range[n], &endp, 10);
                if (endp == range[n] || '\0' != *endp) {
                    rc = PRTE_ERR_SILENT;
                    break;
                }
            }
        }
        PMIx_Argv_free(range);
    }
    PMIx_Argv_free(entries);
    if (PRTE_SUCCESS != rc) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "invalid-value", true,
                       "mapping", "PE-LIST", spec);
    }
    return rc;
}

/*
 * Refuse a device value with a comma in it.
 *
 * No device is named with one - not a class, not an OS device, not a
 * uuid - so a comma is a qualifier written after the device with the
 * wrong separator: "device=gpu,ndev=2".  It used to reach the mapper as the
 * class "gpu" with the rest silently dropped, so each process got one GPU
 * where two were asked for.  The matcher no longer truncates it, which
 * leaves a device nobody has; say what was almost certainly meant instead.
 */
static bool device_spec_ok(const pmix_nspace_t nspace, const char *spec,
                           const char *device)
{
    char *fixed, *p;

    if (NULL == device || NULL == strchr(device, ',')) {
        return true;
    }
    fixed = strdup(spec);
    if (NULL != fixed) {
        for (p = fixed; '\0' != *p; p++) {
            if (',' == *p) {
                *p = ':';
            }
        }
    }
    prte_show_help(nspace, "help-prte-rmaps-base.txt", "rmaps:device-with-comma", true,
                   device, (NULL == fixed) ? spec : fixed);
    free(fixed);
    return false;
}

/* The device a ppr pattern's object names, or NULL if it names none -
 * "N:device=<class>" rather than "N:<hwloc object>" */
static const char *ppr_device(const char *object)
{
    int tag;

    if (PMIX_CLI_MATCH_FOUND != pmix_cli_match(object, prte_cli_ppr_objects, &tag) ||
        PRTE_PPROBJ_DEVICE != tag) {
        return NULL;
    }
    return pmix_cli_qualifier_value((char *) object);
}

static int check_modifiers(char *ck, prte_job_t *jdata,
                           prte_app_context_t *app,
                           prte_mapping_policy_t *tmp)
{
    char **ck2, *ptr, *val;
    int i, rc, tag;
    uint16_t u16;
    pmix_list_t *attrs = NULL;
    bool inherit_given = false;
    bool noinherit_given = false;
    bool hwthread_cpus_given = false;
    bool core_cpus_given = false;
    bool oversubscribe_given = false;
    bool nooversubscribe_given = false;
    bool shared = false;
    long ndev, lval;
    char *eptr;
    uint16_t ndev16;

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "%s rmaps:base check modifiers with %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == ck) ? "NULL" : ck);

    if (NULL == ck) {
        return PRTE_SUCCESS;
    }

    /* where anything we record goes */
    if (NULL != app) {
        attrs = &app->attributes;
    } else if (NULL != jdata) {
        attrs = &jdata->attributes;
    }

    /* nothing but delimiters splits to NULL rather than to an empty array */
    ck2 = PMIx_Argv_split(ck, ':');
    for (i = 0; NULL != ck2 && NULL != ck2[i]; i++) {
        /* against the whole vocabulary at once, so an abbreviation that
         * fits two qualifiers - ":i" is inherit and interleave, ":s" span
         * and shared - is refused rather than settled by which arm below
         * happens to come first */
        rc = prte_cli_match(PRTE_JOB_NSPACE(jdata), "--map-by", ck2[i],
                            prte_cli_mapquals, &tag);
        if (PRTE_ERR_NOT_FOUND == rc) {
            /* the caller names the whole request */
            PMIx_Argv_free(ck2);
            return PRTE_ERR_BAD_PARAM;
        } else if (PRTE_SUCCESS != rc) {
            PMIx_Argv_free(ck2);
            return rc;
        }

        if (PRTE_MAPQUAL_SPAN == tag) {
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_SPAN);
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_GIVEN);

        } else if (PRTE_MAPQUAL_OVERSUB == tag) {
            if (nooversubscribe_given) {
                /* conflicting directives */
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "OVERSUBSCRIBE", "NOOVERSUBSCRIBE");
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            PRTE_UNSET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_SUBSCRIBE_GIVEN);
            oversubscribe_given = true;

        } else if (PRTE_MAPQUAL_NOOVER == tag) {
            if (oversubscribe_given) {
                /* conflicting directives */
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "OVERSUBSCRIBE", "NOOVERSUBSCRIBE");
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_SUBSCRIBE_GIVEN);
            nooversubscribe_given = true;

        } else if (PRTE_MAPQUAL_NOLOCAL == tag) {
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_NO_USE_LOCAL);

        } else if (PRTE_MAPQUAL_ORDERED == tag) {
            if (NULL == attrs) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_ORDERED);

        } else if (PRTE_MAPQUAL_PE == tag) {
            /* the vocabulary requires the value, so it is here (PE=2) */
            val = pmix_cli_qualifier_value(ck2[i]);
            /* Must agree with LIMIT and NDEV: the attribute behind this is a
             * uint16, so casting strtol()'s result into one turned "PE=70000"
             * into 4464 and "PE=-1" into 65535. Zero is worse than wrong - it
             * reaches prte_rmaps_base_check_avail() as the DIVISOR in
             * "ncpus / cpus_per_rank", which is an integer division by zero:
             * silently zero on aarch64, SIGFPE in the HNP on x86-64. */
            errno = 0;
            lval = strtol(val, &ptr, 10);
            if (ptr == val || '\0' != *ptr || 0 != errno ||
                0 >= lval || UINT16_MAX < lval) {
                /* value is invalid */
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "invalid-value", true, "mapping policy",
                               "PE", ck2[i]);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            u16 = (uint16_t) lval;
            if (NULL == attrs) {
                prte_rmaps_base.default_pes = u16;
            } else {
                prte_set_attribute(attrs,
                                   (NULL != app) ? PRTE_APP_PES_PER_PROC : PRTE_JOB_PES_PER_PROC,
                                   PRTE_ATTR_GLOBAL, &u16, PMIX_UINT16);
            }

        } else if (PRTE_MAPQUAL_INHERIT == tag) {
            if (noinherit_given) {
                /* conflicting directives */
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "INHERIT", "NOINHERIT");
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == attrs) {
                prte_rmaps_base.inherit = true;
            } else {
                /* GLOBAL even on an app, where this is only held until the
                 * hoist moves it to the job: a spawn request is packed and
                 * sent to the HNP - possibly itself - so the job that gets
                 * mapped is always an unpacked copy, and a LOCAL attribute
                 * would not survive the trip to be hoisted at all */
                prte_set_bool_attribute(attrs, PRTE_JOB_INHERIT, PRTE_ATTR_GLOBAL, true);
            }
            inherit_given = true;

        } else if (PRTE_MAPQUAL_NOINHERIT == tag) {
            if (inherit_given) {
                /* conflicting directives */
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "INHERIT", "NOINHERIT");
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == attrs) {
                prte_rmaps_base.inherit = false;
            } else {
                prte_set_bool_attribute(attrs, PRTE_JOB_NOINHERIT, PRTE_ATTR_GLOBAL, true);
            }
            noinherit_given = true;

        } else if (PRTE_MAPQUAL_HWTCPUS == tag) {
            if (core_cpus_given) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "HWTCPUS", "CORECPUS");
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == attrs) {
                prte_rmaps_base.hwthread_cpus = true;
            } else {
                prte_set_bool_attribute(attrs, (NULL != app) ? PRTE_APP_HWT_CPUS : PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
            }
            hwthread_cpus_given = true;

        } else if (PRTE_MAPQUAL_CORECPUS == tag) {
            if (hwthread_cpus_given) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "HWTCPUS", "CORECPUS");
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            /* honor the user's "corecpus" unless the topology has no cores at
             * all; a core that holds a single hwthread is still a usable core
             * (matches the per-app path, which records corecpus as given) */
            if (!prte_rmaps_base.have_cores) {
                if (NULL == attrs) {
                    prte_rmaps_base.hwthread_cpus = true;
                } else {
                    prte_set_bool_attribute(attrs, (NULL != app) ? PRTE_APP_HWT_CPUS : PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
                }
            } else {
                if (NULL == attrs) {
                    prte_rmaps_base.hwthread_cpus = false;
                } else {
                    prte_set_bool_attribute(attrs, (NULL != app) ? PRTE_APP_CORE_CPUS : PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, true);
                }
            }
            core_cpus_given = true;

        } else if (PRTE_MAPQUAL_FILE == tag) {
            /* the vocabulary requires the value, so it is here */
            val = pmix_cli_qualifier_value(ck2[i]);
            if (NULL == attrs) {
                if (NULL != prte_rmaps_base.file) {
                    // cannot specify it twice
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "multiply-defined", true, "mapping policy",
                                   "FILE", prte_rmaps_base.file, ck2[i]);
                    PMIx_Argv_free(ck2);
                    return PRTE_ERR_SILENT;
                }
                prte_rmaps_base.file = strdup(val);
            } else {
                prte_set_attribute(attrs, (NULL != app) ? PRTE_APP_MAP_FILE : PRTE_JOB_FILE,
                                   PRTE_ATTR_GLOBAL, val, PMIX_STRING);
            }

        } else if (PRTE_MAPQUAL_INTERLEAVE == tag) {
            val = pmix_cli_qualifier_value(ck2[i]);
            if (NULL == val) {
                /* the level defaults to the package: it is the only one that
                 * does anything interesting on a conventional two-socket
                 * node, and the one whose crossing costs the most */
                val = "package";
            }
            if (!prte_rmaps_base_interleave_level(val, NULL)) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:bad-interleave-level",
                               true, val);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == attrs) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-modifier",
                               true, "mapping policy", PRTE_CLI_INTERLEAVE);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(attrs,
                               (NULL != app) ? PRTE_APP_MAP_INTERLEAVE : PRTE_JOB_MAP_INTERLEAVE,
                               PRTE_ATTR_GLOBAL, val, PMIX_STRING);

        } else if (PRTE_MAPQUAL_NDEV == tag) {
            /* how many devices each proc is given - the vocabulary
             * requires the value, so it is here */
            val = pmix_cli_qualifier_value(ck2[i]);
            ndev = strtol(val, &eptr, 10);
            if ('\0' != *eptr || 0 >= ndev || UINT16_MAX < ndev) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "invalid-value", true,
                               "mapping policy", "NDEV", ck2[i]);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == attrs) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-modifier",
                               true, "mapping policy", PRTE_CLI_NDEV);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            ndev16 = (uint16_t) ndev;
            prte_set_attribute(attrs,
                               (NULL != app) ? PRTE_APP_MAP_NDEV : PRTE_JOB_MAP_NDEV,
                               PRTE_ATTR_GLOBAL, &ndev16, PMIX_UINT16);

        } else if (PRTE_MAPQUAL_SHARED == tag) {
            val = pmix_cli_qualifier_value(ck2[i]);
            if (NULL == val) {
                /* the bare qualifier asks for it */
                shared = true;
            } else if (0 == strcasecmp(val, "true") || 0 == strcasecmp(val, "1")
                       || 0 == strcasecmp(val, "yes")) {
                shared = true;
            } else if (0 == strcasecmp(val, "false") || 0 == strcasecmp(val, "0")
                       || 0 == strcasecmp(val, "no")) {
                shared = false;
            } else {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "invalid-value", true,
                               "mapping policy", "SHARED", ck2[i]);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == attrs) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-modifier",
                               true, "mapping policy", PRTE_CLI_SHARED);
                PMIx_Argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            /* a bool attribute means "true" by its presence, and setting it
             * false removes it - which is exactly the default, so nothing
             * needs recording for shared=false */
            if (shared) {
                prte_set_bool_attribute(attrs, (NULL != app) ? PRTE_APP_MAP_SHARED : PRTE_JOB_MAP_SHARED, PRTE_ATTR_GLOBAL, true);
            }
        }
    }
    PMIx_Argv_free(ck2);
    return PRTE_SUCCESS;
}

/*
 * Collect the job-level qualifiers that the apps' mapping specs carry, and
 * move them onto the job.
 *
 * OVERSUBSCRIBE/NOOVERSUBSCRIBE and INHERIT/NOINHERIT answer a question about
 * the job: whether a node may hold more processes than it has slots, and
 * whether this job takes its parent's launch directives. One app of a job
 * cannot answer either differently from its siblings, so wherever the user
 * wrote the qualifier it is the job's. The one thing that cannot be honored
 * is apps that answer it in opposite ways, and that is what is refused here -
 * where every app is in view. Agreement is enough: apps that say nothing are
 * silent, not dissenting, and a job-level directive is simply the first
 * answer the apps have to agree with.
 *
 * An app's copy is removed once it has been hoisted, so an app whose spec
 * held nothing else no longer drags the job onto the per-app dispatch path.
 *
 * The agreed oversubscription answer is returned rather than written to the
 * job, because resolving the job's mapping policy assigns the whole policy
 * word from a default or a parent and would overwrite it; the caller applies
 * it once that has happened. INHERIT lives in the job's attributes, which
 * nothing overwrites, so it is applied here.
 */
int prte_rmaps_base_hoist_job_directives(prte_job_t *jdata,
                                         prte_mapping_policy_t *oversubscribe,
                                         bool *nolocal)
{
    prte_app_context_t *app;
    prte_mapping_policy_t apppol, agreed = 0;
    uint16_t u16;
    uint16_t *u16ptr = &u16;
    bool given = false, over = false, appover, changed;
    int i;

    *oversubscribe = 0;
    *nolocal = false;

    /* a job-level mapping directive, if one was given, is the first answer -
     * read it now, before the mapping policy is resolved against a default */
    if (NULL != jdata->map &&
        (PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        given = true;
        over = !(PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping));
        agreed = PRTE_MAPPING_SUBSCRIBE_GIVEN;
        if (!over) {
            agreed |= PRTE_MAPPING_NO_OVERSUBSCRIBE;
        }
    }

    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }

        /***   OVERSUBSCRIBE / NOOVERSUBSCRIBE   ***/
        if (prte_get_attribute(&app->attributes, PRTE_APP_MAPBY, (void **) &u16ptr, PMIX_UINT16)) {
            apppol = u16;
            changed = false;
            if (PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(apppol)) {
                appover = !(PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(apppol));
                if (given && over != appover) {
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-job-qualifiers",
                                   true, appover ? "OVERSUBSCRIBE" : "NOOVERSUBSCRIBE",
                                   app->app, over ? "OVERSUBSCRIBE" : "NOOVERSUBSCRIBE");
                    return PRTE_ERR_SILENT;
                }
                given = true;
                over = appover;
                agreed = PRTE_MAPPING_SUBSCRIBE_GIVEN;
                if (!over) {
                    agreed |= PRTE_MAPPING_NO_OVERSUBSCRIBE;
                }
                /* the app no longer carries it */
                PRTE_UNSET_MAPPING_DIRECTIVE(apppol, PRTE_MAPPING_SUBSCRIBE_GIVEN);
                PRTE_UNSET_MAPPING_DIRECTIVE(apppol, PRTE_MAPPING_NO_OVERSUBSCRIBE);
                changed = true;
            }

            /***   NOLOCAL   ***/
            /* "do not use the local node" is the job's answer too, and for a
             * blunter reason than the qualifiers above: the directive reaches
             * the mappers only through jdata->map->mapping - get_target_nodes()
             * is handed the JOB's policy word whichever app it is placing - so
             * a nolocal written on an app segment was read by nothing at all
             * and the app ran on the head node anyway. */
            if (PRTE_MAPPING_NO_USE_LOCAL & PRTE_GET_MAPPING_DIRECTIVE(apppol)) {
                *nolocal = true;
                PRTE_UNSET_MAPPING_DIRECTIVE(apppol, PRTE_MAPPING_NO_USE_LOCAL);
                changed = true;
            }

            if (changed) {
                /* What is left may be nothing but the GIVEN marker: an app
                 * whose spec held only job-wide qualifiers named no mapping
                 * policy of its own, and leaving the attribute behind drags
                 * the whole job onto the per-app dispatch path to be placed
                 * by a policy of zero. Anything the app really did ask for -
                 * a policy, SPAN, ORDERED - keeps it. */
                if (0 == PRTE_GET_MAPPING_POLICY(apppol) &&
                    0 == (PRTE_GET_MAPPING_DIRECTIVE(apppol) & ~PRTE_MAPPING_GIVEN)) {
                    prte_remove_attribute(&app->attributes, PRTE_APP_MAPBY);
                } else {
                    prte_set_attribute(&app->attributes, PRTE_APP_MAPBY, PRTE_ATTR_GLOBAL,
                                       &apppol, PMIX_UINT16);
                }
            }
        }

        /***   INHERIT / NOINHERIT   ***/
        if (PRTE_ATTR_IS_TRUE(&app->attributes, PRTE_JOB_INHERIT)) {
            if (PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_NOINHERIT)) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-job-qualifiers",
                               true, "INHERIT", app->app, "NOINHERIT");
                return PRTE_ERR_SILENT;
            }
            prte_remove_attribute(&app->attributes, PRTE_JOB_INHERIT);
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_INHERIT, PRTE_ATTR_GLOBAL, true);
        }
        if (PRTE_ATTR_IS_TRUE(&app->attributes, PRTE_JOB_NOINHERIT)) {
            if (PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_INHERIT)) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "conflicting-job-qualifiers",
                               true, "NOINHERIT", app->app, "INHERIT");
                return PRTE_ERR_SILENT;
            }
            prte_remove_attribute(&app->attributes, PRTE_JOB_NOINHERIT);
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_NOINHERIT, PRTE_ATTR_GLOBAL, true);
        }
    }

    *oversubscribe = agreed;
    return PRTE_SUCCESS;
}

int prte_rmaps_base_set_default_mapping(prte_job_t *jdata,
                                        prte_rmaps_options_t *options)
{
    prte_binding_policy_t bind;

    if (1 < options->cpus_per_rank) {
        /* assigning multiple cpus to a rank requires that we map to
         * objects that have multiple cpus in them, so default
         * to byslot if nothing else was specified by the user.
         */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps mapping not given with multiple cpus/rank - using byslot");
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);

    } else if (PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
        /* they specified a binding policy, but not a mapping policy, so
         * map by the binding object */
        bind = PRTE_GET_BINDING_POLICY(jdata->map->binding);
        switch(bind) {
            case PRTE_BIND_TO_NONE:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                break;
            case PRTE_BIND_TO_PACKAGE:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYPACKAGE);
                break;
            case PRTE_BIND_TO_NUMA:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYNUMA);
                break;
            case PRTE_BIND_TO_L3CACHE:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYL3CACHE);
                break;
            case PRTE_BIND_TO_L2CACHE:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYL2CACHE);
                break;
            case PRTE_BIND_TO_L1CACHE:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYL1CACHE);
                break;
            case PRTE_BIND_TO_CORE:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYCORE);
                break;
            case PRTE_BIND_TO_HWTHREAD:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYHWTHREAD);
                break;
            default:
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                break;
        }
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps mapping not given but binding set - using %s",
                            prte_rmaps_base_print_mapping(jdata->map->mapping));

    } else {
        if (options->nprocs <= 2) {
            // map by whatever CPU we are using
            if (options->use_hwthreads) {
                /* if we are using hwthread cpus, then map by those */
                pmix_output_verbose(options->verbosity, options->stream,
                                    "mca:rmaps mapping not given - using byhwthread");
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYHWTHREAD);
            } else {
                /* otherwise map by core */
                pmix_output_verbose(options->verbosity, options->stream,
                                    "mca:rmaps mapping not given - using bycore");
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYCORE);
            }
        } else {
            /* if package is available, map by that */
            if (NULL != prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_PACKAGE, 0)) {
                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps mapping not set by user - using bypackage");
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYPACKAGE);
            } else if (NULL != prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_NUMANODE, 0)) {
                /* if NUMA is available, map by that */
                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps mapping not set by user - using bynuma");
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYNUMA);
            } else {
                /* if we have neither, then just do by slot */
                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps mapping not given and no packages/NUMAs - using byslot");
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_rmaps_base_set_mapping_policy(prte_job_t *jdata, char *inspec)
{
    char **ck;
    char *cptr, *val;
    prte_mapping_policy_t tmp;
    int rc, tag;

    /* set defaults */
    tmp = 0;

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "%s rmaps:base set policy with %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == inspec) ? "NULL" : inspec);

    if (NULL == inspec) {
        return PRTE_SUCCESS;
    }

    /* a spec that opens with ':' names no mapping policy - it is qualifiers
     * only, which still have to be parsed */
    if (':' == inspec[0]) {
        rc = check_modifiers(&inspec[1], jdata, NULL, &tmp);
        if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            if (PRTE_ERR_BAD_PARAM == rc) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-modifier", true, inspec);
                rc = PRTE_ERR_SILENT;
            }
            return rc;
        }
        goto setpolicy;
    }

    ck = PMIx_Argv_split(inspec, ':');
    if (NULL == ck) {
        /* an empty spec names no policy at all */
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-policy",
                       true, "mapping", inspec);
        return PRTE_ERR_SILENT;
    }

    /* The policy word, matched against the whole vocabulary: an abbreviation
     * that fits two policies ("n" is node and numa) is refused, as is a
     * value given to a policy that takes none - "core=2" used to be read as
     * "core" with the 2 thrown away. */
    rc = prte_cli_match(PRTE_JOB_NSPACE(jdata), "--map-by", ck[0], prte_cli_mappers, &tag);
    if (PRTE_ERR_NOT_FOUND == rc) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-policy",
                       true, "mapping", ck[0]);
        PMIx_Argv_free(ck);
        return PRTE_ERR_SILENT;
    } else if (PRTE_SUCCESS != rc) {
        PMIx_Argv_free(ck);
        return rc;
    }

    if (PRTE_MAPPER_PPR == tag) {
        /* "ppr:N:object", optionally followed by qualifiers - the pattern
         * is two fields, not a qualifier, so it is taken off first */
        if (3 > PMIx_Argv_count(ck)) {
            prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "invalid-pattern", true, inspec);
            PMIx_Argv_free(ck);
            return PRTE_ERR_SILENT;
        }
        if (!device_spec_ok(PRTE_JOB_NSPACE(jdata), inspec, ppr_device(ck[2]))) {
            PMIx_Argv_free(ck);
            return PRTE_ERR_SILENT;
        }
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "%s rmaps:base policy %s pattern %s:%s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ck[0], ck[1], ck[2]);
        pmix_asprintf(&cptr, "%s:%s", ck[1], ck[2]);
        if (NULL == jdata) {
            prte_rmaps_base.ppr = cptr;
        } else {
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, cptr, PMIX_STRING);
            free(cptr);
        }
        PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_PPR);
        PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);
        if (NULL != ck[3]) {
            PMIX_ARGV_JOIN(cptr, &ck[3], ':');
            rc = check_modifiers(cptr, jdata, NULL, &tmp);
            if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                if (PRTE_ERR_BAD_PARAM == rc) {
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-modifier", true, inspec);
                    rc = PRTE_ERR_SILENT;
                }
                PMIx_Argv_free(ck);
                free(cptr);
                return rc;
            }
            free(cptr);
        }
        PMIx_Argv_free(ck);
        goto setpolicy;
    }

    /* The qualifiers come before the policy is acted on, because acting on
     * it can depend on them - rankfile needs the FILE= qualifier. */
    if (NULL != ck[1]) {
        PMIX_ARGV_JOIN(cptr, &ck[1], ':');
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "%s rmaps:base policy %s modifiers %s provided",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ck[0], cptr);
        rc = check_modifiers(cptr, jdata, NULL, &tmp);
        if (PRTE_SUCCESS != rc && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            if (PRTE_ERR_BAD_PARAM == rc) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-modifier", true, inspec);
                rc = PRTE_ERR_SILENT;
            }
            PMIx_Argv_free(ck);
            free(cptr);
            return rc;
        }
        free(cptr);
    }

    /* the value of a policy that takes one - pe-list=, device= - which the
     * vocabulary has already required to be there */
    val = pmix_cli_qualifier_value(ck[0]);

    switch (tag) {
        case PRTE_MAPPER_SLOT:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYSLOT);
            break;

        case PRTE_MAPPER_NODE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYNODE);
            break;

        case PRTE_MAPPER_SEQ:
            /* there are several mechanisms by which the file specifying
             * the sequence can be passed, so not really feasible to check
             * it here */
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_SEQ);
            break;

        case PRTE_MAPPER_CORE:
            /* honor the user's "core" unless the topology has no cores at all;
             * a core that holds a single hwthread is still a core to map by */
            if (!prte_rmaps_base.have_cores) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYHWTHREAD);
            } else {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYCORE);
            }
            break;

        case PRTE_MAPPER_L1CACHE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL1CACHE);
            break;

        case PRTE_MAPPER_L2CACHE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL2CACHE);
            break;

        case PRTE_MAPPER_L3CACHE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL3CACHE);
            break;

        case PRTE_MAPPER_NUMA:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYNUMA);
            break;

        case PRTE_MAPPER_PACKAGE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYPACKAGE);
            break;

        case PRTE_MAPPER_RANKFILE:
            /* The file naming the rank->host+cpuset assignments: from the
             * FILE= qualifier parsed above or, failing that, the one the
             * DVM was given as its default - which is what the per-app
             * parser has always done.  This parser meant to as well, but
             * refused a job without FILE= before it got that far, so a DVM
             * started with a default rankfile could not use it for any
             * job that asked for rankfile mapping by name. */
            if (NULL != jdata && NULL != prte_rmaps_base.file &&
                !prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, NULL, PMIX_STRING)) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FILE, PRTE_ATTR_GLOBAL,
                                   prte_rmaps_base.file, PMIX_STRING);
            }
            if ((NULL == jdata && NULL == prte_rmaps_base.file) ||
                (NULL != jdata && !prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, NULL, PMIX_STRING))) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rankfile-no-filename", true);
                PMIx_Argv_free(ck);
                return PRTE_ERR_BAD_PARAM;
            }
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYUSER);
            break;

        case PRTE_MAPPER_HWT:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYHWTHREAD);
            /* if we are mapping processes to individual hwthreads, then
             * we need to treat those hwthreads as separate cpus
             */
            if (NULL == jdata) {
                prte_rmaps_base.hwthread_cpus = true;
            } else {
                prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
            }
            break;

        case PRTE_MAPPER_PELIST:
            if (NULL == jdata) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-policy", true,
                               "mapping", ck[0]);
                PMIx_Argv_free(ck);
                return PRTE_ERR_SILENT;
            }
            rc = check_pe_list(val);
            if (PRTE_SUCCESS != rc) {
                PMIx_Argv_free(ck);
                return rc;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL,
                               val, PMIX_STRING);
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_PELIST);
            break;

        case PRTE_MAPPER_DEVICE:
            /* place procs against the devices in each node's topology. The
             * value is either a device class ("gpu", "network", ...) or the
             * name or uuid of one particular device, in which case every proc
             * is placed near that one - which is what the removed "dist"
             * policy did, said with one directive instead of a policy plus an
             * MCA parameter. There is deliberately no bare "--map-by gpu":
             * the class is the directive's value, which is what lets a new
             * class be supported by adding a value rather than a directive. */
            if (NULL == jdata) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-policy", true,
                               "mapping", ck[0]);
                PMIx_Argv_free(ck);
                return PRTE_ERR_SILENT;
            }
            if (!device_spec_ok(PRTE_JOB_NSPACE(jdata), inspec, val)) {
                PMIx_Argv_free(ck);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_MAP_DEVICE, PRTE_ATTR_GLOBAL,
                               val, PMIX_STRING);
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYDEVICE);
            break;

        default:
            /* ppr was handled above; nothing else is in the vocabulary */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            PMIx_Argv_free(ck);
            return PRTE_ERR_BAD_PARAM;
    }
    PMIx_Argv_free(ck);
    PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);

setpolicy:
    /* interleave reorders a device list, so it means nothing anywhere else.
     * Refuse it by name rather than as an unknown qualifier - the spelling
     * is legal, just not here. The check waits until now because the
     * qualifiers are parsed before the directive they belong to. */
    if (NULL != jdata
        && prte_get_attribute(&jdata->attributes, PRTE_JOB_MAP_INTERLEAVE, NULL, PMIX_STRING)
        && PRTE_MAPPING_BYDEVICE != PRTE_GET_MAPPING_POLICY(tmp)) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:interleave-needs-device", true,
                       prte_rmaps_base_print_mapping(tmp));
        return PRTE_ERR_SILENT;
    }
    /* "shared" says devices may be shared, so it means nothing where there
     * are no devices - refuse it by name rather than as an unknown
     * qualifier, since the spelling is legal, just not here */
    if (NULL != jdata
        && PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_MAP_SHARED)
        && PRTE_MAPPING_BYDEVICE != PRTE_GET_MAPPING_POLICY(tmp)) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:shared-needs-device", true,
                       prte_rmaps_base_print_mapping(tmp));
        return PRTE_ERR_SILENT;
    }
    if (NULL != jdata
        && prte_get_attribute(&jdata->attributes, PRTE_JOB_MAP_NDEV, NULL, PMIX_UINT16)
        && PRTE_MAPPING_BYDEVICE != PRTE_GET_MAPPING_POLICY(tmp)) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "rmaps:ndev-needs-device", true,
                       prte_rmaps_base_print_mapping(tmp));
        return PRTE_ERR_SILENT;
    }

    if (NULL == jdata) {
        prte_rmaps_base.mapping = tmp;
    } else {
        if (NULL == jdata->map) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return PRTE_ERR_BAD_PARAM;
        }
        jdata->map->mapping = tmp;
    }

    return PRTE_SUCCESS;
}

int prte_rmaps_base_set_default_ranking(prte_job_t *jdata,
                                        prte_rmaps_options_t *options)
{
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(options);

    rc = prte_rmaps_base_set_ranking_policy(jdata, NULL);
    return rc;
}

/* the ranking policy a --rank-by word names */
static prte_ranking_policy_t rank_policy(int tag)
{
    switch (tag) {
        case PRTE_RANKER_NODE:
            return PRTE_RANK_BY_NODE;
        case PRTE_RANKER_FILL:
            return PRTE_RANK_BY_FILL;
        case PRTE_RANKER_SPAN:
            return PRTE_RANK_BY_SPAN;
        default:
            return PRTE_RANK_BY_SLOT;
    }
}

int prte_rmaps_base_set_ranking_policy(prte_job_t *jdata, char *spec)
{
    prte_ranking_policy_t tmp;
    prte_mapping_policy_t mapping;
    int rc, tag;

    /* set default */
    tmp = 0;

    if (NULL == spec) {
        if (NULL == jdata) {
            return PRTE_SUCCESS;
        }
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
        }
        if (PRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
            return PRTE_SUCCESS;
        }
        mapping = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
        /* if mapping by-node, then default to rank-by node */
        if (PRTE_MAPPING_BYNODE == mapping) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_NODE);

        } else if (PRTE_MAPPING_BYSLOT == mapping) {
            /* default to by-slot */
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SLOT);

        } else if (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
            /* default to by-span */
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SPAN);

        } else if (PRTE_MAPPING_BYHWTHREAD >= mapping &&
                   PRTE_MAPPING_BYNUMA <= mapping) {
            /* default to by-slot */
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_FILL);

        } else {
            /* default to slot */
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SLOT);
        }
        jdata->map->ranking = tmp;
        return PRTE_SUCCESS;
    }

    rc = prte_cli_match(PRTE_JOB_NSPACE(jdata), "--rank-by", spec, prte_cli_rankers, &tag);
    if (PRTE_ERR_NOT_FOUND == rc) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-policy", true,
                       "ranking", spec);
        return PRTE_ERR_SILENT;
    } else if (PRTE_SUCCESS != rc) {
        return rc;
    }
    PRTE_SET_RANKING_POLICY(tmp, rank_policy(tag));
    PRTE_SET_RANKING_DIRECTIVE(tmp, PRTE_RANKING_GIVEN);

    if (NULL == jdata) {
        prte_rmaps_base.ranking = tmp;
    } else {
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
        }
        jdata->map->ranking = tmp;
    }

    return PRTE_SUCCESS;
}

/* Per-app-context mapping policy parser.
 * Stores results in app->attributes rather than jdata->map. A qualifier that
 * spans the whole job (OVERSUBSCRIBE, NOOVERSUBSCRIBE, INHERIT, NOINHERIT) is
 * held on the app until prte_rmaps_base_hoist_job_directives() moves it onto
 * the job, which is also where apps are held to agreeing about it.
 */
int prte_rmaps_base_set_app_mapping_policy(prte_app_context_t *app, char *inspec)
{
    char **ck;
    char *cptr, *val;
    prte_mapping_policy_t tmp;
    int rc, tag = -1;
    bool nolocal = false;

    tmp = 0;

    if (NULL == inspec) {
        return PRTE_SUCCESS;
    }

    /* a spec that opens with ':' names no policy - it is qualifiers only,
     * which still have to be parsed. This mirrors the job-level parser; a
     * per-app "--map-by :OVERSUBSCRIBE" used to be discarded in silence. */
    if (':' == inspec[0]) {
        rc = check_modifiers(&inspec[1], NULL, app, &tmp);
        if (PRTE_SUCCESS != rc) {
            if (PRTE_ERR_BAD_PARAM == rc) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-modifier",
                               true, inspec);
                rc = PRTE_ERR_SILENT;
            }
            return rc;
        }
        goto setpolicy;
    }

    ck = PMIx_Argv_split(inspec, ':');
    if (NULL == ck) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-policy",
                       true, "mapping", inspec);
        return PRTE_ERR_SILENT;
    }

    rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--map-by", ck[0], prte_cli_mappers, &tag);
    if (PRTE_ERR_NOT_FOUND == rc && pmix_check_cli_option(ck[0], PRTE_CLI_NOLOCAL)) {
        /* Per app, and only per app, "nolocal" is accepted as though it
         * were a policy.  It is not one: it sets the directive bit and
         * leaves the policy to be derived.  Asked only after the policies
         * have had their say, so it cannot make one of them ambiguous. */
        nolocal = true;
        rc = PRTE_SUCCESS;
    }
    if (PRTE_ERR_NOT_FOUND == rc) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-policy",
                       true, "mapping", ck[0]);
        PMIx_Argv_free(ck);
        return PRTE_ERR_SILENT;
    } else if (PRTE_SUCCESS != rc) {
        PMIx_Argv_free(ck);
        return rc;
    }

    if (!nolocal && PRTE_MAPPER_PPR == tag) {
        if (3 > PMIx_Argv_count(ck)) {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "invalid-pattern", true, inspec);
            PMIx_Argv_free(ck);
            return PRTE_ERR_SILENT;
        }
        if (!device_spec_ok(PRTE_PROC_MY_NAME->nspace, inspec, ppr_device(ck[2]))) {
            PMIx_Argv_free(ck);
            return PRTE_ERR_SILENT;
        }
        /* ck[1] = N, ck[2] = object type. Save the whole pattern, in the
         * same spelling the job-level parser uses: the object is as much
         * a part of what this app asked for as the count, and recording
         * only the count left the app placed N-per-whatever-object the
         * job happened to resolve */
        pmix_asprintf(&cptr, "%s:%s", ck[1], ck[2]);
        prte_set_attribute(&app->attributes, PRTE_APP_PPR, PRTE_ATTR_GLOBAL, cptr, PMIX_STRING);
        free(cptr);
        PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_PPR);
        PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);
        if (NULL != ck[3]) {
            PMIX_ARGV_JOIN(cptr, &ck[3], ':');
            rc = check_modifiers(cptr, NULL, app, &tmp);
            if (PRTE_SUCCESS != rc) {
                if (PRTE_ERR_BAD_PARAM == rc) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-modifier",
                                   true, cptr);
                    rc = PRTE_ERR_SILENT;
                }
                PMIx_Argv_free(ck);
                free(cptr);
                return rc;
            }
            free(cptr);
        }
        PMIx_Argv_free(ck);
        goto setpolicy;
    }

    /* Parse the qualifiers with the same code every other level uses. A
     * qualifier that spans the whole job is recorded here and hoisted onto
     * the job by prte_rmaps_base_hoist_job_directives() once every app has
     * been seen. They come before the policy is acted on because acting on
     * it can depend on them - rankfile reads the FILE= qualifier. */
    if (NULL != ck[1]) {
        PMIX_ARGV_JOIN(cptr, &ck[1], ':');
        rc = check_modifiers(cptr, NULL, app, &tmp);
        if (PRTE_SUCCESS != rc) {
            if (PRTE_ERR_BAD_PARAM == rc) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-modifier",
                               true, cptr);
                rc = PRTE_ERR_SILENT;
            }
            PMIx_Argv_free(ck);
            free(cptr);
            return rc;
        }
        free(cptr);
    }

    if (nolocal) {
        PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_NO_USE_LOCAL);
        PMIx_Argv_free(ck);
        PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);
        goto setpolicy;
    }

    /* the value of a policy that takes one, which the vocabulary has
     * already required to be there */
    val = pmix_cli_qualifier_value(ck[0]);

    switch (tag) {
        case PRTE_MAPPER_SLOT:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYSLOT);
            break;
        case PRTE_MAPPER_NODE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYNODE);
            break;
        case PRTE_MAPPER_SEQ:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_SEQ);
            break;
        case PRTE_MAPPER_CORE:
            /* honor the user's "core" unless the topology has no cores at all;
             * a core that holds a single hwthread is still a core to map by */
            if (!prte_rmaps_base.have_cores) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYHWTHREAD);
            } else {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYCORE);
            }
            break;
        case PRTE_MAPPER_L1CACHE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL1CACHE);
            break;
        case PRTE_MAPPER_L2CACHE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL2CACHE);
            break;
        case PRTE_MAPPER_L3CACHE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL3CACHE);
            break;
        case PRTE_MAPPER_NUMA:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYNUMA);
            break;
        case PRTE_MAPPER_PACKAGE:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYPACKAGE);
            break;
        case PRTE_MAPPER_HWT:
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYHWTHREAD);
            prte_set_bool_attribute(&app->attributes, PRTE_APP_HWT_CPUS, PRTE_ATTR_GLOBAL, true);
            break;
        case PRTE_MAPPER_PELIST:
            /* the cpus this app is to run on. Recorded per app, exactly as
             * the job-level parser records the job's: which cpus one app of
             * an MPMD job may use is as much its own business as which
             * object it maps by, and a multi-app command line has nowhere
             * else to say it */
            rc = check_pe_list(val);
            if (PRTE_SUCCESS != rc) {
                PMIx_Argv_free(ck);
                return rc;
            }
            prte_set_attribute(&app->attributes, PRTE_APP_CPUSET, PRTE_ATTR_GLOBAL,
                               val, PMIX_STRING);
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_PELIST);
            break;
        case PRTE_MAPPER_DEVICE:
            /* the devices this app is to be placed against - as much its own
             * business as the object it maps by. Must stay in step with the
             * job-level arm: a directive accepted at one level and refused
             * at the other is the recurring bug in this file */
            if (!device_spec_ok(PRTE_PROC_MY_NAME->nspace, inspec, val)) {
                PMIx_Argv_free(ck);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&app->attributes, PRTE_APP_MAP_DEVICE, PRTE_ATTR_GLOBAL,
                               val, PMIX_STRING);
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYDEVICE);
            break;
        case PRTE_MAPPER_RANKFILE:
            /* the file naming this app's rank->host+cpuset assignments. The
             * FILE= qualifier was parsed above onto PRTE_APP_MAP_FILE;
             * without one, fall back to the MCA default the job-level parser
             * also uses. There is no job to read here - the job-level
             * PRTE_JOB_FILE is checked by the rank_file mapper itself when
             * the app named none */
            if (!prte_get_attribute(&app->attributes, PRTE_APP_MAP_FILE, NULL, PMIX_STRING)) {
                if (NULL == prte_rmaps_base.file) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "rankfile-no-filename", true);
                    PMIx_Argv_free(ck);
                    return PRTE_ERR_SILENT;
                }
                prte_set_attribute(&app->attributes, PRTE_APP_MAP_FILE, PRTE_ATTR_GLOBAL,
                                   prte_rmaps_base.file, PMIX_STRING);
            }
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYUSER);
            break;
        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            PMIx_Argv_free(ck);
            return PRTE_ERR_BAD_PARAM;
    }
    PMIx_Argv_free(ck);
    PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);

setpolicy:
    /* interleave reorders a device list, so it means nothing anywhere else -
     * see the job-level parser, which refuses it the same way */
    if (prte_get_attribute(&app->attributes, PRTE_APP_MAP_INTERLEAVE, NULL, PMIX_STRING)
        && PRTE_MAPPING_BYDEVICE != PRTE_GET_MAPPING_POLICY(tmp)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "rmaps:interleave-needs-device", true,
                       prte_rmaps_base_print_mapping(tmp));
        return PRTE_ERR_SILENT;
    }
    if (PRTE_ATTR_IS_TRUE(&app->attributes, PRTE_APP_MAP_SHARED)
        && PRTE_MAPPING_BYDEVICE != PRTE_GET_MAPPING_POLICY(tmp)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "rmaps:shared-needs-device", true,
                       prte_rmaps_base_print_mapping(tmp));
        return PRTE_ERR_SILENT;
    }
    if (prte_get_attribute(&app->attributes, PRTE_APP_MAP_NDEV, NULL, PMIX_UINT16)
        && PRTE_MAPPING_BYDEVICE != PRTE_GET_MAPPING_POLICY(tmp)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "rmaps:ndev-needs-device", true,
                       prte_rmaps_base_print_mapping(tmp));
        return PRTE_ERR_SILENT;
    }
    prte_set_attribute(&app->attributes, PRTE_APP_MAPBY, PRTE_ATTR_GLOBAL, &tmp, PMIX_UINT16);
    return PRTE_SUCCESS;
}

int prte_rmaps_base_set_app_ranking_policy(prte_app_context_t *app, char *spec)
{
    prte_ranking_policy_t tmp = 0;
    int rc, tag;

    if (NULL == spec) {
        return PRTE_SUCCESS;
    }

    rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--rank-by", spec, prte_cli_rankers, &tag);
    if (PRTE_ERR_NOT_FOUND == rc) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "unrecognized-policy", true, "ranking", spec);
        return PRTE_ERR_SILENT;
    } else if (PRTE_SUCCESS != rc) {
        return rc;
    }
    PRTE_SET_RANKING_POLICY(tmp, rank_policy(tag));
    PRTE_SET_RANKING_DIRECTIVE(tmp, PRTE_RANKING_GIVEN);
    prte_set_attribute(&app->attributes, PRTE_APP_RANKBY, PRTE_ATTR_GLOBAL, &tmp, PMIX_UINT16);
    return PRTE_SUCCESS;
}

int prte_rmaps_base_set_app_binding_policy(prte_app_context_t *app, char *spec)
{
    int i, rc, tag;
    prte_binding_policy_t tmp = 0;
    char **quals, *myspec, *ptr, *p2, *endp;
    uint16_t u16;
    long lval;

    if (NULL == spec) {
        return PRTE_SUCCESS;
    }

    myspec = strdup(spec);

    ptr = strchr(myspec, ':');
    if (NULL != ptr) {
        *ptr = '\0';
        ++ptr;
    }

    /* Resolve the policy word first, as the job-level parser does, so a
     * spec whose policy does not parse records nothing on the app.
     *
     * An empty policy word - ":overload-allowed" - names no policy: the app
     * gets the binding it would have had anyway, with these qualifiers.
     * That is what the job-level parser does. The policy bits stay clear
     * and prte_rmaps_base_resolve_app_options() fills them in. */
    if ('\0' != myspec[0]) {
        rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--bind-to", myspec,
                            prte_cli_binders, &tag);
        if (PRTE_ERR_NOT_FOUND == rc) {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-hwloc-base.txt", "invalid binding_policy", true,
                           "binding", spec);
            free(myspec);
            return PRTE_ERR_BAD_PARAM;
        } else if (PRTE_SUCCESS != rc) {
            free(myspec);
            return rc;
        }
        switch (tag) {
            case PRTE_BINDER_NONE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_NONE);
                break;
            case PRTE_BINDER_HWT:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_HWTHREAD);
                break;
            case PRTE_BINDER_CORE:
                /* honor the user's "core" unless the topology has no cores at all;
                 * a core that holds a single hwthread is still a core to bind to */
                if (!prte_rmaps_base.have_cores) {
                    PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_HWTHREAD);
                } else {
                    PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_CORE);
                }
                break;
            case PRTE_BINDER_L1CACHE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L1CACHE);
                break;
            case PRTE_BINDER_L2CACHE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L2CACHE);
                break;
            case PRTE_BINDER_L3CACHE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L3CACHE);
                break;
            case PRTE_BINDER_NUMA:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_NUMA);
                break;
            case PRTE_BINDER_PACKAGE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_PACKAGE);
                break;
        }
    }

    if (NULL != ptr) {
        /* an empty qualifier list - "core:" - splits to NULL, not to an
         * empty array; see the job-level parser */
        quals = PMIx_Argv_split(ptr, ':');
        for (i = 0; NULL != quals && NULL != quals[i]; i++) {
            rc = prte_cli_match(PRTE_PROC_MY_NAME->nspace, "--bind-to", quals[i],
                                prte_cli_bindquals, &tag);
            if (PRTE_ERR_NOT_FOUND == rc) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-hwloc-base.txt", "unrecognized-modifier", true, spec);
                PMIx_Argv_free(quals);
                free(myspec);
                return PRTE_ERR_BAD_PARAM;
            } else if (PRTE_SUCCESS != rc) {
                PMIx_Argv_free(quals);
                free(myspec);
                return rc;
            }

            if (PRTE_BINDQUAL_IF_SUPP == tag) {
                tmp |= PRTE_BIND_IF_SUPPORTED;
            } else if (PRTE_BINDQUAL_OVERLOAD == tag) {
                tmp |= (PRTE_BIND_ALLOW_OVERLOAD | PRTE_BIND_OVERLOAD_GIVEN);
            } else if (PRTE_BINDQUAL_NOOVERLOAD == tag) {
                tmp = (tmp & ~PRTE_BIND_ALLOW_OVERLOAD);
                tmp |= PRTE_BIND_OVERLOAD_GIVEN;
            } else if (PRTE_BINDQUAL_REPORT == tag) {
                /* The job-level parser in src/hwloc accepts this and records
                 * PRTE_JOB_REPORT_BINDINGS. There is no per-app counterpart
                 * to that attribute - reporting is a property of the whole
                 * job - so say precisely that rather than calling a spelling
                 * that is legal one app segment earlier unrecognized. */
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "job-only-modifier", true,
                               "binding", quals[i]);
                PMIx_Argv_free(quals);
                free(myspec);
                return PRTE_ERR_SILENT;
            } else if (PRTE_BINDQUAL_LIMIT == tag) {
                p2 = pmix_cli_qualifier_value(quals[i]);
                /* Must agree with the job-level parser in
                 * prte_hwloc_base_set_binding_policy(): the attribute is a
                 * uint16, so casting strtol()'s result into one turned
                 * "limit=70000" into a limit of 4464, and a limit of zero is
                 * read downstream as "no limit at all" rather than as what
                 * the user wrote. */
                errno = 0;
                lval = strtol(p2, &endp, 10);
                if (endp == p2 || '\0' != *endp || 0 != errno ||
                    0 >= lval || UINT16_MAX < lval) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-rmaps-base.txt", "invalid-value", true,
                                   "binding limit", "LIMIT", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                u16 = (uint16_t) lval;
                prte_set_attribute(&app->attributes, PRTE_APP_BINDING_LIMIT,
                                   PRTE_ATTR_GLOBAL, &u16, PMIX_UINT16);
            }
        }
        PMIx_Argv_free(quals);
    }
    free(myspec);

    prte_set_attribute(&app->attributes, PRTE_APP_BINDTO, PRTE_ATTR_GLOBAL, &tmp, PMIX_UINT16);
    return PRTE_SUCCESS;
}
