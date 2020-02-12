/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2015-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include "src/include/version.h"
#include "src/mca/installdirs/installdirs.h"
#include "src/class/prrte_value_array.h"
#include "src/class/prrte_pointer_array.h"
#include "src/util/printf.h"
#include "src/include/prrte_portable_platform.h"

#include "src/util/show_help.h"


#include "src/tools/prte_info/pinfo.h"


/*
 * Public variables
 */

const char *prrte_info_component_all = "all";
const char *prrte_info_param_all = "all";

const char *prrte_info_path_prefix = "prefix";
const char *prrte_info_path_bindir = "bindir";
const char *prrte_info_path_libdir = "libdir";
const char *prrte_info_path_incdir = "incdir";
const char *prrte_info_path_mandir = "mandir";
const char *prrte_info_path_pkglibdir = "pkglibdir";
const char *prrte_info_path_sysconfdir = "sysconfdir";
const char *prrte_info_path_exec_prefix = "exec_prefix";
const char *prrte_info_path_sbindir = "sbindir";
const char *prrte_info_path_libexecdir = "libexecdir";
const char *prrte_info_path_datarootdir = "datarootdir";
const char *prrte_info_path_datadir = "datadir";
const char *prrte_info_path_sharedstatedir = "sharedstatedir";
const char *prrte_info_path_localstatedir = "localstatedir";
const char *prrte_info_path_infodir = "infodir";
const char *prrte_info_path_pkgdatadir = "pkgdatadir";
const char *prrte_info_path_pkgincludedir = "pkgincludedir";

void prrte_info_do_params(bool want_all_in, bool want_internal)
{
    int count;
    char *type, *component, *str;
    bool found;
    int i;
    bool want_all = false;
    prrte_value_t *pval;

    prrte_info_components_open();

    if (want_all_in) {
        want_all = true;
    } else {
        /* See if the special param "all" was givin to --param; that
         * superceeds any individual type
         */
        count = prrte_cmd_line_get_ninsts(prrte_info_cmd_line, "param");
        for (i = 0; i < count; ++i) {
            pval = prrte_cmd_line_get_param(prrte_info_cmd_line, "param", (int)i, 0);
            if (0 == strcmp(prrte_info_type_all, pval->data.string)) {
                want_all = true;
                break;
            }
        }
    }

    /* Show the params */
    if (want_all) {
        for (i = 0; i < mca_types.size; ++i) {
            if (NULL == (type = (char *)prrte_pointer_array_get_item(&mca_types, i))) {
                continue;
            }
            prrte_info_show_mca_params(type, prrte_info_component_all, want_internal);
        }
    } else {
        for (i = 0; i < count; ++i) {
            pval = prrte_cmd_line_get_param(prrte_info_cmd_line, "param", (int)i, 0);
            type = pval->data.string;
            pval = prrte_cmd_line_get_param(prrte_info_cmd_line, "param", (int)i, 1);
            component = pval->data.string;

            for (found = false, i = 0; i < mca_types.size; ++i) {
                if (NULL == (str = (char *)prrte_pointer_array_get_item(&mca_types, i))) {
                    continue;
                }
                if (0 == strcmp(str, type)) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                char *usage = prrte_cmd_line_get_usage_msg(prrte_info_cmd_line, false);
                prrte_show_help("help-pinfo.txt", "not-found", true, type);
                free(usage);
                exit(1);
            }

            prrte_info_show_mca_params(type, component, want_internal);
        }
    }
}

static void prrte_info_show_mca_group_params(const prrte_mca_base_var_group_t *group, bool want_internal)
{
    const prrte_mca_base_var_t *var;
    const int *variables;
    int ret, i, j, count;
    const int *groups;
    char **strings;

    variables = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_vars, const int);
    count = prrte_value_array_get_size((prrte_value_array_t *)&group->group_vars);

    for (i = 0 ; i < count ; ++i) {
        ret = prrte_mca_base_var_get(variables[i], &var);
        if (PRRTE_SUCCESS != ret || ((var->mbv_flags & PRRTE_MCA_BASE_VAR_FLAG_INTERNAL) &&
                                    !want_internal)) {
            continue;
        }

        ret = prrte_mca_base_var_dump(variables[i], &strings, !prrte_info_pretty ? PRRTE_MCA_BASE_VAR_DUMP_PARSABLE : PRRTE_MCA_BASE_VAR_DUMP_READABLE);
        if (PRRTE_SUCCESS != ret) {
            continue;
        }

        for (j = 0 ; strings[j] ; ++j) {
            if (0 == j && prrte_info_pretty) {
                char *message;

                prrte_asprintf (&message, "MCA %s", group->group_framework);
                prrte_info_out(message, message, strings[j]);
                free(message);
            } else {
                prrte_info_out("", "", strings[j]);
            }
            free(strings[j]);
        }
        free(strings);
    }

    groups = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_subgroups, const int);
    count = prrte_value_array_get_size((prrte_value_array_t *)&group->group_subgroups);

    for (i = 0 ; i < count ; ++i) {
        ret = prrte_mca_base_var_group_get(groups[i], &group);
        if (PRRTE_SUCCESS != ret) {
            continue;
        }
        prrte_info_show_mca_group_params(group, want_internal);
    }
}

