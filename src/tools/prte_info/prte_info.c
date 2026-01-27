/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010-2016 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <errno.h>
#include <signal.h>

#include "src/class/pmix_object.h"
#include "src/class/pmix_pointer_array.h"
#include "src/include/prte_portable_platform_real.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/schizo/base/base.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/runtime/pmix_info_support.h"
#include "src/runtime/pmix_rte.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/error.h"
#include "src/util/pmix_path.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "constants.h"
#include "src/include/prte_frameworks.h"
#include "src/include/version.h"
#include "src/runtime/prte_locks.h"

static int register_framework_params(pmix_pointer_array_t *component_map)
{
    int rc;

    /* Register mca/base parameters */
    if (PMIX_SUCCESS != pmix_mca_base_open(NULL)) {
        pmix_show_help("help-prte-info.txt", "lib-call-fail", true, "pmix_mca_base_open",
                       __FILE__, __LINE__);
        return PMIX_ERROR;
    }

    /* Register the PMIX layer's MCA parameters */
    if (PMIX_SUCCESS != (rc = prte_register_params())) {
        fprintf(stderr, "prte_register_params failed\n");
        return rc;
    }

    return pmix_info_register_project_frameworks("prte", prte_frameworks, component_map);
}

/*
 * Public variables
 */

const char *prte_info_type_all = "all";
const char *prte_info_type_prte = "prte";
const char *prte_info_type_base = "base";


