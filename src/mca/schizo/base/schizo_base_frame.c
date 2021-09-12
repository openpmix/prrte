/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
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

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"

#include "src/mca/schizo/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */

#include "src/mca/schizo/base/static-components.h"

/*
 * Global variables
 */
prte_schizo_base_t prte_schizo_base = {{{0}}};
prte_schizo_API_module_t prte_schizo = {.parse_env = prte_schizo_base_parse_env,
                                        .detect_proxy = prte_schizo_base_detect_proxy,
                                        .setup_app = prte_schizo_base_setup_app,
                                        .setup_fork = prte_schizo_base_setup_fork,
                                        .setup_child = prte_schizo_base_setup_child,
                                        .job_info = prte_schizo_base_job_info,
                                        .check_sanity = prte_schizo_base_check_sanity,
                                        .finalize = prte_schizo_base_finalize};

static int prte_schizo_base_register(prte_mca_base_register_flag_t flags)
{
    /* test proxy launch */
    prte_schizo_base.test_proxy_launch = false;
    prte_mca_base_var_register("prte", "schizo", "base", "test_proxy_launch", "Test proxy launches",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_schizo_base.test_proxy_launch);
    return PRTE_SUCCESS;
}

static int prte_schizo_base_close(void)
{
    /* cleanup globals */
    PRTE_LIST_DESTRUCT(&prte_schizo_base.active_modules);

    return prte_mca_base_framework_components_close(&prte_schizo_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_schizo_base_open(prte_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PRTE_CONSTRUCT(&prte_schizo_base.active_modules, prte_list_t);

    /* Open up all available components */
    rc = prte_mca_base_framework_components_open(&prte_schizo_base_framework, flags);

    /* All done */
    return rc;
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, schizo, "PRTE Schizo Subsystem", prte_schizo_base_register,
                                prte_schizo_base_open, prte_schizo_base_close,
                                prte_schizo_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

typedef struct {
    char *name;
    char **conflicts;
} prte_schizo_conflicts_t;

static prte_schizo_conflicts_t mapby_modifiers[] = {{.name = "oversubscribe",
                                                     .conflicts = (char *[]){"nooversubscribe",
                                                                             NULL}},
                                                    {.name = ""}};

static prte_schizo_conflicts_t rankby_modifiers[] = {{.name = ""}};

static prte_schizo_conflicts_t bindto_modifiers[] = {{.name = ""}};

static prte_schizo_conflicts_t output_modifiers[] = {{.name = ""}};

static prte_schizo_conflicts_t display_modifiers[] = {{.name = ""}};

static int check_modifiers(char *modifier, char **checks, prte_schizo_conflicts_t *conflicts)
{
    int n, m, k;

    if (NULL == conflicts) {
        return PRTE_SUCCESS;
    }
    for (n = 0; 0 != strlen(conflicts[n].name); n++) {
        if (0 == strcasecmp(conflicts[n].name, modifier)) {
            for (m = 0; NULL != checks[m]; m++) {
                for (k = 0; NULL != conflicts[n].conflicts[k]; k++) {
                    if (0 == strcasecmp(checks[m], conflicts[n].conflicts[k])) {
                        return PRTE_ERR_BAD_PARAM;
                    }
                }
            }
            break;
        }
    }
    return PRTE_SUCCESS;
}

int prte_schizo_base_convert(char ***argv, int idx, int ntodelete,
                             char *option, char *directive,
                             char *modifier, bool report)
{
    bool found;
    char *p2, *help_str, *old_arg;
    int j, k, cnt, rc;
    char **pargs = *argv;
    char **tmp, **tmp2, *output;
    prte_schizo_conflicts_t *modifiers = NULL;

    /* pick the modifiers to be checked */
    if (NULL != modifier) {
        if (0 == strcmp(option, "--map-by")) {
            modifiers = mapby_modifiers;
        } else if (0 == strcmp(option, "--rank-by")) {
            modifiers = rankby_modifiers;
        } else if (0 == strcmp(option, "--bind-to")) {
            modifiers = bindto_modifiers;
        } else if (0 == strcmp(option, "--output")) {
            modifiers = output_modifiers;
        } else if (0 == strcmp(option, "--display")) {
            modifiers = display_modifiers;
        } else {
            prte_output(0, "UNRECOGNIZED OPTION: %s", option);
            return PRTE_ERR_BAD_PARAM;
        }
    }

    /* does the matching option already exist? */
    found = false;
    for (j = 0; NULL != pargs[j]; j++) {
        if (0 == strcmp(pargs[j], option)) {
            found = true;
            /* if it is a --tune, --output, or --display option, then we need to simply
             * append the comma-delimited list of files they gave to the existing one */
            if (0 == strcasecmp(option, "--tune") ||
                0 == strcasecmp(option, "--output") ||
                0 == strcasecmp(option, "--display")) {
                /* it is possible someone gave this option more than once - avoid that here
                 * while preserving ordering of files */
                if (j < idx) {
                    tmp = prte_argv_split(pargs[j + 1], ',');
                    tmp2 = prte_argv_split(pargs[idx + 1], ',');
                } else {
                    tmp2 = prte_argv_split(pargs[j + 1], ',');
                    tmp = prte_argv_split(pargs[idx + 1], ',');
                }
                for (k = 0; NULL != tmp2[k]; k++) {
                    prte_argv_append_unique_nosize(&tmp, tmp2[k]);
                }
                prte_argv_free(tmp2);
                p2 = prte_argv_join(tmp, ',');
                prte_argv_free(tmp);
                free(pargs[j + 1]);
                pargs[j + 1] = p2;
                break;
            }
            /* were we given a directive? */
            if (NULL != directive) {
                /* does it conflict? */
                if (':' != pargs[j + 1][0] &&
                    0 != strncasecmp(pargs[j + 1], directive, strlen(directive))) {
                    prte_asprintf(&help_str, "Conflicting directives \"%s %s\"",
                                  pargs[j + 1], directive);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-fail", true,
                                                   pargs[j], help_str);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(help_str);
                    return PRTE_ERR_BAD_PARAM;
                }
                /* no conflict on directive - see if we need to add it */
                if (':' == pargs[j + 1][0]) {
                    prte_asprintf(&p2, "%s%s", directive, pargs[j + 1]);
                    free(pargs[j + 1]);
                    pargs[j + 1] = p2;
                    break;
                }
            }
            /* were we given a modifier? */
            if (NULL != modifier) {
                /* add the modifier to this value */
                if (0 == strncmp(pargs[j + 1], "ppr", 3)) {
                    /* count the number of characters in the directive */
                    cnt = 0;
                    for (k = 0; '\0' != pargs[j + 1][k]; k++) {
                        if (':' == pargs[j + 1][k]) {
                            ++cnt;
                        }
                    }
                    /* there are two colons in the map-by directive */
                    if (2 == cnt) {
                        /* no modifier */
                        prte_asprintf(&p2, "%s:%s", pargs[j + 1], modifier);
                    } else {
                        /* there already is a modifier in the directive */
                        p2 = strrchr(pargs[j + 1], ':');
                        goto modify;
                    }
                } else if (NULL == (p2 = strchr(pargs[j + 1], ':'))) {
                    /* if this is the output option and the directive is
                     * either "dir" or "file", then treat it differently */
                    if (0 == strcasecmp(option, "--output") &&
                        (0 == strcasecmp(directive, "dir") ||
                         0 == strcasecmp(directive, "file"))) {
                        prte_asprintf(&p2, "%s=%s", directive, modifier);
                    } else {
                        prte_asprintf(&p2, "%s:%s", pargs[j + 1], modifier);
                    }
                } else {
                modify:
                    /* we already have modifiers - need to check for conflict with
                     * the one we were told to add */
                    ++p2; // step over the colon
                    tmp = prte_argv_split(p2, ',');
                    rc = check_modifiers(modifier, tmp, modifiers);
                    prte_argv_free(tmp);
                    if (PRTE_SUCCESS != rc) {
                        /* we have a conflict */
                        prte_asprintf(&help_str, "  Option %s\n  Conflicting modifiers \"%s %s\"",
                                      option, pargs[j + 1], modifier);
                        /* can't just call show_help as we want every instance to be reported */
                        output = prte_show_help_string("help-schizo-base.txt", "deprecated-fail",
                                                       true, pargs[j], help_str);
                        fprintf(stderr, "%s\n", output);
                        free(output);
                        free(help_str);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    /* if the directive is ppr, then the new modifier must be prepended */
                    if (NULL != directive && 0 == strcasecmp(directive, "ppr")) {
                        /* if we don't already have ppr in the modifier, be sure to add it */
                        if (NULL == strstr(modifier, "ppr")) {
                            prte_asprintf(&p2, "ppr:%s:%s", modifier, pargs[j + 1]);
                        } else {
                            prte_asprintf(&p2, "%s:%s", modifier, pargs[j + 1]);
                        }
                    } else {
                        prte_asprintf(&p2, "%s:%s", pargs[j + 1], modifier);
                    }
                }
                free(pargs[j + 1]);
                pargs[j + 1] = p2;
                if (report) {
                    prte_asprintf(&help_str, "%s %s", option, p2);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted",
                                                   true, pargs[idx], help_str);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(help_str);
                }
            }
        }
    }
    if (found) {
        if (0 < ntodelete) {
            /* we need to remove the indicated number of positions */
            prte_argv_delete(NULL, argv, idx, ntodelete);
        }
        return PRTE_SUCCESS;
    }

    /**** Get here if the specified option is not found in the
     **** current argv list
     ****/

    /* if this is the special "-N" option, we silently change it */
    if (0 == strcmp(pargs[idx], "-N")) {
        /* free the location of "-N" */
        free(pargs[idx]);
        /* replace it with the option */
        pargs[idx] = strdup(option);
        /* free the next position as it holds the ppn */
        free(pargs[idx + 1]);
        /* replace it with the directive */
        pargs[idx + 1] = strdup(directive);
        *argv = pargs;
        return PRTE_ERR_SILENT;
    }

    /* add the option */
    old_arg = strdup(pargs[idx]);
    free(pargs[idx]);
    pargs[idx] = strdup(option);
    help_str = NULL;
    /* if the argument is --am or --amca, then we got
     * here because there wasn't already a --tune argument.
     * In this case, we don't want to delete anything as
     * we are just substituting --tune for the original arg */
    if (0 != strcmp(old_arg, "--am") && 0 != strcasecmp(old_arg, "--amca") && 1 < ntodelete) {
        prte_argv_delete(NULL, argv, idx + 1, ntodelete - 1);
    }
    if (0 == strcasecmp(option, "--tune")) {
        p2 = NULL;
        prte_asprintf(&help_str, "%s %s", pargs[idx], pargs[idx + 1]);
    } else if (0 == strcasecmp(option, "--output")) {
        if (NULL != directive) {
            if (0 == strcasecmp(directive, "dir") ||
                0 == strcasecmp(directive, "file")) {
                prte_asprintf(&p2, "%s=%s", directive, modifier);
                free(pargs[idx+1]);
                pargs[idx+1] = p2;
                prte_asprintf(&help_str, "%s %s", pargs[idx], p2);
                p2 = NULL;
            } else {
                prte_asprintf(&p2, "%s:%s", directive, modifier);
            }
        } else {
            p2 = strdup(modifier);
        }
    } else if (0 == strcasecmp(option, "--display")) {
        if (NULL != directive) {
            prte_asprintf(&p2, "%s:%s", directive, modifier);
        } else {
            p2 = strdup(modifier);
        }
    } else if (NULL == directive) {
        prte_asprintf(&p2, ":%s", modifier);
    } else if (NULL == modifier) {
        p2 = strdup(directive);
    } else {
        prte_asprintf(&p2, "%s:%s", directive, modifier);
    }
    if (NULL == help_str) {
        if (NULL != p2) {
            prte_argv_insert_element(&pargs, idx + 1, p2);
            prte_asprintf(&help_str, "%s %s", pargs[idx], p2);
        } else {
            help_str = strdup(pargs[idx]);
        }
    }
    if (report) {
        /* can't just call show_help as we want every instance to be reported */
        output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true,
                                       old_arg, help_str);
        fprintf(stderr, "%s\n", output);
        free(output);
    }
    if (NULL != p2) {
        free(p2);
    }
    free(help_str);
    free(old_arg);
    *argv = pargs; // will have been reallocated

    return PRTE_SUCCESS;
}

static bool check_qualifiers(char *directive,
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
    v = prte_argv_join(valid, ',');
    prte_show_help("help-prte-rmaps-base.txt",
                   "unrecognized-qualifier", true,
                   directive, qual, v);
    free(v);
    return false;
}

static bool check_directives(char *directive,
                             char **valid,
                             char **quals,
                             char *dir)
{
    size_t n, m, len, l1, l2;
    char **args, **qls, *v, *q;
    char *pproptions[] = {"slot", "hwthread", "core", "l1cache",
                          "l2cache",  "l3cache", "package", "node",
                          NULL};
    bool found;

    /* if it starts with a ':', then these are just modifiers */
    if (':' == dir[0]) {
        return check_qualifiers(directive, quals, &dir[1]);
    }

    args = prte_argv_split(dir, ':');
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
                if (0 == strcmp(directive, "map-by") &&
                    0 == strcmp(args[0], "ppr")) {
                    /* unfortunately, this is a special case that
                     * must be checked separately due to the format
                     * of the qualifier */
                    v = NULL;
                    m = strtoul(args[1], &v, 10);
                    if (NULL != v && 0 < strlen(v)) {
                        /* the first entry had to be a pure number */
                        prte_asprintf(&v, "ppr:[Number of procs/object]:%s", args[2]);
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       directive, dir, v);
                        free(v);
                        prte_argv_free(args);
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
                        v = prte_argv_join(pproptions, ',');
                        prte_asprintf(&q, "ppr:%s:[%s]", args[1], v);
                        free(v);
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "unrecognized-qualifier", true,
                                       directive, dir, q);
                        free(q);
                        prte_argv_free(args);
                        return false;
                    }
                    if (NULL != args[3]) {
                        qls = prte_argv_split(args[3], ',');
                    } else {
                        prte_argv_free(args);
                        return true;
                    }
                } else {
                    qls = prte_argv_split(args[1], ',');
                }
               for (m=0; NULL != qls[m]; m++) {
                    if (!check_qualifiers(directive, quals, qls[m])) {
                        prte_argv_free(qls);
                        prte_argv_free(args);
                        return false;
                    }
                }
                prte_argv_free(qls);
                prte_argv_free(args);
                return true;
            }
            prte_argv_free(args);
            return true;
        }
    }
    v = prte_argv_join(valid, ',');
    prte_show_help("help-prte-rmaps-base.txt",
                   "unrecognized-directive", true,
                   directive, dir, v);
    prte_argv_free(args);
    return false;
}