void prrte_info_show_mca_params(const char *type, const char *component,
                               bool want_internal)
{
    const prrte_mca_base_var_group_t *group;
    int ret;

    if (0 == strcmp (component, "all")) {
        ret = prrte_mca_base_var_group_find("*", type, NULL);
        if (0 > ret) {
            return;
        }

        (void) prrte_mca_base_var_group_get(ret, &group);

        prrte_info_show_mca_group_params(group, want_internal);
    } else {
        ret = prrte_mca_base_var_group_find("*", type, component);
        if (0 > ret) {
            return;
        }

        (void) prrte_mca_base_var_group_get(ret, &group);
        prrte_info_show_mca_group_params(group, want_internal);
    }
}

void prrte_info_do_path(bool want_all, prrte_cmd_line_t *cmd_line)
{
    int i, count;
    char *scope;
    prrte_value_t *pval;

    /* Check bozo case */
    count = prrte_cmd_line_get_ninsts(cmd_line, "path");
    for (i = 0; i < count; ++i) {
        pval = prrte_cmd_line_get_param(cmd_line, "path", i, 0);
        scope = pval->data.string;
        if (0 == strcmp("all", scope)) {
            want_all = true;
            break;
        }
    }

    if (want_all) {
        prrte_info_show_path(prrte_info_path_prefix, prrte_install_dirs.prefix);
        prrte_info_show_path(prrte_info_path_exec_prefix, prrte_install_dirs.exec_prefix);
        prrte_info_show_path(prrte_info_path_bindir, prrte_install_dirs.bindir);
        prrte_info_show_path(prrte_info_path_sbindir, prrte_install_dirs.sbindir);
        prrte_info_show_path(prrte_info_path_libdir, prrte_install_dirs.libdir);
        prrte_info_show_path(prrte_info_path_incdir, prrte_install_dirs.includedir);
        prrte_info_show_path(prrte_info_path_mandir, prrte_install_dirs.mandir);
        prrte_info_show_path(prrte_info_path_pkglibdir, prrte_install_dirs.prrtelibdir);
        prrte_info_show_path(prrte_info_path_libexecdir, prrte_install_dirs.libexecdir);
        prrte_info_show_path(prrte_info_path_datarootdir, prrte_install_dirs.datarootdir);
        prrte_info_show_path(prrte_info_path_datadir, prrte_install_dirs.datadir);
        prrte_info_show_path(prrte_info_path_sysconfdir, prrte_install_dirs.sysconfdir);
        prrte_info_show_path(prrte_info_path_sharedstatedir, prrte_install_dirs.sharedstatedir);
        prrte_info_show_path(prrte_info_path_localstatedir, prrte_install_dirs.localstatedir);
        prrte_info_show_path(prrte_info_path_infodir, prrte_install_dirs.infodir);
        prrte_info_show_path(prrte_info_path_pkgdatadir, prrte_install_dirs.prrtedatadir);
        prrte_info_show_path(prrte_info_path_pkglibdir, prrte_install_dirs.prrtelibdir);
        prrte_info_show_path(prrte_info_path_pkgincludedir, prrte_install_dirs.prrteincludedir);
    } else {
        count = prrte_cmd_line_get_ninsts(cmd_line, "path");
        for (i = 0; i < count; ++i) {
            pval = prrte_cmd_line_get_param(cmd_line, "path", i, 0);
            scope = pval->data.string;

            if (0 == strcmp(prrte_info_path_prefix, scope)) {
                prrte_info_show_path(prrte_info_path_prefix, prrte_install_dirs.prefix);
            } else if (0 == strcmp(prrte_info_path_bindir, scope)) {
                prrte_info_show_path(prrte_info_path_bindir, prrte_install_dirs.bindir);
            } else if (0 == strcmp(prrte_info_path_libdir, scope)) {
                prrte_info_show_path(prrte_info_path_libdir, prrte_install_dirs.libdir);
            } else if (0 == strcmp(prrte_info_path_incdir, scope)) {
                prrte_info_show_path(prrte_info_path_incdir, prrte_install_dirs.includedir);
            } else if (0 == strcmp(prrte_info_path_mandir, scope)) {
                prrte_info_show_path(prrte_info_path_mandir, prrte_install_dirs.mandir);
            } else if (0 == strcmp(prrte_info_path_pkglibdir, scope)) {
                prrte_info_show_path(prrte_info_path_pkglibdir, prrte_install_dirs.prrtelibdir);
            } else if (0 == strcmp(prrte_info_path_sysconfdir, scope)) {
                prrte_info_show_path(prrte_info_path_sysconfdir, prrte_install_dirs.sysconfdir);
            } else if (0 == strcmp(prrte_info_path_exec_prefix, scope)) {
                prrte_info_show_path(prrte_info_path_exec_prefix, prrte_install_dirs.exec_prefix);
            } else if (0 == strcmp(prrte_info_path_sbindir, scope)) {
                prrte_info_show_path(prrte_info_path_sbindir, prrte_install_dirs.sbindir);
            } else if (0 == strcmp(prrte_info_path_libexecdir, scope)) {
                prrte_info_show_path(prrte_info_path_libexecdir, prrte_install_dirs.libexecdir);
            } else if (0 == strcmp(prrte_info_path_datarootdir, scope)) {
                prrte_info_show_path(prrte_info_path_datarootdir, prrte_install_dirs.datarootdir);
            } else if (0 == strcmp(prrte_info_path_datadir, scope)) {
                prrte_info_show_path(prrte_info_path_datadir, prrte_install_dirs.datadir);
            } else if (0 == strcmp(prrte_info_path_sharedstatedir, scope)) {
                prrte_info_show_path(prrte_info_path_sharedstatedir, prrte_install_dirs.sharedstatedir);
            } else if (0 == strcmp(prrte_info_path_localstatedir, scope)) {
                prrte_info_show_path(prrte_info_path_localstatedir, prrte_install_dirs.localstatedir);
            } else if (0 == strcmp(prrte_info_path_infodir, scope)) {
                prrte_info_show_path(prrte_info_path_infodir, prrte_install_dirs.infodir);
            } else if (0 == strcmp(prrte_info_path_pkgdatadir, scope)) {
                prrte_info_show_path(prrte_info_path_pkgdatadir, prrte_install_dirs.prrtedatadir);
            } else if (0 == strcmp(prrte_info_path_pkgincludedir, scope)) {
                prrte_info_show_path(prrte_info_path_pkgincludedir, prrte_install_dirs.prrteincludedir);
            } else {
                char *usage = prrte_cmd_line_get_usage_msg(cmd_line, false);
                prrte_show_help("help-pinfo.txt", "usage", true, usage);
                free(usage);
                exit(1);
            }
        }
    }
}


