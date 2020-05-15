/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/mca/base/base.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/rmaps/base/static-components.h"

/*
 * Global variables
 */
prrte_rmaps_base_t prrte_rmaps_base = {{{0}}};

/*
 * Local variables
 */
static char *rmaps_base_mapping_policy = NULL;
static char *rmaps_base_ranking_policy = NULL;
static bool rmaps_base_inherit = false;

static int prrte_rmaps_base_register(prrte_mca_base_register_flag_t flags)
{
    /* define default mapping policy */
    rmaps_base_mapping_policy = NULL;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "default", "mapping_policy",
                                       "Default mapping Policy [slot | hwthread | core (default:np<=2) | l1cache | "
                                       "l2cache | l3cache | package (default:np>2) | node | seq | dist | ppr],"
                                       " with supported colon-delimited modifiers: PE=y (for multiple cpus/proc), "
                                       "SPAN, OVERSUBSCRIBE, NOOVERSUBSCRIBE, NOLOCAL, HWTCPUS, CORECPUS, "
                                       "DEVICE(for dist policy), INHERIT, NOINHERIT",
                                       PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &rmaps_base_mapping_policy);

    /* define default ranking policy */
    rmaps_base_ranking_policy = NULL;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "default", "ranking_policy",
                                       "Default ranking Policy [slot (default:np<=2) | hwthread | core | l1cache "
                                       "| l2cache | l3cache | package (default:np>2) | node], with modifier :SPAN or :FILL",
                                       PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &rmaps_base_ranking_policy);

    rmaps_base_inherit = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "default", "inherit",
                                       "Whether child jobs shall inherit mapping/ranking/binding directives from their parent by default",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_inherit);

    return PRRTE_SUCCESS;
}

static int prrte_rmaps_base_close(void)
{
    prrte_list_item_t *item;

    /* cleanup globals */
    while (NULL != (item = prrte_list_remove_first(&prrte_rmaps_base.selected_modules))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_rmaps_base.selected_modules);

    return prrte_mca_base_framework_components_close(&prrte_rmaps_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_rmaps_base_open(prrte_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PRRTE_CONSTRUCT(&prrte_rmaps_base.selected_modules, prrte_list_t);
    prrte_rmaps_base.mapping = 0;
    prrte_rmaps_base.ranking = 0;
    prrte_rmaps_base.inherit = rmaps_base_inherit;
    prrte_rmaps_base.hwthread_cpus = false;
    if (NULL == prrte_set_slots) {
        prrte_set_slots = strdup("core");
    }

    /* set the default mapping and ranking policies */
    if (NULL != rmaps_base_mapping_policy) {
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_set_mapping_policy(NULL, rmaps_base_mapping_policy))) {
            return rc;
        }
    }

    if (NULL != rmaps_base_ranking_policy) {
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_set_ranking_policy(NULL, rmaps_base_ranking_policy))) {
            return rc;
        }
    }

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_rmaps_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, rmaps, "PRRTE Mapping Subsystem",
                                 prrte_rmaps_base_register, prrte_rmaps_base_open, prrte_rmaps_base_close,
                                 prrte_rmaps_base_static_components, 0);

PRRTE_CLASS_INSTANCE(prrte_rmaps_base_selected_module_t,
                   prrte_list_item_t,
                   NULL, NULL);


