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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/class/prte_object.h"
#include "src/class/prte_pointer_array.h"
#include "src/mca/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/schizo/base/base.h"
#include "src/prted/pmix/pmix_server.h"
#include "src/util/argv.h"
#include "src/util/cmd_line.h"
#include "src/util/error.h"
#include "src/util/path.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "constants.h"
#include "src/include/frameworks.h"
#include "src/include/version.h"
#include "src/runtime/prte_locks.h"

#include "src/tools/prte_info/pinfo.h"

/*
 * Public variables
 */

bool prte_info_pretty = true;
prte_cmd_line_t *prte_info_cmd_line = NULL;

const char *prte_info_type_all = "all";
const char *prte_info_type_prte = "prte";
const char *prte_info_type_base = "base";

prte_pointer_array_t mca_types = {{0}};

prte_cmd_line_init_t info_cmd_line_opts[]
    = {{'h', "help", 0, PRTE_CMD_LINE_TYPE_BOOL, "This help message", PRTE_CMD_LINE_OTYPE_GENERAL},
       {'V', "version", 0, PRTE_CMD_LINE_TYPE_BOOL, "Print version and exit",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'\0', "show-version", 2, PRTE_CMD_LINE_TYPE_STRING,
        "Show version of PRTE or a component.  The first parameter can be the keywords \"prte\" or "
        "\"all\", a framework name (indicating all components in a framework), or a "
        "framework:component string (indicating a specific component).  The second parameter can "
        "be one of: full, major, minor, release, greek, svn.",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'\0', "param", 2, PRTE_CMD_LINE_TYPE_STRING,
        "Show MCA parameters.  The first parameter is the framework (or the keyword \"all\"); the "
        "second parameter is the specific component name (or the keyword \"all\").",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'\0', "internal", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Show internal MCA parameters (not meant to be modified by users)",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'\0', "path", 1, PRTE_CMD_LINE_TYPE_STRING,
        "Show paths that PRTE was configured with.  Accepts the following parameters: prefix, "
        "bindir, libdir, incdir, mandir, pkglibdir, sysconfdir",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'\0', "arch", 0, PRTE_CMD_LINE_TYPE_BOOL, "Show architecture PRTE was compiled on",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'c', "config", 0, PRTE_CMD_LINE_TYPE_BOOL, "Show configuration options",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'\0', "hostname", 0, PRTE_CMD_LINE_TYPE_BOOL,
        "Show the hostname that PRTE was configured "
        "and built on",
        PRTE_CMD_LINE_OTYPE_GENERAL},
       {'a', "all", 0, PRTE_CMD_LINE_TYPE_BOOL, "Show all configuration options and MCA parameters",
        PRTE_CMD_LINE_OTYPE_GENERAL},

       /* End of list */
       {'\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL}};

int main(int argc, char *argv[])
{
    int ret = 0;
    bool want_help = false;
    bool cmd_error = false;
    bool acted = false;
    bool want_all = false;
    int i;
    char *str;

    /* protect against problems if someone passes us thru a pipe
     * and then abnormally terminates the pipe early */
    signal(SIGPIPE, SIG_IGN);

    /* Initialize the argv parsing stuff */
    if (PRTE_SUCCESS != (ret = prte_init_util(PRTE_PROC_MASTER))) {
        prte_show_help("help-prte-info.txt", "lib-call-fail", true, "prte_init_util", __FILE__,
                       __LINE__, NULL);
        exit(ret);
    }

    prte_info_cmd_line = PRTE_NEW(prte_cmd_line_t);
    if (NULL == prte_info_cmd_line) {
        ret = errno;
        prte_show_help("help-prte-info.txt", "lib-call-fail", true, "prte_cmd_line_create",
                       __FILE__, __LINE__, NULL);
        exit(ret);
    }

    ret = prte_cmd_line_add(prte_info_cmd_line, info_cmd_line_opts);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /* Do the parsing */
    ret = prte_cmd_line_parse(prte_info_cmd_line, false, false, argc, argv);
    if (PRTE_SUCCESS != ret) {
        if (PRTE_ERR_SILENT != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], prte_strerror(ret));
        }
        cmd_error = true;
    }
    if (!cmd_error
        && (prte_cmd_line_is_taken(prte_info_cmd_line, "help")
            || prte_cmd_line_is_taken(prte_info_cmd_line, "h"))) {
        char *usage;

        want_help = true;
        usage = prte_cmd_line_get_usage_msg(prte_info_cmd_line, false);
        str = prte_show_help_string("help-prte-info.txt", "usage", true, usage);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(usage);
    }
    if (cmd_error || want_help) {
        prte_mca_base_close();
        PRTE_RELEASE(prte_info_cmd_line);
        exit(cmd_error ? 1 : 0);
    }

    if (prte_cmd_line_is_taken(prte_info_cmd_line, "version")) {
        fprintf(stdout, "PRTE v%s\n\n%s\n", PRTE_VERSION, PACKAGE_BUGREPORT);
        exit(0);
    }

    /* setup the mca_types array */
    PRTE_CONSTRUCT(&mca_types, prte_pointer_array_t);
    prte_pointer_array_init(&mca_types, 256, INT_MAX, 128);

    /* add a type for prte itself */
    prte_pointer_array_add(&mca_types, "prte");

    /* add a type for hwloc */
    prte_pointer_array_add(&mca_types, "hwloc");

    /* let the pmix server register params */
    pmix_server_register_params();
    /* add those in */
    prte_pointer_array_add(&mca_types, "pmix");

    /* push all the types found by autogen */
    for (i = 0; NULL != prte_frameworks[i]; i++) {
        prte_pointer_array_add(&mca_types, prte_frameworks[i]->framework_name);
    }

    /* Execute the desired action(s) */

    if (prte_cmd_line_is_taken(prte_info_cmd_line, "prte_info_pretty")) {
        prte_info_pretty = true;
    } else if (prte_cmd_line_is_taken(prte_info_cmd_line, "parsable")
               || prte_cmd_line_is_taken(prte_info_cmd_line, "parseable")) {
        prte_info_pretty = false;
    }

    want_all = prte_cmd_line_is_taken(prte_info_cmd_line, "all");
    if (want_all || prte_cmd_line_is_taken(prte_info_cmd_line, "show-version")) {
        prte_info_do_version(want_all, prte_info_cmd_line);
        acted = true;
    }
    if (want_all || prte_cmd_line_is_taken(prte_info_cmd_line, "path")) {
        prte_info_do_path(want_all, prte_info_cmd_line);
        acted = true;
    }
    if (want_all || prte_cmd_line_is_taken(prte_info_cmd_line, "arch")) {
        prte_info_do_arch();
        acted = true;
    }
    if (want_all || prte_cmd_line_is_taken(prte_info_cmd_line, "hostname")) {
        prte_info_do_hostname();
        acted = true;
    }
    if (want_all || prte_cmd_line_is_taken(prte_info_cmd_line, "config")) {
        prte_info_do_config(true);
        acted = true;
    }
    if (want_all || prte_cmd_line_is_taken(prte_info_cmd_line, "param")) {
        prte_info_do_params(want_all, prte_cmd_line_is_taken(prte_info_cmd_line, "internal"));
        acted = true;
    }

    /* If no command line args are specified, show default set */

    if (!acted) {
        prte_info_show_prte_version(prte_info_ver_full);
        prte_info_show_path(prte_info_path_prefix, prte_install_dirs.prefix);
        prte_info_do_arch();
        prte_info_do_hostname();
        prte_info_do_config(false);
        prte_info_components_open();
        for (i = 0; i < mca_types.size; ++i) {
            if (NULL == (str = (char *) prte_pointer_array_get_item(&mca_types, i))) {
                continue;
            }
            prte_info_show_component_version(str, prte_info_component_all, prte_info_ver_full,
                                             prte_info_type_all);
        }
    }

    /* All done */
    prte_info_components_close();
    PRTE_DESTRUCT(&mca_types);
    prte_mca_base_close();

    return 0;
}