void prrte_info_show_path(const char *type, const char *value)
{
    char *pretty, *path;

    pretty = strdup(type);
    pretty[0] = toupper(pretty[0]);

    prrte_asprintf(&path, "path:%s", type);
    prrte_info_out(pretty, path, value);
    free(pretty);
    free(path);
}


void prrte_info_do_arch()
{
    prrte_info_out("Configured architecture", "config:arch", PRRTE_ARCH);
}


void prrte_info_do_hostname()
{
    prrte_info_out("Configure host", "config:host", PRRTE_CONFIGURE_HOST);
}


/*
 * do_config
 * Accepts:
 *      - want_all: boolean flag; TRUE -> display all options
 *                                FALSE -> display selected options
 *
 * This function displays all the options with which the current
 * installation of prrte was configured. There are many options here
 * that are carried forward from PRRTE-7 and are not mca parameters
 * in PRRTE-10. I have to dig through the invalid options and replace
 * them with PRRTE-10 options.
 */
void prrte_info_do_config(bool want_all)
{
    char *heterogeneous;
    char *debug;
    char *have_dl;
    char *prun_prefix_by_default;
    char *symbol_visibility;

    /* setup the strings that don't require allocations*/
    heterogeneous = PRRTE_ENABLE_HETEROGENEOUS_SUPPORT ? "yes" : "no";
    debug = PRRTE_ENABLE_DEBUG ? "yes" : "no";
    have_dl = PRRTE_HAVE_DL_SUPPORT ? "yes" : "no";
    prun_prefix_by_default = PRRTE_WANT_PRRTE_PREFIX_BY_DEFAULT ? "yes" : "no";
    symbol_visibility = PRRTE_C_HAVE_VISIBILITY ? "yes" : "no";

    /* output values */
    prrte_info_out("Configured by", "config:user", PRRTE_CONFIGURE_USER);
    prrte_info_out("Configured on", "config:timestamp", PRRTE_CONFIGURE_DATE);
    prrte_info_out("Configure host", "config:host", PRRTE_CONFIGURE_HOST);
    prrte_info_out("Configure command line", "config:cli", PRRTE_CONFIGURE_CLI);

    prrte_info_out("Built by", "build:user", PRRTE_BUILD_USER);
    prrte_info_out("Built on", "build:timestamp", PRRTE_BUILD_DATE);
    prrte_info_out("Built host", "build:host", PRRTE_BUILD_HOST);

    prrte_info_out("C compiler", "compiler:c:command", PRRTE_CC);
    prrte_info_out("C compiler absolute", "compiler:c:absolute", PRRTE_CC_ABSOLUTE);
    prrte_info_out("C compiler family name", "compiler:c:familyname", _STRINGIFY(PLATFORM_COMPILER_FAMILYNAME));
    prrte_info_out("C compiler version", "compiler:c:version", _STRINGIFY(PLATFORM_COMPILER_VERSION_STR));

    if (want_all) {
        prrte_info_out_int("C char size", "compiler:c:sizeof:char", sizeof(char));
        /* JMS: should be fixed in MPI-2.2 to differentiate between C
         _Bool and C++ bool.  For the moment, the code base assumes
         that they are the same.  Because of prrte_config_bottom.h,
         we can sizeof(bool) here, so we might as well -- even
         though this technically isn't right.  This should be fixed
         when we update to MPI-2.2.  See below for note about C++
         bool alignment. */
        prrte_info_out_int("C bool size", "compiler:c:sizeof:bool", sizeof(bool));
        prrte_info_out_int("C short size", "compiler:c:sizeof:short", sizeof(short));
        prrte_info_out_int("C int size", "compiler:c:sizeof:int", sizeof(int));
        prrte_info_out_int("C long size", "compiler:c:sizeof:long", sizeof(long));
        prrte_info_out_int("C float size", "compiler:c:sizeof:float", sizeof(float));
        prrte_info_out_int("C double size", "compiler:c:sizeof:double", sizeof(double));
        prrte_info_out_int("C pointer size", "compiler:c:sizeof:pointer", sizeof(void *));
        prrte_info_out_int("C char align", "compiler:c:align:char", PRRTE_ALIGNMENT_CHAR);
        prrte_info_out("C bool align", "compiler:c:align:bool", "skipped");
        prrte_info_out_int("C int align", "compiler:c:align:int", PRRTE_ALIGNMENT_INT);
        prrte_info_out_int("C float align", "compiler:c:align:float", PRRTE_ALIGNMENT_FLOAT);
        prrte_info_out_int("C double align", "compiler:c:align:double", PRRTE_ALIGNMENT_DOUBLE);
    }

    prrte_info_out("Thread support", "option:threads", "posix");

    if (want_all) {


        prrte_info_out("Build CFLAGS", "option:build:cflags", PRRTE_BUILD_CFLAGS);
        prrte_info_out("Build LDFLAGS", "option:build:ldflags", PRRTE_BUILD_LDFLAGS);
        prrte_info_out("Build LIBS", "option:build:libs", PRRTE_BUILD_LIBS);

        prrte_info_out("Wrapper extra CFLAGS", "option:wrapper:extra_cflags",
                      PRRTE_WRAPPER_EXTRA_CFLAGS);
        prrte_info_out("Wrapper extra LDFLAGS", "option:wrapper:extra_ldflags",
                      PRRTE_WRAPPER_EXTRA_LDFLAGS);
        prrte_info_out("Wrapper extra LIBS", "option:wrapper:extra_libs",
                      PRRTE_WRAPPER_EXTRA_LIBS);
    }

    prrte_info_out("Internal debug support", "option:debug", debug);
    prrte_info_out("dl support", "option:dlopen", have_dl);
    prrte_info_out("Heterogeneous support", "options:heterogeneous", heterogeneous);
    prrte_info_out("prun default --prefix", "prun:prefix_by_default",
                  prun_prefix_by_default);
    prrte_info_out("Symbol vis. support", "options:visibility", symbol_visibility);

}
