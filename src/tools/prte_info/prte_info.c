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
 * Copyright (c) 2007-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2016 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <errno.h>
#include <signal.h>

#include "src/mca/installdirs/installdirs.h"
#include "src/class/prrte_object.h"
#include "src/class/prrte_pointer_array.h"
#include "src/util/cmd_line.h"
#include "src/util/error.h"
#include "src/util/argv.h"
#include "src/mca/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/util/show_help.h"

#include "constants.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_locks.h"
#include "src/include/frameworks.h"

#include "src/tools/prte_info/pinfo.h"

/*
 * Public variables
 */

bool prrte_info_pretty = true;
prrte_cmd_line_t *prrte_info_cmd_line = NULL;

const char *prrte_info_type_all = "all";
const char *prrte_info_type_prrte = "prrte";
const char *prrte_info_type_base = "base";

prrte_pointer_array_t mca_types = {{0}};

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

    prrte_info_cmd_line = PRRTE_NEW(prrte_cmd_line_t);
    if (NULL == prrte_info_cmd_line) {
        ret = errno;
        prrte_show_help("help-pinfo.txt", "lib-call-fail", true,
                       "prrte_cmd_line_create", __FILE__, __LINE__, NULL);
        exit(ret);
    }

    prrte_cmd_line_make_opt3(prrte_info_cmd_line, '\0', "show-version", 2,
                            "Show version of PRRTE or a component.  The first parameter can be the keywords \"prrte\" or \"all\", a framework name (indicating all components in a framework), or a framework:component string (indicating a specific component).  The second parameter can be one of: full, major, minor, release, greek, svn.",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, '\0', "param", 2,
                            "Show MCA parameters.  The first parameter is the framework (or the keyword \"all\"); the second parameter is the specific component name (or the keyword \"all\").",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, '\0', "internal", 0,
                            "Show internal MCA parameters (not meant to be modified by users)",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, '\0', "path", 1,
                            "Show paths that PRRTE was configured with.  Accepts the following parameters: prefix, bindir, libdir, incdir, mandir, pkglibdir, sysconfdir",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, '\0', "arch", 0,
                            "Show architecture PRRTE was compiled on",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, 'c', "config", 0,
                            "Show configuration options",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, '\0', "hostname", 0,
                            "Show the hostname that PRRTE was configured "
                            "and built on",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);
    prrte_cmd_line_make_opt3(prrte_info_cmd_line, 'a', "all", 0,
                            "Show all configuration options and MCA parameters",
                            PRRTE_CMD_LINE_OTYPE_GENERAL);

    /* Call some useless functions in order to guarantee to link in some
     * global variables.  Only check the return value so that the
     * cprrteler doesn't optimize out the useless function.
     */

    if (PRRTE_SUCCESS != prrte_locks_init()) {
        /* Stop .. or I'll say stop again! */
        ++ret;
    } else {
        --ret;
    }

    /* initialize install dirs code */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_installdirs_base_framework, 0))) {
        fprintf(stderr, "prrte_installdirs_base_open() failed -- process will likely abort (%s:%d, returned %d instead of PRRTE_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }

    /* Get MCA parameters, if any */

    if( PRRTE_SUCCESS != prrte_mca_base_open() ) {
        prrte_show_help("help-pinfo.txt", "lib-call-fail", true, "mca_base_open", __FILE__, __LINE__ );
        PRRTE_RELEASE(prrte_info_cmd_line);
        exit(1);
    }
    /* open the SCHIZO framework */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_schizo_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    if (PRRTE_SUCCESS != (ret = prrte_schizo_base_select())) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }
    /* scan for personalities */
    for (i=0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            prrte_argv_append_nosize(&prrte_schizo_base.personalities, argv[i+1]);
        }
    }

    /* setup the rest of the cmd line only once */
    if (PRRTE_SUCCESS != (ret = prrte_schizo.define_cli(prrte_info_cmd_line))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    /* Do the parsing */

    ret = prrte_cmd_line_parse(prrte_info_cmd_line, false, false, argc, argv);
    if (PRRTE_SUCCESS != ret) {
        if (PRRTE_ERR_SILENT != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(ret));
        }
        cmd_error = true;
    }
    if (!cmd_error &&
        (prrte_cmd_line_is_taken(prrte_info_cmd_line, "help") ||
         prrte_cmd_line_is_taken(prrte_info_cmd_line, "h"))) {
        char *str, *usage;

        want_help = true;
        usage = prrte_cmd_line_get_usage_msg(prrte_info_cmd_line, false);
        str = prrte_show_help_string("help-pinfo.txt", "usage", true,
                                    usage);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(usage);
    }
    if (cmd_error || want_help) {
        prrte_mca_base_close();
        PRRTE_RELEASE(prrte_info_cmd_line);
        exit(cmd_error ? 1 : 0);
    }

    /* setup the mca_types array */
    PRRTE_CONSTRUCT(&mca_types, prrte_pointer_array_t);
    prrte_pointer_array_init(&mca_types, 256, INT_MAX, 128);

    /* push all the types found by autogen */
    for (i=0; NULL != prrte_frameworks[i]; i++) {
        prrte_pointer_array_add(&mca_types, prrte_frameworks[i]->framework_name);
    }

    /* Execute the desired action(s) */

    if (prrte_cmd_line_is_taken(prrte_info_cmd_line, "prrte_info_pretty")) {
        prrte_info_pretty = true;
    } else if (prrte_cmd_line_is_taken(prrte_info_cmd_line, "parsable") || prrte_cmd_line_is_taken(prrte_info_cmd_line, "parseable")) {
        prrte_info_pretty = false;
    }

    want_all = prrte_cmd_line_is_taken(prrte_info_cmd_line, "all");
    if (want_all || prrte_cmd_line_is_taken(prrte_info_cmd_line, "show-version")) {
        prrte_info_do_version(want_all, prrte_info_cmd_line);
        acted = true;
    }
    if (want_all || prrte_cmd_line_is_taken(prrte_info_cmd_line, "path")) {
        prrte_info_do_path(want_all, prrte_info_cmd_line);
        acted = true;
    }
    if (want_all || prrte_cmd_line_is_taken(prrte_info_cmd_line, "arch")) {
        prrte_info_do_arch();
        acted = true;
    }
    if (want_all || prrte_cmd_line_is_taken(prrte_info_cmd_line, "hostname")) {
        prrte_info_do_hostname();
        acted = true;
    }
    if (want_all || prrte_cmd_line_is_taken(prrte_info_cmd_line, "config")) {
        prrte_info_do_config(true);
        acted = true;
    }
    if (want_all || prrte_cmd_line_is_taken(prrte_info_cmd_line, "param")) {
        prrte_info_do_params(want_all, prrte_cmd_line_is_taken(prrte_info_cmd_line, "internal"));
        acted = true;
    }

    /* If no command line args are specified, show default set */

    if (!acted) {
        prrte_info_show_prrte_version(prrte_info_ver_full);
        prrte_info_show_path(prrte_info_path_prefix, prrte_install_dirs.prefix);
        prrte_info_do_arch();
        prrte_info_do_hostname();
        prrte_info_do_config(false);
        prrte_info_components_open();
        for (i = 0; i < mca_types.size; ++i) {
            if (NULL == (str = (char*)prrte_pointer_array_get_item(&mca_types, i))) {
                continue;
            }
            prrte_info_show_component_version(str, prrte_info_component_all,
                                             prrte_info_ver_full, prrte_info_type_all);
        }
    }

    /* All done */
    prrte_info_components_close();
    PRRTE_DESTRUCT(&mca_types);
    prrte_mca_base_close();

    return 0;
}
