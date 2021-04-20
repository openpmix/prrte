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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */

#include "src/mca/rmaps/base/static-components.h"

/*
 * Global variables
 */
prte_rmaps_base_t prte_rmaps_base = {{{0}}};

/*
 * Local variables
 */
static char *rmaps_base_mapping_policy = NULL;
static char *rmaps_base_ranking_policy = NULL;
static bool rmaps_base_inherit = false;

static int prte_rmaps_base_register(prte_mca_base_register_flag_t flags)
{
    /* define default mapping policy */
    rmaps_base_mapping_policy = NULL;
    (void) prte_mca_base_var_register(
        "prte", "rmaps", "default", "mapping_policy",
        "Default mapping Policy [slot | hwthread | core (default:np<=2) | l1cache | "
        "l2cache | l3cache | package (default:np>2) | node | seq | dist | ppr | rankfile],"
        " with supported colon-delimited modifiers: PE=y (for multiple cpus/proc), "
        "SPAN, OVERSUBSCRIBE, NOOVERSUBSCRIBE, NOLOCAL, HWTCPUS, CORECPUS, "
        "DEVICE=dev (for dist policy), INHERIT, NOINHERIT, PE-LIST=a,b (comma-delimited "
        "ranges of cpus to use for this job), FILE=%s (path to file containing sequential "
        "or rankfile entries)",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_mapping_policy);

    /* define default ranking policy */
    rmaps_base_ranking_policy = NULL;
    (void) prte_mca_base_var_register(
        "prte", "rmaps", "default", "ranking_policy",
        "Default ranking Policy [slot (default:np<=2) | hwthread | core | l1cache "
        "| l2cache | l3cache | package (default:np>2) | node], with modifier :SPAN or :FILL",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_ranking_policy);

    rmaps_base_inherit = false;
    (void) prte_mca_base_var_register("prte", "rmaps", "default", "inherit",
                                      "Whether child jobs shall inherit mapping/ranking/binding "
                                      "directives from their parent by default",
                                      PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_inherit);

    return PRTE_SUCCESS;
}