static int check_modifiers(char *ck, prrte_job_t *jdata,
                           prrte_mapping_policy_t *tmp)
{
    char **ck2, *ptr;
    int i;
    uint16_t u16;
    bool inherit_given = false;
    bool noinherit_given = false;
    bool hwthread_cpus_given = false;
    bool core_cpus_given = false;
    bool oversubscribe_given = false;
    bool nooversubscribe_given = false;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "%s rmaps:base check modifiers with %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == ck) ? "NULL" : ck);

    if (NULL == ck) {
        return PRRTE_SUCCESS;
    }

    ck2 = prrte_argv_split(ck, ':');
    for (i=0; NULL != ck2[i]; i++) {
        if (0 == strcasecmp(ck2[i], "SPAN")) {
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_SPAN);
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_GIVEN);

        } else if (0 == strcasecmp(ck2[i], "OVERSUBSCRIBE")) {
            if (nooversubscribe_given) {
                /* conflicting directives */
                prrte_show_help("help-prrte-rmaps-base.txt", "conflicting-directives", true,
                                "OVERSUBSCRIBE", "NOOVERSUBSCRIBE");
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            PRRTE_UNSET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
            oversubscribe_given = true;

        } else if (0 == strcasecmp(ck2[i], "NOOVERSUBSCRIBE")) {
            if (oversubscribe_given) {
                /* conflicting directives */
                prrte_show_help("help-prrte-rmaps-base.txt", "conflicting-directives", true,
                                "OVERSUBSCRIBE", "NOOVERSUBSCRIBE");
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
            nooversubscribe_given = true;

        } else if (0 == strcasecmp(ck2[i], "DISPLAY")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYDEVEL")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYTOPO")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_TOPO, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYDIFF")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYALLOC")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_ALLOC, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DONOTLAUNCH")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strcasecmp(ck2[i], "NOLOCAL")) {
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_NO_USE_LOCAL);

        } else if (0 == strcasecmp(ck2[i], "XMLOUTPUT")) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_XML_OUTPUT, PRRTE_ATTR_GLOBAL,
                                NULL, PRRTE_BOOL);

        } else if (0 == strncasecmp(ck2[i], "PE-LIST", 7)) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            if (NULL == (ptr = strchr(ck2[i], '='))) {
                /* missing the value */
                prrte_show_help("help-prrte-rmaps-base.txt", "missing-value", true,
                                "mapping policy", "PE-LIST", ck2[i]);
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            ptr++;
            /* quick check - if it matches the default, then don't set it */
            if (NULL != prrte_hwloc_default_cpu_list) {
                if (0 != strcmp(prrte_hwloc_default_cpu_list, ptr)) {
                    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_CPUSET,
                                        PRRTE_ATTR_GLOBAL, ptr, PRRTE_STRING);
                }
            } else {
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_CPUSET,
                                    PRRTE_ATTR_GLOBAL, ptr, PRRTE_STRING);
            }

        } else if (0 == strncasecmp(ck2[i], "PE", 2)) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                "mapping policy", ck2[i]);
                return PRRTE_ERR_SILENT;
            }
            if (NULL == (ptr = strchr(ck2[i], '='))) {
                /* missing the value */
                prrte_show_help("help-prrte-rmaps-base.txt", "missing-value", true,
                                "mapping policy", "PE", ck2[i]);
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            ptr++;
            u16 = strtol(ptr, NULL, 10);
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_PES_PER_PROC, PRRTE_ATTR_GLOBAL,
                                &u16, PRRTE_UINT16);

        } else if (0 == strcasecmp(ck2[i], "INHERIT")) {
            if (noinherit_given) {
                /* conflicting directives */
                prrte_show_help("help-prrte-rmaps-base.txt", "conflicting-directives", true,
                                "INHERIT", "NOINHERIT");
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prrte_rmaps_base.inherit = true;
            } else {
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_INHERIT, PRRTE_ATTR_GLOBAL,
                                    NULL, PRRTE_BOOL);
            }
            inherit_given = true;

        } else if (0 == strcasecmp(ck2[i], "NOINHERIT")) {
            if (inherit_given) {
                /* conflicting directives */
                prrte_show_help("help-prrte-rmaps-base.txt", "conflicting-directives", true,
                                "INHERIT", "NOINHERIT");
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prrte_rmaps_base.inherit = false;
            } else {
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NOINHERIT, PRRTE_ATTR_GLOBAL,
                                    NULL, PRRTE_BOOL);
            }
            noinherit_given = true;

        } else if (0 == strncasecmp(ck2[i], "DEVICE", 6)) {
            if (NULL == (ptr = strchr(ck2[i], '='))) {
                /* missing the value */
                prrte_show_help("help-prrte-rmaps-base.txt", "missing-value", true,
                                "mapping policy", "DEVICE", ck2[i]);
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            ptr++;
            if (NULL == jdata) {
                prrte_rmaps_base.device = strdup(ptr);
            } else {
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_DIST_DEVICE, PRRTE_ATTR_GLOBAL,
                                    ptr, PRRTE_STRING);
            }

        } else if (0 == strcasecmp(ck2[i], "HWTCPUS")) {
            if (core_cpus_given) {
                prrte_show_help("help-prrte-rmaps-base.txt", "conflicting-directives", true,
                                "HWTCPUS", "CORECPUS");
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prrte_rmaps_base.hwthread_cpus = true;
            } else {
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_HWT_CPUS, PRRTE_ATTR_GLOBAL,
                                    NULL, PRRTE_BOOL);
            }
            hwthread_cpus_given = true;

        } else if (0 == strcasecmp(ck2[i], "CORECPUS")) {
            if (hwthread_cpus_given) {
                prrte_show_help("help-prrte-rmaps-base.txt", "conflicting-directives", true,
                                "HWTCPUS", "CORECPUS");
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prrte_rmaps_base.hwthread_cpus = false;
            } else {
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_CORE_CPUS, PRRTE_ATTR_GLOBAL,
                                    NULL, PRRTE_BOOL);
            }
            core_cpus_given = true;

        } else {
            /* unrecognized modifier */
            prrte_argv_free(ck2);
            return PRRTE_ERR_BAD_PARAM;
        }
    }
    prrte_argv_free(ck2);
    return PRRTE_SUCCESS;
}

