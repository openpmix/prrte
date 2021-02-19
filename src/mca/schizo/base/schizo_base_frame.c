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

#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

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
prte_schizo_API_module_t prte_schizo = {
    .define_cli = prte_schizo_base_define_cli,
    .parse_cli = prte_schizo_base_parse_cli,
    .parse_deprecated_cli = prte_schizo_base_parse_deprecated_cli,
    .parse_proxy_cli = prte_schizo_base_parse_proxy_cli,
    .parse_env = prte_schizo_base_parse_env,
    .detect_proxy = prte_schizo_base_detect_proxy,
    .define_session_dir = prte_schizo_base_define_session_dir,
    .allow_run_as_root = prte_schizo_base_allow_run_as_root,
    .wrap_args = prte_schizo_base_wrap_args,
    .setup_app = prte_schizo_base_setup_app,
    .setup_fork = prte_schizo_base_setup_fork,
    .setup_child = prte_schizo_base_setup_child,
    .job_info = prte_schizo_base_job_info,
    .get_remaining_time = prte_schizo_base_get_remaining_time,
    .finalize = prte_schizo_base_finalize
};

static char *personalities = "prte";

static int prte_schizo_base_register(prte_mca_base_register_flag_t flags)
{
    /* pickup any defined personalities */
    prte_mca_base_var_register("prte", "schizo", "base", "personalities",
                                "Comma-separated list of personalities",
                                PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                PRTE_MCA_BASE_VAR_FLAG_NONE,
                                PRTE_INFO_LVL_9,
                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &personalities);

    /* test proxy launch */
    prte_schizo_base.test_proxy_launch = false;
    prte_mca_base_var_register("prte", "schizo", "base", "test_proxy_launch",
                                "Test proxy launches",
                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                PRTE_MCA_BASE_VAR_FLAG_NONE,
                                PRTE_INFO_LVL_9,
                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &prte_schizo_base.test_proxy_launch);
    return PRTE_SUCCESS;
}