static int prte_rmaps_base_close(void)
{
    prte_list_item_t *item;

    /* cleanup globals */
    while (NULL != (item = prte_list_remove_first(&prte_rmaps_base.selected_modules))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_rmaps_base.selected_modules);

    return prte_mca_base_framework_components_close(&prte_rmaps_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_rmaps_base_open(prte_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PRTE_CONSTRUCT(&prte_rmaps_base.selected_modules, prte_list_t);
    prte_rmaps_base.mapping = 0;
    prte_rmaps_base.ranking = 0;
    prte_rmaps_base.inherit = rmaps_base_inherit;
    prte_rmaps_base.hwthread_cpus = false;
    if (NULL == prte_set_slots) {
        prte_set_slots = strdup("core");
    }

    /* set the default mapping and ranking policies */
    if (NULL != rmaps_base_mapping_policy) {
        if (PRTE_SUCCESS
            != (rc = prte_rmaps_base_set_mapping_policy(NULL, rmaps_base_mapping_policy))) {
            return rc;
        }
    }

    if (NULL != rmaps_base_ranking_policy) {
        if (PRTE_SUCCESS
            != (rc = prte_rmaps_base_set_ranking_policy(NULL, rmaps_base_ranking_policy))) {
            return rc;
        }
    }

    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_rmaps_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, rmaps, "PRTE Mapping Subsystem", prte_rmaps_base_register,
                                prte_rmaps_base_open, prte_rmaps_base_close,
                                prte_rmaps_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

PRTE_CLASS_INSTANCE(prte_rmaps_base_selected_module_t, prte_list_item_t, NULL, NULL);

static int check_modifiers(char *ck, prte_job_t *jdata, prte_mapping_policy_t *tmp)
{
    char **ck2, *ptr, *temp_parm, *temp_token, *parm_delimiter;
    int i;
    uint16_t u16;
    bool inherit_given = false;
    bool noinherit_given = false;
    bool hwthread_cpus_given = false;
    bool core_cpus_given = false;
    bool oversubscribe_given = false;
    bool nooversubscribe_given = false;
    prte_job_t *djob;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "%s rmaps:base check modifiers with %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == ck) ? "NULL" : ck);

    if (NULL == ck) {
        return PRTE_SUCCESS;
    }

    ck2 = prte_argv_split(ck, ':');
    for (i = 0; NULL != ck2[i]; i++) {
        if (0 == strcasecmp(ck2[i], "SPAN")) {
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_SPAN);
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_GIVEN);

        } else if (0 == strcasecmp(ck2[i], "OVERSUBSCRIBE")) {
            if (nooversubscribe_given) {
                /* conflicting directives */
                prte_show_help("help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "OVERSUBSCRIBE", "NOOVERSUBSCRIBE");
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            PRTE_UNSET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_SUBSCRIBE_GIVEN);
            oversubscribe_given = true;

        } else if (0 == strcasecmp(ck2[i], "NOOVERSUBSCRIBE")) {
            if (oversubscribe_given) {
                /* conflicting directives */
                prte_show_help("help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "OVERSUBSCRIBE", "NOOVERSUBSCRIBE");
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_SUBSCRIBE_GIVEN);
            nooversubscribe_given = true;

        } else if (0 == strcasecmp(ck2[i], "DISPLAY")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, PRTE_ATTR_GLOBAL, NULL,
                               PMIX_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYDEVEL")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, PRTE_ATTR_GLOBAL,
                               NULL, PMIX_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYTOPO")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_TOPO, PRTE_ATTR_GLOBAL, NULL,
                               PMIX_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DISPLAYALLOC")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_ALLOC, PRTE_ATTR_GLOBAL, NULL,
                               PMIX_BOOL);

        } else if (0 == strcasecmp(ck2[i], "DONOTLAUNCH")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, PRTE_ATTR_GLOBAL, NULL,
                               PMIX_BOOL);
            /* if we are not in a persistent DVM, then make sure we don't try to launch
             * the daemons either */
            if (!prte_persistent) {
                djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
                prte_set_attribute(&djob->attributes, PRTE_JOB_DO_NOT_LAUNCH, PRTE_ATTR_GLOBAL,
                                   NULL, PMIX_BOOL);
            }

        } else if (0 == strcasecmp(ck2[i], "NOLOCAL")) {
            PRTE_SET_MAPPING_DIRECTIVE(*tmp, PRTE_MAPPING_NO_USE_LOCAL);

        } else if (0 == strcasecmp(ck2[i], "TAGOUTPUT")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, PRTE_ATTR_GLOBAL, NULL,
                               PMIX_BOOL);

        } else if (0 == strcasecmp(ck2[i], "TIMESTAMPOUTPUT")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, PRTE_ATTR_GLOBAL,
                               NULL, PMIX_BOOL);

        } else if (0 == strcasecmp(ck2[i], "XMLOUTPUT")) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, PRTE_ATTR_GLOBAL, NULL,
                               PMIX_BOOL);

        } else if (0 == strncasecmp(ck2[i], "PE-LIST=", 8)) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            ptr = &ck2[i][8];
            /* Verify the option parmeter is a list of numeric tokens */
            temp_parm = strdup(ptr);
            temp_token = strtok(temp_parm, ",");
            while (NULL != temp_token) {
                u16 = strtol(temp_token, &parm_delimiter, 10);
                if ('\0' != *parm_delimiter) {
                    prte_show_help("help-prte-rmaps-base.txt", "invalid-value", true,
                                   "mapping policy", "PE", ck2[i]);
                    prte_argv_free(ck2);
                    free(temp_parm);
                    return PRTE_ERR_SILENT;
                }
                temp_token = strtok(NULL, ",");
            }
            free(temp_parm);
            /* quick check - if it matches the default, then don't set it */
            if (NULL != prte_hwloc_default_cpu_list) {
                if (0 != strcmp(prte_hwloc_default_cpu_list, ptr)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL, ptr,
                                       PMIX_STRING);
                }
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL, ptr,
                                   PMIX_STRING);
            }

        } else if (0 == strncasecmp(ck2[i], "PE=", 3)) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                               "mapping policy", ck2[i]);
                return PRTE_ERR_SILENT;
            }
            /* Numeric value must immediately follow '=' (PE=2) */
            u16 = strtol(&ck2[i][3], &ptr, 10);
            if ('\0' != *ptr) {
                /* missing the value or value is invalid */
                prte_show_help("help-prte-rmaps-base.txt", "invalid-value", true, "mapping policy",
                               "PE", ck2[i]);
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, &u16,
                               PMIX_UINT16);

        } else if (0 == strcasecmp(ck2[i], "INHERIT")) {
            if (noinherit_given) {
                /* conflicting directives */
                prte_show_help("help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "INHERIT", "NOINHERIT");
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prte_rmaps_base.inherit = true;
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_INHERIT, PRTE_ATTR_GLOBAL, NULL,
                                   PMIX_BOOL);
            }
            inherit_given = true;

        } else if (0 == strcasecmp(ck2[i], "NOINHERIT")) {
            if (inherit_given) {
                /* conflicting directives */
                prte_show_help("help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "INHERIT", "NOINHERIT");
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prte_rmaps_base.inherit = false;
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_NOINHERIT, PRTE_ATTR_GLOBAL, NULL,
                                   PMIX_BOOL);
            }
            noinherit_given = true;

        } else if (0 == strncasecmp(ck2[i], "DEVICE=", 7)) {
            if ('\0' == ck2[i][7]) {
                /* missing the value */
                prte_show_help("help-prte-rmaps-base.txt", "missing-value", true, "mapping policy",
                               "DEVICE", ck2[i]);
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prte_rmaps_base.device = strdup(&ck2[i][7]);
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_DIST_DEVICE, PRTE_ATTR_GLOBAL,
                                   &ck2[i][7], PMIX_STRING);
            }

        } else if (0 == strcasecmp(ck2[i], "HWTCPUS")) {
            if (core_cpus_given) {
                prte_show_help("help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "HWTCPUS", "CORECPUS");
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prte_rmaps_base.hwthread_cpus = true;
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL,
                                   PMIX_BOOL);
            }
            hwthread_cpus_given = true;

        } else if (0 == strcasecmp(ck2[i], "CORECPUS")) {
            if (hwthread_cpus_given) {
                prte_show_help("help-prte-rmaps-base.txt", "conflicting-directives", true,
                               "HWTCPUS", "CORECPUS");
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prte_rmaps_base.hwthread_cpus = false;
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL,
                                   PMIX_BOOL);
            }
            core_cpus_given = true;

        } else if (0 == strncasecmp(ck2[i], "FILE=", 5)) {
            if ('\0' == ck2[i][5]) {
                /* missing the value */
                prte_show_help("help-prte-rmaps-base.txt", "missing-value", true, "mapping policy",
                               "FILE", ck2[i]);
                prte_argv_free(ck2);
                return PRTE_ERR_SILENT;
            }
            if (NULL == jdata) {
                prte_rmaps_base.file = strdup(&ck2[i][5]);
            } else {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FILE, PRTE_ATTR_GLOBAL, &ck2[i][5],
                                   PMIX_STRING);
            }

        } else {
            /* unrecognized modifier */
            prte_argv_free(ck2);
            return PRTE_ERR_BAD_PARAM;
        }
    }
    prte_argv_free(ck2);
    return PRTE_SUCCESS;
}