int prte_schizo_base_sanity(prte_cmd_line_t *cmd_line)
{
    prte_value_t *pval;
    char *mappers[] = {"slot", "hwthread", "core", "l1cache", "l2cache",  "l3cache", "package",
                       "node", "seq",      "dist", "ppr",     "rankfile", NULL};
    char *mapquals[] = {"pe=", "span", "oversubscribe", "nooversubscribe", "nolocal",
                        "hwtcpus", "corecpus", "device=", "inherit", "noinherit", "pe-list=",
                        "file=", "donotlaunch", NULL};

    char *rankers[] = {"slot",    "hwthread", "core", "l1cache", "l2cache",
                       "l3cache", "package",  "node", NULL};
    char *rkquals[] = {"span", "fill", NULL};

    char *binders[] = {"none",    "hwthread", "core",    "l1cache",
                       "l2cache", "l3cache",  "package", NULL};
    char *bndquals[] = {"overload-allowed", "if-supported", "ordered", "report", NULL};

    char *outputs[] = {"tag", "timestamp", "xml", "merge-stderr-to-stdout", "directory", "filename", NULL};
    char *outquals[] = {"nocopy", NULL};

    char *displays[] = {"allocation", "map", "bind", "map-devel", "topo", NULL};

    bool hwtcpus = false;

    if (1 < prte_cmd_line_get_ninsts(cmd_line, "map-by")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "map-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "rank-by")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "rank-by");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "bind-to")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "bind-to");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "output")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "output");
        return PRTE_ERR_SILENT;
    }
    if (1 < prte_cmd_line_get_ninsts(cmd_line, "display")) {
        prte_show_help("help-schizo-base.txt", "multi-instances", true, "display");
        return PRTE_ERR_SILENT;
    }

    /* quick check that we have valid directives */
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "map-by", 0, 0))) {
        if (NULL != strcasestr(pval->value.data.string, "HWTCPUS")) {
            hwtcpus = true;
        }
        if (!check_directives("map-by", mappers, mapquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "rank-by", 0, 0))) {
        if (!check_directives("rank-by", rankers, rkquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "bind-to", 0, 0))) {
        if (!check_directives("bind-to", binders, bndquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
        if (0 == strncasecmp(pval->value.data.string, "HWTHREAD", strlen("HWTHREAD")) && !hwtcpus) {
            /* if we are told to bind-to hwt, then we have to be treating
             * hwt's as the allocatable unit */
            prte_show_help("help-prte-rmaps-base.txt", "invalid-combination", true);
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "output", 0, 0))) {
        if (!check_directives("output", outputs, outquals, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "display", 0, 0))) {
        if (!check_directives("display", displays, NULL, pval->value.data.string)) {
            return PRTE_ERR_SILENT;
        }
    }

    return PRTE_SUCCESS;
}

PRTE_CLASS_INSTANCE(prte_schizo_base_active_module_t, prte_list_item_t, NULL, NULL);