int prrte_rmaps_base_set_mapping_policy(prrte_job_t *jdata, char *inspec)
{
    char *ck;
    char *ptr, *cptr;
    prrte_mapping_policy_t tmp;
    int rc;
    size_t len;
    char *spec = NULL;
    bool ppr = false;

    /* set defaults */
    tmp = 0;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "%s rmaps:base set policy with %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == inspec) ? "NULL" : inspec);

    if (NULL == inspec) {
        return PRRTE_SUCCESS;
    }

    spec = strdup(inspec);  // protect the input string
    /* see if a colon was included - if so, then we have a modifier */
    ck = strchr(spec, ':');
    if (NULL != ck) {
        *ck = '\0';  // terminate spec where the colon was
        ck++;    // step past the colon
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "%s rmaps:base policy %s modifiers %s provided",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), spec, ck);

        len = strlen(spec);
        if (0 < len && 0 == strncasecmp(spec, "ppr", len)) {
            if (NULL == jdata) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-policy", true,
                                "mapping", spec);
                free(spec);
                return PRRTE_ERR_SILENT;
            } else if (NULL == jdata->map) {
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                free(spec);
                return PRRTE_ERR_BAD_PARAM;
            }
            /* at this point, ck points to a string that contains at least
             * two fields (specifying the #procs/obj and the object we are
             * to map by). we have to allow additional modifiers here - e.g.,
             * specifying #pe's/proc or oversubscribe - so check for modifiers. if
             * they are present, ck will look like "N:obj:mod1,mod2,mod3"
             */
            if (NULL == (ptr = strchr(ck, ':'))) {
                /* this is an error - there had to be at least one
                 * colon to delimit the number from the object type
                 */
                prrte_show_help("help-prrte-rmaps-base.txt", "invalid-pattern", true, inspec);
                free(spec);
                return PRRTE_ERR_SILENT;
            }
            ptr++; // move past the colon
            /* at this point, ptr is pointing to the beginning of the string that describes
             * the object plus any modifiers (i.e., "obj:mod1,mod2". We first check to see if there
             * is another colon indicating that there are modifiers to the request */
            if (NULL != (cptr = strchr(ptr, ':'))) {
                /* there are modifiers, so we terminate the object string
                 * at the location of the colon */
                *cptr = '\0';
                /* step over that colon */
                cptr++;  // cptr now points to the start of the modifiers
           }
            /* now save the pattern */
            prrte_set_attribute(&jdata->attributes, PRRTE_JOB_PPR, PRRTE_ATTR_GLOBAL,
                                ck, PRRTE_STRING);
            PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_PPR);
            PRRTE_SET_MAPPING_DIRECTIVE(tmp, PRRTE_MAPPING_GIVEN);
            ppr = true;
            if (NULL == cptr) {
                /* there are no modifiers, so we are done */
                free(spec);
                spec = NULL;
                goto setpolicy;
            }
        } else {
            cptr = ck;
        }
        if (PRRTE_SUCCESS != (rc = check_modifiers(cptr, jdata, &tmp)) &&
            PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
            if (PRRTE_ERR_BAD_PARAM == rc) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-modifier", true, inspec);
            }
            if (NULL != spec) {
                free(spec);
            }
            return rc;
        }
        if (ppr) {
            /* we are done */
            free(spec);
            spec = NULL;
            goto setpolicy;
        }
    }

    if (NULL != spec) {
        len = strlen(spec);
        if (0 < len) {
            if (0 == strncasecmp(spec, "slot", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYSLOT);
            } else if (0 == strncasecmp(spec, "node", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYNODE);
            } else if (0 == strncasecmp(spec, "seq", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_SEQ);
            } else if (0 == strncasecmp(spec, "core", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYCORE);
            } else if (0 == strncasecmp(spec, "l1cache", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYL1CACHE);
            } else if (0 == strncasecmp(spec, "l2cache", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYL2CACHE);
            } else if (0 == strncasecmp(spec, "l3cache", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYL3CACHE);
            } else if (0 == strncasecmp(spec, "package", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYPACKAGE);
             } else if (0 == strncasecmp(spec, "hwthread", len)) {
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYHWTHREAD);
                /* if we are mapping processes to individual hwthreads, then
                 * we need to treat those hwthreads as separate cpus
                 */
                if (NULL == jdata) {
                    prrte_rmaps_base.hwthread_cpus = true;
                } else {
                    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_HWT_CPUS, PRRTE_ATTR_GLOBAL,
                                        NULL, PRRTE_BOOL);
                }
            } else if (0 == strncasecmp(spec, "dist", len)) {
                if (NULL == jdata) {
                    if (NULL == prrte_rmaps_base.device) {
                        prrte_show_help("help-prrte-rmaps-base.txt", "device-not-specified", true);
                        free(spec);
                        return PRRTE_ERR_SILENT;
                    }
                } else if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DIST_DEVICE, NULL, PRRTE_STRING)) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "device-not-specified", true);
                    free(spec);
                    return PRRTE_ERR_SILENT;
                }
                PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYDIST);
            } else {
                prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy", true, "mapping", spec);
                free(spec);
                return PRRTE_ERR_SILENT;
            }
            PRRTE_SET_MAPPING_DIRECTIVE(tmp, PRRTE_MAPPING_GIVEN);
        }
    }

  setpolicy:
    if (NULL != spec) {
        free(spec);
    }
    if (NULL != jdata) {
        if (NULL == jdata->map) {
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            return PRRTE_ERR_BAD_PARAM;
        }
        jdata->map->mapping = tmp;
    }

    return PRRTE_SUCCESS;
}