int prte_rmaps_base_set_mapping_policy(prte_job_t *jdata, char *inspec)
{
    char *ck;
    char *ptr, *cptr;
    prte_mapping_policy_t tmp;
    int rc;
    size_t len;
    char *spec = NULL;
    bool ppr = false;

    /* set defaults */
    tmp = 0;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "%s rmaps:base set policy with %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == inspec) ? "NULL" : inspec);

    if (NULL == inspec) {
        return PRTE_SUCCESS;
    }

    spec = strdup(inspec); // protect the input string
    /* see if a colon was included - if so, then we have a modifier */
    ck = strchr(spec, ':');
    if (NULL != ck) {
        *ck = '\0'; // terminate spec where the colon was
        ck++;       // step past the colon
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "%s rmaps:base policy %s modifiers %s provided",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), spec, ck);

        len = strlen(spec);
        if (0 < len && 0 == strncasecmp(spec, "ppr", len)) {
            if (NULL == jdata) {
                prte_show_help("help-prte-rmaps-base.txt", "unsupported-default-policy", true,
                               "mapping", spec);
                free(spec);
                return PRTE_ERR_SILENT;
            } else if (NULL == jdata->map) {
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                free(spec);
                return PRTE_ERR_BAD_PARAM;
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
                prte_show_help("help-prte-rmaps-base.txt", "invalid-pattern", true, inspec);
                free(spec);
                return PRTE_ERR_SILENT;
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
                cptr++; // cptr now points to the start of the modifiers
            }
            /* now save the pattern */
            prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, ck, PMIX_STRING);
            PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_PPR);
            PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);
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
        if (PRTE_SUCCESS != (rc = check_modifiers(cptr, jdata, &tmp))
            && PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            if (PRTE_ERR_BAD_PARAM == rc) {
                prte_show_help("help-prte-rmaps-base.txt", "unrecognized-modifier", true, inspec);
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
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYSLOT);
            } else if (0 == strncasecmp(spec, "node", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYNODE);
            } else if (0 == strncasecmp(spec, "seq", len)) {
                /* there are several mechanisms by which the file specifying
                 * the sequence can be passed, so not really feasible to check
                 * it here */
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_SEQ);
            } else if (0 == strncasecmp(spec, "core", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYCORE);
            } else if (0 == strncasecmp(spec, "l1cache", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL1CACHE);
            } else if (0 == strncasecmp(spec, "l2cache", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL2CACHE);
            } else if (0 == strncasecmp(spec, "l3cache", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYL3CACHE);
            } else if (0 == strncasecmp(spec, "package", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYPACKAGE);
            } else if (0 == strcasecmp(spec, "rankfile")) {
                /* check that the file was given */
                if ((NULL == jdata && NULL == prte_rmaps_base.file)
                    || !prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, NULL, PMIX_STRING)) {
                    prte_show_help("help-prte-rmaps-base.txt", "rankfile-no-filename", true);
                    return PRTE_ERR_BAD_PARAM;
                }
                /* if they asked for rankfile and didn't specify one, but did
                 * provide one via MCA param, then use it */
                if (NULL != jdata) {
                    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, NULL, PMIX_STRING)) {
                        if (NULL == prte_rmaps_base.file) {
                            /* also not allowed */
                            prte_show_help("help-prte-rmaps-base.txt", "rankfile-no-filename",
                                           true);
                            return PRTE_ERR_BAD_PARAM;
                        }
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_FILE, PRTE_ATTR_GLOBAL,
                                           prte_rmaps_base.file, PMIX_STRING);
                    }
                }
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYUSER);
            } else if (0 == strncasecmp(spec, "hwthread", len)) {
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYHWTHREAD);
                /* if we are mapping processes to individual hwthreads, then
                 * we need to treat those hwthreads as separate cpus
                 */
                if (NULL == jdata) {
                    prte_rmaps_base.hwthread_cpus = true;
                } else {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL,
                                       NULL, PMIX_BOOL);
                }
            } else if (0 == strncasecmp(spec, "dist", len)) {
                if (NULL == jdata) {
                    if (NULL == prte_rmaps_base.device) {
                        prte_show_help("help-prte-rmaps-base.txt", "device-not-specified", true);
                        free(spec);
                        return PRTE_ERR_SILENT;
                    }
                } else if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DIST_DEVICE, NULL,
                                               PMIX_STRING)) {
                    prte_show_help("help-prte-rmaps-base.txt", "device-not-specified", true);
                    free(spec);
                    return PRTE_ERR_SILENT;
                }
                PRTE_SET_MAPPING_POLICY(tmp, PRTE_MAPPING_BYDIST);
            } else {
                prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "mapping",
                               spec);
                free(spec);
                return PRTE_ERR_SILENT;
            }
            PRTE_SET_MAPPING_DIRECTIVE(tmp, PRTE_MAPPING_GIVEN);
        }
    }