static int prte_schizo_base_close(void)
{
    /* cleanup globals */
    PRTE_LIST_DESTRUCT(&prte_schizo_base.active_modules);
    if (NULL != prte_schizo_base.personalities) {
        prte_argv_free(prte_schizo_base.personalities);
    }

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
    prte_schizo_base.personalities = NULL;
    if (NULL != personalities) {
        prte_schizo_base.personalities = prte_argv_split(personalities, ',');
    }

    /* Open up all available components */
    rc = prte_mca_base_framework_components_open(&prte_schizo_base_framework, flags);

    /* All done */
    return rc;
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, schizo, "PRTE Schizo Subsystem",
                                 prte_schizo_base_register,
                                 prte_schizo_base_open, prte_schizo_base_close,
                                 prte_schizo_base_static_components, PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static void cvcon(prte_convertor_t *p)
{
    p->options = NULL;
}
static void cvdes(prte_convertor_t *p)
{
    if (NULL != p->options) {
        prte_argv_free(p->options);
    }
}
PRTE_CLASS_INSTANCE(prte_convertor_t,
                     prte_list_item_t,
                     cvcon, cvdes);

typedef struct {
    char *name;
    char **conflicts;
} prte_schizo_conflicts_t;

static prte_schizo_conflicts_t mapby_modifiers[] = {
    {.name = "oversubscribe", .conflicts = (char *[]){"nooversubscribe", NULL}},
    {.name = ""}
};

static prte_schizo_conflicts_t rankby_modifiers[] = {
    {.name = ""}
};

static prte_schizo_conflicts_t bindto_modifiers[] = {
    {.name = ""}
};

static int check_modifiers(char *modifier, char **checks, prte_schizo_conflicts_t *conflicts)
{
    int n, m, k;

    if (NULL == conflicts) {
        return PRTE_SUCCESS;
    }
    for (n=0; 0 != strlen(conflicts[n].name); n++) {
        if (0 == strcasecmp(conflicts[n].name, modifier)) {
            for (m=0; NULL != checks[m]; m++) {
                for (k=0; NULL != conflicts[n].conflicts[k]; k++) {
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
        } else  {
            prte_output(0, "UNRECOGNIZED OPTION: %s", option);
            return PRTE_ERR_BAD_PARAM;
        }
    }

    /* does the matching option already exist? */
    found = false;
    for (j=0; NULL != pargs[j]; j++) {
        if (0 == strcmp(pargs[j], option)) {
            found = true;
            /* if it is a --tune option, then we need to simply append the
             * comma-delimited list of files they gave to the existing one */
            if (0 == strcasecmp(option, "--tune")) {
                /* it is possible someone gave this option more than once - avoid that here
                 * while preserving ordering of files */
                if (j < idx) {
                    tmp = prte_argv_split(pargs[j+1], ',');
                    tmp2 = prte_argv_split(pargs[idx+1], ',');
                } else {
                    tmp2 = prte_argv_split(pargs[j+1], ',');
                    tmp = prte_argv_split(pargs[idx+1], ',');
                }
                for (k=0; NULL != tmp2[k]; k++) {
                    prte_argv_append_unique_nosize(&tmp, tmp2[k]);
                }
                prte_argv_free(tmp2);
                p2 = prte_argv_join(tmp, ',');
                prte_argv_free(tmp);
                free(pargs[j+1]);
                pargs[j+1] = p2;
                if (0 != strcmp(pargs[j], "--tune")) {
                    prte_asprintf(&help_str, "%s %s", option, p2);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true,
                                                    pargs[idx], help_str);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(help_str);
                }
                break;
            }
            /* were we given a directive? */
            if (NULL != directive) {
                /* does it conflict? */
                if (0 != strncasecmp(pargs[j+1], directive, strlen(directive))) {
                    prte_asprintf(&help_str, "Conflicting directives \"%s %s\"", pargs[j+1], directive);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-fail", true,
                                                    pargs[j], help_str);
                    fprintf(stderr, "%s\n", output);
                    free(output);
                    free(help_str);
                    return PRTE_ERR_BAD_PARAM;
                }
                /* if they match, then nothing further to do */
            }
            /* were we given a modifier? */
            if (NULL != modifier) {
                /* add the modifier to this value */
                if (0 == strncmp(pargs[j+1], "ppr", 3)) {
                    /* count the number of characters in the directive */
                    cnt = 0;
                    for (k=0; '\0' != pargs[j+1][k]; k++) {
                        if (':' == pargs[j+1][k]) {
                            ++cnt;
                        }
                    }
                    /* there are two colons in the map-by directive */
                    if (2 == cnt) {
                        /* no modifier */
                        prte_asprintf(&p2, "%s:%s", pargs[j+1], modifier);
                    } else {
                        /* there already is a modifier in the directive */
                        p2 = strrchr(pargs[j+1], ':');
                        goto modify;
                    }
                } else if (NULL == (p2 = strchr(pargs[j+1], ':'))) {
                    prte_asprintf(&p2, "%s:%s", pargs[j+1], modifier);
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
                        prte_asprintf(&help_str, "  Option %s\n  Conflicting modifiers \"%s %s\"", option, pargs[j+1], modifier);
                        /* can't just call show_help as we want every instance to be reported */
                        output = prte_show_help_string("help-schizo-base.txt", "deprecated-fail", true,
                                                        pargs[j], help_str);
                        fprintf(stderr, "%s\n", output);
                        free(output);
                        free(help_str);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    prte_asprintf(&p2, "%s:%s", pargs[j+1], modifier);
                }
                free(pargs[j+1]);
                pargs[j+1] = p2;
                if (report) {
                    prte_asprintf(&help_str, "%s %s", option, p2);
                    /* can't just call show_help as we want every instance to be reported */
                    output = prte_show_help_string("help-schizo-base.txt", "deprecated-converted", true,
                                                    pargs[idx], help_str);
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
        free(pargs[idx+1]);
        /* replace it with the directive */
        pargs[idx+1] = strdup(directive);
        *argv = pargs;
        return PRTE_ERR_SILENT;
    } else {
        /* add the option */
        old_arg = strdup(pargs[idx]);
        free(pargs[idx]);
        pargs[idx] = strdup(option);
        /* delete arguments if needed */
        if (1 < ntodelete) {
            prte_argv_delete(NULL, argv, idx+1, ntodelete-1);
        }
        if (0 == strcasecmp(option, "--tune")) {
            p2 = NULL;
            prte_asprintf(&help_str, "%s %s", pargs[idx], pargs[idx+1]);
        } else if (NULL == directive) {
            prte_asprintf(&p2, ":%s", modifier);
        } else if (NULL == modifier) {
            p2 = strdup(directive);
        } else {
            prte_asprintf(&p2, "%s:%s", directive, modifier);
        }
        if (NULL != p2) {
            prte_argv_insert_element(&pargs, idx+1, p2);
            prte_asprintf(&help_str, "%s %s", pargs[idx], p2);
        } else {
            help_str = strdup(pargs[idx]);
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
    }
    *argv = pargs;  // will have been reallocated

    return PRTE_SUCCESS;
}

PRTE_CLASS_INSTANCE(prte_schizo_base_active_module_t,
                   prte_list_item_t,
                   NULL, NULL);
