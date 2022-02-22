/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_argv.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"

#include "src/mca/schizo/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public pmix_mca_base_component_t struct.
 */

#include "src/mca/schizo/base/static-components.h"

/*
 * Global variables
 */
prte_schizo_base_t prte_schizo_base = {
    .active_modules = PMIX_LIST_STATIC_INIT,
    .test_proxy_launch = false
};

static int prte_schizo_base_register(pmix_mca_base_register_flag_t flags)
{
    PRTE_HIDE_UNUSED_PARAMS(flags);

    /* test proxy launch */
    prte_schizo_base.test_proxy_launch = false;
    pmix_mca_base_var_register("prte", "schizo", "base", "test_proxy_launch", "Test proxy launches",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_schizo_base.test_proxy_launch);
    return PRTE_SUCCESS;
}

static int prte_schizo_base_close(void)
{
    /* cleanup globals */
    PMIX_LIST_DESTRUCT(&prte_schizo_base.active_modules);

    return pmix_mca_base_framework_components_close(&prte_schizo_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_schizo_base_open(pmix_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PMIX_CONSTRUCT(&prte_schizo_base.active_modules, pmix_list_t);

    /* Open up all available components */
    rc = pmix_mca_base_framework_components_open(&prte_schizo_base_framework, flags);

    /* All done */
    return rc;
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, schizo, "PRTE Schizo Subsystem", prte_schizo_base_register,
                                prte_schizo_base_open, prte_schizo_base_close,
                                prte_schizo_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

void prte_schizo_base_expose(char *param, char *prefix)
{
    char *value, *pm;

    value = strchr(param, '=');
    *value = '\0';
    ++value;
    pmix_asprintf(&pm, "%s_%s", prefix, param);
    setenv(pm, value, true);
    free(pm);
    --value;
    *value = '=';
}

bool prte_schizo_base_check_qualifiers(char *directive,
                                       char **valid,
                                       char *qual)
{
    size_t n, len, l1, l2;
    char *v;

    l1 = strlen(qual);
    for (n=0; NULL != valid[n]; n++) {
        l2 = strlen(valid[n]);
        len = (l1 < l2) ? l1 : l2;
        if (0 == strncasecmp(valid[n], qual, len)) {
            return true;
        }
    }
    v = pmix_argv_join(valid, ',');
    prte_show_help("help-prte-rmaps-base.txt",
                   "unrecognized-qualifier", true,
                   directive, qual, v);
    free(v);
    return false;
}

bool prte_schizo_base_check_directives(char *directive,
                                       char **valid,
                                       char **quals,
                                       char *dir)
{
    size_t n, m, len, l1, l2;
    char **args, **qls, *v, *q;
    char *pproptions[] = {
        PRTE_CLI_SLOT,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        PRTE_CLI_NODE,
        NULL
    };
    bool found;
    char *str;

    /* if it starts with a ':', then these are just modifiers */
    if (':' == dir[0]) {
        qls = pmix_argv_split(&dir[1], ',');
        for (m=0; NULL != qls[m]; m++) {
            if (!prte_schizo_base_check_qualifiers(directive, quals, qls[m])) {
                pmix_argv_free(qls);
                return false;
            }
        }
        pmix_argv_free(qls);
        return true;
    }

    /* always accept the "help" directive */
    if (0 == strcasecmp(dir, "help") ||
        0 == strcasecmp(dir, "-help") ||
        0 == strcasecmp(dir, "--help")) {
        return true;
    }

    args = pmix_argv_split(dir, ':');
    /* remove any '=' in the directive */
    if (NULL != (v = strchr(args[0], '='))) {
        *v = '\0';
    }
    for (n = 0; NULL != valid[n]; n++) {
        l1 = strlen(args[0]);
        l2 = strlen(valid[n]);
        len = (l1 < l2) ? l1 : l2;
        if (0 == strncasecmp(args[0], valid[n], len)) {
            /* valid directive - check any qualifiers */
            if (NULL != args[1] && NULL != quals) {
                if (0 == strcmp(directive, PRTE_CLI_MAPBY) &&
                    0 == strcmp(args[0], PRTE_CLI_PPR)) {
                    /* unfortunately, this is a special case that
                     * must be checked separately due to the format
                     * of the qualifier */
                    v = NULL;
                    m = strtoul(args[1], &v, 10);
                    if (NULL != v && 0 < strlen(v)) {
                        /* the first entry had to be a pure number */
                        pmix_asprintf(&v, "ppr:[Number of procs/object]:%s", args[2]);
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       directive, dir, v);
                        free(v);
                        pmix_argv_free(args);
                        return false;
                    }
                    found = false;
                    for (m=0; NULL != pproptions[m]; m++) {
                        if (0 == strcasecmp(args[2], pproptions[m])) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        v = pmix_argv_join(pproptions, ',');
                        pmix_asprintf(&q, "ppr:%s:[%s]", args[1], v);
                        free(v);
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       directive, dir, q);
                        free(q);
                        pmix_argv_free(args);
                        return false;
                    }
                    if (NULL != args[3]) {
                        qls = pmix_argv_split(args[3], ',');
                    } else {
                        pmix_argv_free(args);
                        return true;
                    }
                } else {
                    qls = pmix_argv_split(args[1], ',');
                }
               for (m=0; NULL != qls[m]; m++) {
                    if (!prte_schizo_base_check_qualifiers(directive, quals, qls[m])) {
                        pmix_argv_free(qls);
                        pmix_argv_free(args);
                        return false;
                    }
                }
                pmix_argv_free(qls);
                pmix_argv_free(args);
                return true;
            }
            pmix_argv_free(args);
            return true;
        }
    }
    v = pmix_argv_join(valid, ',');
    prte_show_help("help-prte-rmaps-base.txt",
                   "unrecognized-directive", true,
                   directive, dir, v);
    pmix_argv_free(args);
    return false;
}

typedef struct {
    const char *alias;
    const char *name;
} prte_synonym_t;

static prte_synonym_t synonyms[] = {
    {.alias = PRTE_CLI_MACHINEFILE, .name = PRTE_CLI_HOSTFILE},
    {.alias = PRTE_CLI_WD, .name = PRTE_CLI_WDIR},
    {.alias = NULL, .name = NULL}
};

static const char* check_synonym(const char *alias)
{
    int n;

    for (n=0; NULL != synonyms[n].alias; n++) {
        if (0 == strcmp(alias, synonyms[n].alias)) {
            return synonyms[n].name;
        }
    }
    return NULL;
}

static char *limits[] = {
    PRTE_CLI_PATH,
    PRTE_CLI_WDIR,
    PRTE_CLI_PSET,
    PRTE_CLI_NP,
    PRTE_CLI_KEEPALIVE,
    NULL
};

static int check_ndirs(prte_cli_item_t *opt)
{
    int n, count;
    char *param;

    for (n=0; NULL != limits[n]; n++) {
        if (0 == strcmp(opt->key, limits[n])) {
            count = pmix_argv_count(opt->values);
            if (1 > count) {
                param = pmix_argv_join(opt->values, ' ');
                prte_show_help("help-schizo-base.txt", "too-many-instances", true,
                               param, opt->key, count, 1);
                return PRTE_ERR_SILENT;
            }
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_sanity(pmix_cli_result_t *cmd_line)
{
    prte_cli_item_t *opt, *newopt;
    int n, rc, count;
    const char *tgt, *param;

    char *mappers[] = {
        PRTE_CLI_SLOT,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        PRTE_CLI_NODE,
        PRTE_CLI_SEQ,
        PRTE_CLI_DIST,
        PRTE_CLI_PPR,
        PRTE_CLI_RANKFILE,
        NULL
    };
    char *mapquals[] = {
        PRTE_CLI_PE,
        PRTE_CLI_SPAN,
        PRTE_CLI_OVERSUB,
        PRTE_CLI_NOOVER,
        PRTE_CLI_NOLOCAL,
        PRTE_CLI_HWTCPUS,
        PRTE_CLI_CORECPUS,
        PRTE_CLI_DEVICE,
        PRTE_CLI_INHERIT,
        PRTE_CLI_NOINHERIT,
        PRTE_CLI_PELIST,
        PRTE_CLI_QFILE,
        PRTE_CLI_NOLAUNCH,
        NULL
    };

    char *rankers[] = {
        PRTE_CLI_SLOT,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        PRTE_CLI_NODE,
        NULL
    };
    char *rkquals[] = {
        PRTE_CLI_SPAN,
        PRTE_CLI_FILL,
        NULL
    };

    char *binders[] = {
        PRTE_CLI_NONE,
        PRTE_CLI_HWT,
        PRTE_CLI_CORE,
        PRTE_CLI_L1CACHE,
        PRTE_CLI_L2CACHE,
        PRTE_CLI_L3CACHE,
        PRTE_CLI_NUMA,
        PRTE_CLI_PACKAGE,
        NULL
    };
    char *bndquals[] = {
        PRTE_CLI_OVERLOAD,
        PRTE_CLI_NOOVERLOAD,
        PRTE_CLI_IF_SUPP,
        PRTE_CLI_ORDERED,
        PRTE_CLI_REPORT,
        NULL
    };

    char *outputs[] = {
        PRTE_CLI_TAG,
        PRTE_CLI_RANK,
        PRTE_CLI_TIMESTAMP,
        PRTE_CLI_XML,
        PRTE_CLI_MERGE_ERROUT,
        PRTE_CLI_DIR,
        PRTE_CLI_FILE,
        NULL
    };
    char *outquals[] = {
        PRTE_CLI_NOCOPY,
        PRTE_CLI_RAW,
        NULL
    };

    char *displays[] = {
        PRTE_CLI_ALLOC,
        PRTE_CLI_MAP,
        PRTE_CLI_BIND,
        PRTE_CLI_MAPDEV,
        PRTE_CLI_TOPO,
        NULL
    };

    bool hwtcpus = false;

    if (1 < prte_cmd_line_get_ninsts(cmd_line, PRTE_CLI_MAPBY)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_MAPBY);
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, PRTE_CLI_RANKBY)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_RANKBY);
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, PRTE_CLI_BINDTO)) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, PRTE_CLI_BINDTO);
        return PRTE_ERR_SILENT;
    }

    /* check for synonyms */
    PMIX_LIST_FOREACH(opt, &cmd_line->instances, prte_cli_item_t) {
        if (NULL != (tgt = check_synonym(opt->key))) {
            if (NULL == opt->values) {
                // the presence is adequate
                if (NULL == prte_cmd_line_get_param(cmd_line, tgt)) {
                    newopt = PMIX_NEW(prte_cli_item_t);
                    newopt->key = strdup(tgt);
                    pmix_list_append(&cmd_line->instances, &newopt->super);
                }
            } else {
                for (n=0; NULL != opt->values[n]; n++) {
                    rc = prte_schizo_base_add_directive(cmd_line, opt->key, tgt,
                                                        opt->values[n], false);
                    if (PRTE_SUCCESS != rc) {
                        return rc;
                    }
                }
            }
        }
    }

    /* quick check that we have valid directives */
    opt = prte_cmd_line_get_param(cmd_line, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        if (NULL != strcasestr(opt->values[0], PRTE_CLI_HWTCPUS)) {
            hwtcpus = true;
        }
        if (!prte_schizo_base_check_directives(PRTE_CLI_MAPBY, mappers, mapquals, opt->values[0])) {
            return PRTE_ERR_SILENT;
        }
    }

    opt = prte_cmd_line_get_param(cmd_line, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_RANKBY, rankers, rkquals, opt->values[0])) {
            return PRTE_ERR_SILENT;
        }
    }

    opt = prte_cmd_line_get_param(cmd_line, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        if (!prte_schizo_base_check_directives(PRTE_CLI_BINDTO, binders, bndquals, opt->values[0])) {
            return PRTE_ERR_SILENT;
        }
        if (0 == strncasecmp(opt->values[0], PRTE_CLI_HWT, strlen(PRTE_CLI_HWT)) && !hwtcpus) {
            /* if we are told to bind-to hwt, then we have to be treating
             * hwt's as the allocatable unit */
            prte_show_help("help-prte-rmaps-base.txt", "invalid-combination", true);
            return PRTE_ERR_SILENT;
        }
    }

    opt = prte_cmd_line_get_param(cmd_line, PRTE_CLI_OUTPUT);
    if (NULL != opt) {
        for (n=0; NULL != opt->values[n]; n++) {
            if (!prte_schizo_base_check_directives(PRTE_CLI_OUTPUT, outputs, outquals, opt->values[n])) {
                return PRTE_ERR_SILENT;
            }
        }
    }

    opt = prte_cmd_line_get_param(cmd_line, PRTE_CLI_DISPLAY);
    if (NULL != opt) {
        for (n=0; NULL != opt->values[n]; n++) {
            if (!prte_schizo_base_check_directives(PRTE_CLI_DISPLAY, displays, NULL, opt->values[n])) {
                return PRTE_ERR_SILENT;
            }
        }
    }

    // check too many directives
    PMIX_LIST_FOREACH(opt, &cmd_line->instances, prte_cli_item_t) {
        rc = check_ndirs(opt);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    return PRTE_SUCCESS;
}

PMIX_CLASS_INSTANCE(prte_schizo_base_active_module_t, pmix_list_item_t, NULL, NULL);