setpolicy:
    if (NULL != spec) {
        free(spec);
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

int prte_rmaps_base_set_ranking_policy(prte_job_t *jdata, char *spec)
{
    prte_mapping_policy_t map, mapping;
    prte_ranking_policy_t tmp;
    char **ck;
    size_t len;

    /* set default */
    tmp = 0;

    if (NULL == spec) {
        if (NULL != jdata) {
            if (NULL == jdata->map) {
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                return PRTE_ERR_BAD_PARAM;
            }
            mapping = jdata->map->mapping;
        } else {
            mapping = prte_rmaps_base.mapping;
        }
        /* check for map-by object directives - we set the
         * ranking to match if one was given
         */
        if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(mapping)) {
            map = PRTE_GET_MAPPING_POLICY(mapping);
            switch (map) {
            case PRTE_MAPPING_BYSLOT:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SLOT);
                break;
            case PRTE_MAPPING_BYNODE:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_NODE);
                break;
            case PRTE_MAPPING_BYCORE:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_CORE);
                break;
            case PRTE_MAPPING_BYL1CACHE:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_L1CACHE);
                break;
            case PRTE_MAPPING_BYL2CACHE:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_L2CACHE);
                break;
            case PRTE_MAPPING_BYL3CACHE:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_L3CACHE);
                break;
            case PRTE_MAPPING_BYPACKAGE:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_PACKAGE);
                break;
            case PRTE_MAPPING_BYHWTHREAD:
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_HWTHREAD);
                break;
            default:
                /* anything not tied to a specific hw obj can rank by slot */
                PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SLOT);
                break;
            }
        } else {
            /* if no map-by was given, default to by-slot */
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SLOT);
        }
    } else {
        ck = prte_argv_split(spec, ':');
        if (2 < prte_argv_count(ck)) {
            /* incorrect format */
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "ranking",
                           spec);
            prte_argv_free(ck);
            return PRTE_ERR_SILENT;
        }
        if (2 == prte_argv_count(ck)) {
            if (0 == strncasecmp(ck[1], "span", strlen(ck[1]))) {
                PRTE_SET_RANKING_DIRECTIVE(tmp, PRTE_RANKING_SPAN);
            } else if (0 == strncasecmp(ck[1], "fill", strlen(ck[1]))) {
                PRTE_SET_RANKING_DIRECTIVE(tmp, PRTE_RANKING_FILL);
            } else {
                /* unrecognized modifier */
                prte_show_help("help-prte-rmaps-base.txt", "unrecognized-modifier", true, ck[1]);
                prte_argv_free(ck);
                return PRTE_ERR_SILENT;
            }
        }
        len = strlen(ck[0]);
        if (0 == strncasecmp(ck[0], "slot", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_SLOT);
        } else if (0 == strncasecmp(ck[0], "node", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_NODE);
        } else if (0 == strncasecmp(ck[0], "hwthread", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_HWTHREAD);
        } else if (0 == strncasecmp(ck[0], "core", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_CORE);
        } else if (0 == strncasecmp(ck[0], "l1cache", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_L1CACHE);
        } else if (0 == strncasecmp(ck[0], "l2cache", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_L2CACHE);
        } else if (0 == strncasecmp(ck[0], "l3cache", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_L3CACHE);
        } else if (0 == strncasecmp(ck[0], "package", len)) {
            PRTE_SET_RANKING_POLICY(tmp, PRTE_RANK_BY_PACKAGE);
        } else {
            prte_show_help("help-prte-rmaps-base.txt", "unrecognized-policy", true, "ranking",
                           rmaps_base_ranking_policy);
            prte_argv_free(ck);
            return PRTE_ERR_SILENT;
        }
        prte_argv_free(ck);
        PRTE_SET_RANKING_DIRECTIVE(tmp, PRTE_RANKING_GIVEN);
    }

    if (NULL == jdata) {
        prte_rmaps_base.ranking = tmp;
    } else {
        if (NULL == jdata->map) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return PRTE_ERR_BAD_PARAM;
        }
        jdata->map->ranking = tmp;
    }

    return PRTE_SUCCESS;
}