int prrte_rmaps_base_set_ranking_policy(prrte_job_t *jdata, char *spec)
{
    prrte_mapping_policy_t map, mapping;
    prrte_ranking_policy_t tmp;
    char **ck;
    size_t len;

    /* set default */
    tmp = 0;

    if (NULL == spec) {
        if (NULL != jdata) {
            if (NULL == jdata->map) {
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                return PRRTE_ERR_BAD_PARAM;
            }
            mapping = jdata->map->mapping;
        } else {
            mapping = prrte_rmaps_base.mapping;
        }
        /* check for map-by object directives - we set the
         * ranking to match if one was given
         */
        if (PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
            map = PRRTE_GET_MAPPING_POLICY(mapping);
            switch (map) {
                case PRRTE_MAPPING_BYSLOT:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
                    break;
                case PRRTE_MAPPING_BYNODE:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_NODE);
                    break;
                case PRRTE_MAPPING_BYCORE:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_CORE);
                    break;
                case PRRTE_MAPPING_BYL1CACHE:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L1CACHE);
                    break;
                case PRRTE_MAPPING_BYL2CACHE:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L2CACHE);
                    break;
                case PRRTE_MAPPING_BYL3CACHE:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L3CACHE);
                    break;
                case PRRTE_MAPPING_BYPACKAGE:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_PACKAGE);
                    break;
                case PRRTE_MAPPING_BYHWTHREAD:
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_HWTHREAD);
                    break;
                default:
                    /* anything not tied to a specific hw obj can rank by slot */
                    PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
                    break;
            }
        } else {
            /* if no map-by was given, default to by-slot */
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
        }
    } else {
        ck = prrte_argv_split(spec, ':');
        if (2 < prrte_argv_count(ck)) {
            /* incorrect format */
            prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy", true, "ranking", spec);
            prrte_argv_free(ck);
            return PRRTE_ERR_SILENT;
        }
        if (2 == prrte_argv_count(ck)) {
            if (0 == strncasecmp(ck[1], "span", strlen(ck[1]))) {
                PRRTE_SET_RANKING_DIRECTIVE(tmp, PRRTE_RANKING_SPAN);
            } else if (0 == strncasecmp(ck[1], "fill", strlen(ck[1]))) {
                PRRTE_SET_RANKING_DIRECTIVE(tmp, PRRTE_RANKING_FILL);
            } else {
                /* unrecognized modifier */
                prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-modifier", true, ck[1]);
                prrte_argv_free(ck);
                return PRRTE_ERR_SILENT;
            }
        }
        len = strlen(ck[0]);
        if (0 == strncasecmp(ck[0], "slot", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
        } else if (0 == strncasecmp(ck[0], "node", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_NODE);
        } else if (0 == strncasecmp(ck[0], "hwthread", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_HWTHREAD);
        } else if (0 == strncasecmp(ck[0], "core", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_CORE);
        } else if (0 == strncasecmp(ck[0], "l1cache", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L1CACHE);
        } else if (0 == strncasecmp(ck[0], "l2cache", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L2CACHE);
        } else if (0 == strncasecmp(ck[0], "l3cache", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L3CACHE);
        } else if (0 == strncasecmp(ck[0], "package", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_PACKAGE);
        } else {
            prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy", true,
                            "ranking", rmaps_base_ranking_policy);
            prrte_argv_free(ck);
            return PRRTE_ERR_SILENT;
        }
        prrte_argv_free(ck);
        PRRTE_SET_RANKING_DIRECTIVE(tmp, PRRTE_RANKING_GIVEN);
    }

    if (NULL == jdata) {
        prrte_rmaps_base.ranking = tmp;
    } else {
        if (NULL == jdata->map) {
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            return PRRTE_ERR_BAD_PARAM;
        }
        jdata->map->ranking = tmp;
    }

    return PRRTE_SUCCESS;
}
