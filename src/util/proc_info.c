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
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <ctype.h>

#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_var.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/attr.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/util/proc_info.h"

#include "src/util/proc_info.h"

/* provide a connection to a reqd variable */
extern bool prte_keep_fqdn_hostnames;

PRTE_EXPORT prte_process_info_t prte_process_info = {.myproc = {{0}, 0},
                                                     .my_hnp = {{0}, 0},
                                                     .my_hnp_uri = NULL,
                                                     .my_parent = {{0}, 0},
                                                     .hnp_pid = 0,
                                                     .num_daemons = 1,
                                                     .num_nodes = 1,
                                                     .nodename = NULL,
                                                     .aliases = NULL,
                                                     .pid = 0,
                                                     .proc_type = PRTE_PROC_TYPE_NONE,
                                                     .my_port = 0,
                                                     .num_restarts = 0,
                                                     .tmpdir_base = NULL,
                                                     .top_session_dir = NULL,
                                                     .jobfam_session_dir = NULL,
                                                     .job_session_dir = NULL,
                                                     .proc_session_dir = NULL,
                                                     .sock_stdin = NULL,
                                                     .sock_stdout = NULL,
                                                     .sock_stderr = NULL,
                                                     .cpuset = NULL};

static bool init = false;
static char *prte_strip_prefix;

void prte_setup_hostname(void)
{
    char *ptr;
    char hostname[PRTE_MAXHOSTNAMELEN];
    char **prefixes;
    bool match;
    int i, idx;

    /* whether or not to keep FQDN hostnames */
    prte_keep_fqdn_hostnames = false;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "keep_fqdn_hostnames",
                                      "Whether or not to keep FQDN hostnames [default: no]",
                                      PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_keep_fqdn_hostnames);

    /* get the nodename */
    gethostname(hostname, sizeof(hostname));
    /* add this to our list of aliases */
    prte_argv_append_nosize(&prte_process_info.aliases, hostname);

    // Strip off the FQDN if present, ignore IP addresses
    if (!prte_keep_fqdn_hostnames && !prte_net_isaddr(hostname)) {
        if (NULL != (ptr = strchr(hostname, '.'))) {
            *ptr = '\0';
            /* add this to our list of aliases */
            prte_argv_append_nosize(&prte_process_info.aliases, hostname);
        }
    }

    prte_strip_prefix = NULL;
    (void) prte_mca_base_var_register(
        "prte", "prte", NULL, "strip_prefix",
        "Prefix(es) to match when deciding whether to strip leading characters and zeroes from "
        "node names returned by daemons",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_strip_prefix);

    /* we have to strip node names here, if user directs, to ensure that
     * the names exchanged in the modex match the names found locally
     */
    if (NULL != prte_strip_prefix && !prte_net_isaddr(hostname)) {
        prefixes = prte_argv_split(prte_strip_prefix, ',');
        match = false;
        for (i = 0; NULL != prefixes[i]; i++) {
            if (0 == strncmp(hostname, prefixes[i], strlen(prefixes[i]))) {
                /* remove the prefix and leading zeroes */
                idx = strlen(prefixes[i]);
                while (idx < (int) strlen(hostname)
                       && (hostname[idx] <= '0' || '9' < hostname[idx])) {
                    idx++;
                }
                if ((int) strlen(hostname) <= idx) {
                    /* there were no non-zero numbers in the name */
                    prte_process_info.nodename = strdup(&hostname[strlen(prefixes[i])]);
                } else {
                    prte_process_info.nodename = strdup(&hostname[idx]);
                }
                /* add this to our list of aliases */
                prte_argv_append_nosize(&prte_process_info.aliases, prte_process_info.nodename);
                match = true;
                break;
            }
        }
        /* if we didn't find a match, then just use the hostname as-is */
        if (!match) {
            prte_process_info.nodename = strdup(hostname);
        }
        prte_argv_free(prefixes);
    } else {
        prte_process_info.nodename = strdup(hostname);
    }
}