int main(int argc, char *argv[])
{
    int ret = 0;
    bool acted = false;
    bool want_all = false;
    bool include_pmix = false;
    int i;
    char *color;
    char *str;
    char *ptr;
    prte_schizo_base_module_t *schizo;
    pmix_pointer_array_t prte_component_map;
    pmix_pointer_array_t mca_types;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;
    PRTE_HIDE_UNUSED_PARAMS(argc);

    /* protect against problems if someone passes us thru a pipe
     * and then abnormally terminates the pipe early */
    signal(SIGPIPE, SIG_IGN);

    prte_tool_basename = pmix_basename(argv[0]);
    prte_tool_actual = "prte_info";

    /* Initialize the argv parsing stuff */
    if (PRTE_SUCCESS != (ret = prte_init_util(PRTE_PROC_MASTER))) {
        pmix_show_help("help-prte-info.txt", "lib-call-fail", true, "prte_init_util", __FILE__,
                       __LINE__, NULL);
        exit(ret);
    }

    /* open the SCHIZO framework */
    ret = pmix_mca_base_framework_open(&prte_schizo_base_framework,
                                       PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    if (PRTE_SUCCESS != (ret = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    /* detect if we are running as a proxy and select the active
     * schizo module for this tool */
    schizo = prte_schizo_base_detect_proxy("prte");
    if (NULL == schizo) {
        pmix_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, "prte");
        return 1;
    }

    /* parse the input argv to get values, including everyone's MCA params */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    ret = schizo->parse_cli(argv, &results, PMIX_CLI_SILENT);
    if (PRTE_SUCCESS != ret) {
        PMIX_DESTRUCT(&results);
        if (PRTE_OPERATION_SUCCEEDED == ret) {
            return PRTE_SUCCESS;
        }
        if (PRTE_ERR_SILENT != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(ret));
        }
        return ret;
    }
    // we do NOT accept arguments other than our own
    if (NULL != results.tail) {
        str = PMIX_ARGV_JOIN_COMPAT(results.tail, ' ');
        if (0 != strcmp(str, argv[0])) {
            ptr = pmix_show_help_string("help-pterm.txt", "no-args", false,
                                        prte_tool_basename, str, prte_tool_basename);
            free(str);
            if (NULL != ptr) {
                printf("%s", ptr);
                free(ptr);
            }
            return -1;
        }
        free(str);
    }

    // see if they want PMIx info included
    include_pmix = pmix_cmd_line_is_taken(&results, "include-pmix");

    /* Determine color support */
    opt = pmix_cmd_line_get_param(&results, PMIX_CLI_INFO_COLOR);
    if (NULL != opt) {
        color = NULL;
        if (NULL != opt->values) {
            color = opt->values[0];
        }
        if (NULL == color) {
            color = "auto";
        }
    } else {
        color = "auto";
    }
    if (0 == strcasecmp(color, "auto")) {
        #if HAVE_ISATTY
            pmix_info_color = isatty(STDOUT_FILENO);
        #else
            pmix_info_color = false;
        #endif
    } else if (0 == strcasecmp(color, "always")) {
        pmix_info_color = true;
    } else if (0 == strcasecmp(color, "never")) {
        pmix_info_color = false;
    } else {
        fprintf(stderr, "%s: Unrecognized value '%s' to color parameter\n", argv[0], color);
        exit(2);
    }


    /* set the flags */
    if (pmix_cmd_line_is_taken(&results, PMIX_CLI_PRETTY_PRINT)) {
        pmix_info_pretty = true;
    } else if (pmix_cmd_line_is_taken(&results, PMIX_CLI_PARSABLE) ||
               pmix_cmd_line_is_taken(&results, PMIX_CLI_PARSEABLE)) {
        pmix_info_pretty = false;
    }

    if (pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_SELECTED_ONLY)) {
        /* register only selected components */
        pmix_info_register_flags = PMIX_MCA_BASE_REGISTER_DEFAULT;
    }

    if (pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_SHOW_FAILED)) {
        pmix_mca_base_component_track_load_errors = true;
    }

    /* setup the mca_types array */
    PMIX_CONSTRUCT(&mca_types, pmix_pointer_array_t);
    pmix_pointer_array_init(&mca_types, 256, INT_MAX, 128);

    /* add a type for prte itself */
    pmix_pointer_array_add(&mca_types, "mca");
    pmix_pointer_array_add(&mca_types, "prte");

    /* add a type for hwloc */
    pmix_pointer_array_add(&mca_types, "hwloc");

    /* let the pmix server register params */
    pmix_server_register_params();
    /* add those in */
    pmix_pointer_array_add(&mca_types, "pmix");

    /* add the rml and routed types since they are no
     * longer in a framework */
    pmix_pointer_array_add(&mca_types, "rml");
    pmix_pointer_array_add(&mca_types, "routed");

    /* push all the types found by autogen */
    for (i = 0; NULL != prte_frameworks[i]; i++) {
        pmix_pointer_array_add(&mca_types, prte_frameworks[i]->framework_name);
    }

    if (include_pmix) {
        // push all the PMIx framework types
        pmix_info_register_types(&mca_types, true);
    }

    /* setup the component_map array */
    PMIX_CONSTRUCT(&prte_component_map, pmix_pointer_array_t);
    pmix_pointer_array_init(&prte_component_map, 256, INT_MAX, 128);

    /* Register all global MCA Params */
    if (PRTE_SUCCESS != (ret = prte_register_params())) {
        if (PRTE_ERR_SILENT != ret) {
            pmix_show_help("help-prte-runtime", "prte_init:startup:internal-failure", true,
                           "prte register params",
                           PRTE_ERROR_NAME(ret), ret);
        }
        return 1;
    }

     /* Register framework/component params */
    if (PMIX_SUCCESS != (ret = register_framework_params(&prte_component_map))) {
        if (PMIX_ERR_BAD_PARAM == ret) {
            /* output what we got */
            pmix_info_do_params("PRRTE", true, pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_INTERNAL),
                                &mca_types, &prte_component_map, NULL);
        }
        exit(1);
    }

    if (include_pmix) {
        /* register params for pmix */
        if (PMIX_SUCCESS != (ret = pmix_register_params())) {
            fprintf(stderr, "pmix_register_params failed with %d\n", ret);
            return PMIX_ERROR;
        }


        /* Register PMIx's params */
        if (PMIX_SUCCESS != (ret = pmix_info_register_framework_params(&prte_component_map))) {
            if (PMIX_ERR_BAD_PARAM == ret) {
                /* output what we got */
                pmix_info_do_params("PMIx", true, pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_INTERNAL),
                                    &mca_types, &prte_component_map, NULL);
            }
            exit(1);
        }
    }

    /* Execute the desired action(s) */
    want_all = pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_ALL);

    opt = pmix_cmd_line_get_param(&results, PMIX_CLI_INFO_VERSION);
    if (want_all || NULL != opt) {
        if (NULL == opt) {
            pmix_info_show_package(PRTE_PACKAGE_STRING);
            pmix_info_show_version("PRRTE", pmix_info_ver_full, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                   PRTE_RELEASE_VERSION, PRTE_GREEK_VERSION, PRTE_REPO_REV,
                                   PRTE_RELEASE_DATE);
            pmix_info_show_component_version("PRRTE", &mca_types, &prte_component_map, pmix_info_type_all,
                                             pmix_info_component_all, pmix_info_ver_full,
                                             pmix_info_ver_all);

        } else if (NULL == opt->values) {
            pmix_info_show_package(PRTE_PACKAGE_STRING);
            pmix_info_show_version("PRRTE", pmix_info_ver_full, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION, PRTE_GREEK_VERSION, PRTE_REPO_REV,
                                    PRTE_RELEASE_DATE);
            pmix_info_show_component_version("PRRTE", &mca_types, &prte_component_map, pmix_info_type_all,
                                             pmix_info_component_all, pmix_info_ver_full,
                                             pmix_info_ver_all);

        } else {
            if (0 == strcasecmp(opt->values[0], "prte") ||
                0 == strcasecmp(opt->values[0], "all")) {
                pmix_info_show_package(PRTE_PACKAGE_STRING);
                pmix_info_show_version("PRRTE", pmix_info_ver_full, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                       PRTE_RELEASE_VERSION, PRTE_GREEK_VERSION, PRTE_REPO_REV,
                                       PRTE_RELEASE_DATE);
                pmix_info_show_component_version("PRRTE", &mca_types, &prte_component_map, pmix_info_type_all,
                                                 pmix_info_component_all, pmix_info_ver_full,
                                                 pmix_info_ver_all);

            } else {
                // the first arg is either the name of a framework, or a framework:component pair
                char **tmp = PMIx_Argv_split(opt->values[0], ':');
                const char *component = pmix_info_component_all;
                const char *modifier = pmix_info_ver_full;
                if (NULL != tmp[1]) {
                    component = tmp[1];
                }
                if (NULL != opt->values[1]) {
                    modifier = opt->values[1];
                }
                pmix_info_show_component_version("PRRTE", &mca_types, &prte_component_map, tmp[0],
                                                 component, modifier,
                                                 pmix_info_ver_all);
                PMIx_Argv_free(tmp);
            }
            acted = true;
        }
    }

    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PATH)) {
        pmix_info_do_path(want_all, &results, &prte_install_dirs);
        acted = true;
    }

    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_ARCH)) {
        pmix_info_do_arch();
        acted = true;
    }

    if (!want_all && pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_HOSTNAME)) {
        // hostname is contain in do_config, so don't duplicate it here
        pmix_info_do_hostname();
        acted = true;
    }

    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_CONFIG)) {
        pmix_info_do_config(true, PRTE_CONFIGURE_USER, PRTE_CONFIGURE_DATE, PRTE_CONFIGURE_HOST,
                            PRTE_CONFIGURE_CLI, PRTE_BUILD_USER, PRTE_BUILD_DATE, PRTE_BUILD_HOST,
                            PRTE_CC, PRTE_CC_ABSOLUTE, PLATFORM_STRINGIFY(PLATFORM_COMPILER_FAMILYNAME),
                            PLATFORM_STRINGIFY(PLATFORM_COMPILER_VERSION_STR), PRTE_BUILD_CFLAGS, PRTE_BUILD_LDFLAGS,
                            PRTE_BUILD_LIBS, PRTE_ENABLE_DEBUG, PRTE_HAVE_DL_SUPPORT,
                            PRTE_C_HAVE_VISIBILITY);
        acted = true;
    }

    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PARAM) ||
        pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PARAMS)) {
        pmix_info_do_params("PRRTE", true, pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_INTERNAL),
                            &mca_types, &prte_component_map, &results);
        acted = true;
    }

    if (pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_TYPES)) {
        pmix_info_do_type(&results);
        acted = true;
    }

    /* If no command line args are specified, show default set */

    if (!acted) {
        pmix_info_show_package(PRTE_PACKAGE_STRING);
        pmix_info_show_version("PRRTE", pmix_info_ver_full, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                               PRTE_RELEASE_VERSION, PRTE_GREEK_VERSION, PRTE_REPO_REV,
                               PRTE_RELEASE_DATE);
        pmix_info_show_path(pmix_info_path_prefix, prte_install_dirs.prefix);
        pmix_info_do_arch();
        pmix_info_do_config(false, PRTE_CONFIGURE_USER, PRTE_CONFIGURE_DATE, PRTE_CONFIGURE_HOST,
                            PRTE_CONFIGURE_CLI, PRTE_BUILD_USER, PRTE_BUILD_DATE, PRTE_BUILD_HOST,
                            PRTE_CC, PRTE_CC_ABSOLUTE, PLATFORM_STRINGIFY(PLATFORM_COMPILER_FAMILYNAME),
                            PLATFORM_STRINGIFY(PLATFORM_COMPILER_VERSION_STR), PRTE_BUILD_CFLAGS, PRTE_BUILD_LDFLAGS,
                            PRTE_BUILD_LIBS, PRTE_ENABLE_DEBUG, PRTE_HAVE_DL_SUPPORT,
                            PRTE_C_HAVE_VISIBILITY);
        pmix_info_show_component_version("PRRTE", &mca_types, &prte_component_map, pmix_info_type_all,
                                         pmix_info_component_all, pmix_info_ver_full,
                                         pmix_info_ver_all);
    }

    // if requested, do the PMIx info
    if (include_pmix) {
        acted = false;
        pmix_info_out(NULL, NULL, "\n\n\n==================");
        pmix_info_out(NULL, NULL, "\nPMIx CONFIGURATION\n");
        if (want_all || NULL != opt) {
            if (NULL == opt) {
                pmix_info_show_pmix_package();
                pmix_info_show_pmix_version();
                pmix_info_show_component_version("PMIx", &mca_types, &prte_component_map, pmix_info_type_all,
                                                 pmix_info_component_all, pmix_info_ver_full,
                                                 pmix_info_ver_all);

            } else if (NULL == opt->values) {
                pmix_info_show_pmix_package();
                pmix_info_show_pmix_version();
                pmix_info_show_component_version("PRRTE", &mca_types, &prte_component_map, pmix_info_type_all,
                                                 pmix_info_component_all, pmix_info_ver_full,
                                                 pmix_info_ver_all);
                if (include_pmix) {
                    pmix_info_show_component_version("PMIx", &mca_types, &prte_component_map, pmix_info_type_all,
                                                     pmix_info_component_all, pmix_info_ver_full,
                                                     pmix_info_ver_all);
                }

            } else {
                if (0 == strcasecmp(opt->values[0], "prte") ||
                    0 == strcasecmp(opt->values[0], "all")) {
                    pmix_info_show_pmix_package();
                    pmix_info_show_pmix_version();
                    pmix_info_show_component_version("PMIx", &mca_types, &prte_component_map, pmix_info_type_all,
                                                     pmix_info_component_all, pmix_info_ver_full,
                                                     pmix_info_ver_all);

                } else {
                    // the first arg is either the name of a framework, or a framework:component pair
                    char **tmp = PMIx_Argv_split(opt->values[0], ':');
                    const char *component = pmix_info_component_all;
                    const char *modifier = pmix_info_ver_full;
                    if (NULL != tmp[1]) {
                        component = tmp[1];
                    }
                    if (NULL != opt->values[1]) {
                        modifier = opt->values[1];
                    }
                    pmix_info_show_component_version("PMIx", &mca_types, &prte_component_map, tmp[0],
                                                     component, modifier,
                                                     pmix_info_ver_all);

                    PMIx_Argv_free(tmp);
                }
                acted = true;
            }
        }

        if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PATH)) {
            pmix_info_do_pmix_path(want_all, &results);
            acted = true;
        }

        if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_ARCH)) {
            pmix_info_do_arch();
            acted = true;
        }

        if (!want_all && pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_HOSTNAME)) {
            // hostname is contain in do_config, so don't duplicate it here
            pmix_info_do_hostname();
            acted = true;
        }

        if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_CONFIG)) {
            pmix_info_do_pmix_config(want_all);
            acted = true;
        }

        if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PARAM) ||
            pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PARAMS)) {
             pmix_info_do_params("PMIx", true, pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_INTERNAL),
                                &mca_types, &prte_component_map, &results);
            acted = true;
        }

        if (pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_TYPES)) {
            pmix_info_do_type(&results);
            acted = true;
        }

        /* If no command line args are specified, show default set */

    if (!acted) {
            pmix_info_show_pmix_package();
            pmix_info_show_pmix_version();
            pmix_info_show_path(pmix_info_path_prefix, prte_install_dirs.prefix);
            pmix_info_do_arch();
            pmix_info_do_pmix_config(want_all);
            pmix_info_show_component_version("PMIx", &mca_types, &prte_component_map, pmix_info_type_all,
                                             pmix_info_component_all, pmix_info_ver_full,
                                             pmix_info_ver_all);
        }
    }
    /* All done */
    PMIX_DESTRUCT(&mca_types);
    pmix_mca_base_close();

    return 0;
}