bool prte_check_host_is_local(char *name)
{
    int i;

    for (i = 0; NULL != prte_process_info.aliases[i]; i++) {
        if (0 == strcmp(name, prte_process_info.aliases[i]) || 0 == strcmp(name, "localhost")
            || 0 == strcmp(name, "127.0.0.1")) {
            return true;
        }
    }
    return false;
}

int prte_proc_info(void)
{

    char *ptr;

    if (init) {
        return PRTE_SUCCESS;
    }

    init = true;

    prte_process_info.my_hnp_uri = NULL;
    prte_mca_base_var_register("prte", "prte", NULL, "hnp_uri", "HNP contact info",
                               PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                               PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                               PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_process_info.my_hnp_uri);

    if (NULL != prte_process_info.my_hnp_uri) {
        ptr = prte_process_info.my_hnp_uri;
        /* the uri value passed to us will have quote marks around it to protect
         * the value if passed on the command line. We must remove those
         * to have a correct uri string
         */
        if ('"' == ptr[0]) {
            /* if the first char is a quote, then so will the last one be */
            ptr[strlen(ptr) - 1] = '\0';
            memmove(ptr, ptr + 1, strlen(ptr));
        }
    }

    /* get the process id */
    prte_process_info.pid = getpid();

    /* get the number of nodes in the job */
    prte_process_info.num_nodes = 1;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "num_nodes",
                                      "Number of nodes in the job", PRTE_MCA_BASE_VAR_TYPE_INT,
                                      NULL, 0, PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                      &prte_process_info.num_nodes);

    /* get the number of times this proc has restarted */
    prte_process_info.num_restarts = 0;
    (void) prte_mca_base_var_register("prte", "prte", NULL, "num_restarts",
                                      "Number of times this proc has restarted",
                                      PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                      &prte_process_info.num_restarts);

    return PRTE_SUCCESS;
}

int prte_proc_info_finalize(void)
{
    if (!init) {
        return PRTE_SUCCESS;
    }

    if (NULL != prte_process_info.tmpdir_base) {
        free(prte_process_info.tmpdir_base);
        prte_process_info.tmpdir_base = NULL;
    }

    if (NULL != prte_process_info.top_session_dir) {
        free(prte_process_info.top_session_dir);
        prte_process_info.top_session_dir = NULL;
    }

    if (NULL != prte_process_info.jobfam_session_dir) {
        free(prte_process_info.jobfam_session_dir);
        prte_process_info.jobfam_session_dir = NULL;
    }

    if (NULL != prte_process_info.job_session_dir) {
        free(prte_process_info.job_session_dir);
        prte_process_info.job_session_dir = NULL;
    }

    if (NULL != prte_process_info.proc_session_dir) {
        free(prte_process_info.proc_session_dir);
        prte_process_info.proc_session_dir = NULL;
    }

    if (NULL != prte_process_info.nodename) {
        free(prte_process_info.nodename);
        prte_process_info.nodename = NULL;
    }

    if (NULL != prte_process_info.cpuset) {
        free(prte_process_info.cpuset);
        prte_process_info.cpuset = NULL;
    }

    if (NULL != prte_process_info.sock_stdin) {
        free(prte_process_info.sock_stdin);
        prte_process_info.sock_stdin = NULL;
    }

    if (NULL != prte_process_info.sock_stdout) {
        free(prte_process_info.sock_stdout);
        prte_process_info.sock_stdout = NULL;
    }

    if (NULL != prte_process_info.sock_stderr) {
        free(prte_process_info.sock_stderr);
        prte_process_info.sock_stderr = NULL;
    }

    prte_process_info.proc_type = PRTE_PROC_TYPE_NONE;

    prte_argv_free(prte_process_info.aliases);

    init = false;
    return PRTE_SUCCESS;
}
