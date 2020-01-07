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
 * Copyright (c) 2009-2016 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <ctype.h>

#include "src/mca/base/base.h"
#include "src/mca/base/prrte_mca_base_var.h"
#include "src/util/argv.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/util/proc_info.h"

#include "src/util/attr.h"

#include "src/util/proc_info.h"

/* provide a connection to a reqd variable */
extern bool prrte_keep_fqdn_hostnames;

#define PRRTE_NAME_INVALID {PRRTE_JOBID_INVALID, PRRTE_VPID_INVALID}

PRRTE_EXPORT prrte_process_info_t prrte_process_info = {
    .my_name =                         PRRTE_NAME_INVALID,
    .my_daemon =                       PRRTE_NAME_INVALID,
    .my_daemon_uri =                   NULL,
    .my_hnp =                          PRRTE_NAME_INVALID,
    .my_hnp_uri =                      NULL,
    .my_parent =                       PRRTE_NAME_INVALID,
    .hnp_pid =                         0,
    .app_num =                         0,
    .num_procs =                       1,
    .max_procs =                       1,
    .num_daemons =                     1,
    .num_nodes =                       1,
    .nodename =                        NULL,
    .aliases =                         NULL,
    .pid =                             0,
    .proc_type =                       PRRTE_PROC_TYPE_NONE,
    .my_port =                         0,
    .num_restarts =                    0,
    .my_node_rank =                    PRRTE_NODE_RANK_INVALID,
    .my_local_rank =                   PRRTE_LOCAL_RANK_INVALID,
    .num_local_peers =                 0,
    .tmpdir_base =                     NULL,
    .top_session_dir =                 NULL,
    .jobfam_session_dir =              NULL,
    .job_session_dir =                 NULL,
    .proc_session_dir =                NULL,
    .sock_stdin =                      NULL,
    .sock_stdout =                     NULL,
    .sock_stderr =                     NULL,
    .cpuset =                          NULL,
    .app_rank =                        -1,
    .my_hostid =                       PRRTE_VPID_INVALID
};

static bool init=false;
static int prrte_ess_node_rank;
static char *prrte_strip_prefix;

void prrte_setup_hostname(void)
{
    char *ptr;
    char hostname[PRRTE_MAXHOSTNAMELEN];
    char **prefixes;
    bool match;
    int i, idx;

    /* whether or not to keep FQDN hostnames */
    prrte_keep_fqdn_hostnames = false;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "keep_fqdn_hostnames",
                                  "Whether or not to keep FQDN hostnames [default: no]",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                  &prrte_keep_fqdn_hostnames);

    /* get the nodename */
    gethostname(hostname, sizeof(hostname));
    /* add this to our list of aliases */
    prrte_argv_append_nosize(&prrte_process_info.aliases, hostname);

    // Strip off the FQDN if present, ignore IP addresses
    if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(hostname) ) {
        if (NULL != (ptr = strchr(hostname, '.'))) {
            *ptr = '\0';
            /* add this to our list of aliases */
            prrte_argv_append_nosize(&prrte_process_info.aliases, hostname);
        }
    }

    prrte_strip_prefix = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "strip_prefix",
                                        "Prefix(es) to match when deciding whether to strip leading characters and zeroes from "
                                        "node names returned by daemons", PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                        PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                        &prrte_strip_prefix);

    /* we have to strip node names here, if user directs, to ensure that
     * the names exchanged in the modex match the names found locally
     */
    if (NULL != prrte_strip_prefix && !prrte_net_isaddr(hostname)) {
        prefixes = prrte_argv_split(prrte_strip_prefix, ',');
        match = false;
        for (i=0; NULL != prefixes[i]; i++) {
            if (0 == strncmp(hostname, prefixes[i], strlen(prefixes[i]))) {
                /* remove the prefix and leading zeroes */
                idx = strlen(prefixes[i]);
                while (idx < (int)strlen(hostname) &&
                       (hostname[idx] <= '0' || '9' < hostname[idx])) {
                    idx++;
                }
                if ((int)strlen(hostname) <= idx) {
                    /* there were no non-zero numbers in the name */
                    prrte_process_info.nodename = strdup(&hostname[strlen(prefixes[i])]);
                } else {
                    prrte_process_info.nodename = strdup(&hostname[idx]);
                }
                /* add this to our list of aliases */
                prrte_argv_append_nosize(&prrte_process_info.aliases, prrte_process_info.nodename);
                match = true;
                break;
            }
        }
        /* if we didn't find a match, then just use the hostname as-is */
        if (!match) {
            prrte_process_info.nodename = strdup(hostname);
        }
        prrte_argv_free(prefixes);
    } else {
        prrte_process_info.nodename = strdup(hostname);
    }

    /* add "localhost" to our list of aliases */
    prrte_argv_append_nosize(&prrte_process_info.aliases, "localhost");

}

bool prrte_check_host_is_local(char *name)
{
    int i;

    for (i=0; NULL != prrte_process_info.aliases[i]; i++) {
        if (0 == strcmp(name, prrte_process_info.aliases[i])) {
            return true;
        }
    }
    return false;
}

int prrte_proc_info(void)
{

    char *ptr;

    if (init) {
        return PRRTE_SUCCESS;
    }

    init = true;

    prrte_process_info.my_hnp_uri = NULL;
    prrte_mca_base_var_register ("prrte", "prrte", NULL, "hnp_uri",
                                 "HNP contact info",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                 PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                 PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                 &prrte_process_info.my_hnp_uri);

    if (NULL != prrte_process_info.my_hnp_uri) {
        ptr = prrte_process_info.my_hnp_uri;
        /* the uri value passed to us will have quote marks around it to protect
        * the value if passed on the command line. We must remove those
        * to have a correct uri string
        */
        if ('"' == ptr[0]) {
            /* if the first char is a quote, then so will the last one be */
            ptr[strlen(ptr)-1] = '\0';
            memmove (ptr, ptr + 1, strlen (ptr));
        }
    }

    prrte_process_info.my_daemon_uri = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "local_daemon_uri",
                                        "Daemon contact info",
                                        PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                        PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                        PRRTE_INFO_LVL_9,
                                        PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                        &prrte_process_info.my_daemon_uri);

    if (NULL != prrte_process_info.my_daemon_uri) {
        ptr = prrte_process_info.my_daemon_uri;
        /* the uri value passed to us may have quote marks around it to protect
         * the value if passed on the command line. We must remove those
         * to have a correct uri string
         */
        if ('"' == ptr[0]) {
            /* if the first char is a quote, then so will the last one be */
            ptr[strlen(ptr)-1] = '\0';
            memmove (ptr, ptr + 1, strlen (ptr) - 1);
        }
    }

    prrte_process_info.app_num = 0;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "app_num",
                                        "Index of the app_context that defines this proc",
                                        PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                        PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                        PRRTE_INFO_LVL_9,
                                        PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                        &prrte_process_info.app_num);

    /* get the process id */
    prrte_process_info.pid = getpid();

    /* get the number of nodes in the job */
    prrte_process_info.num_nodes = 1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "num_nodes",
                                        "Number of nodes in the job",
                                        PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                        PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                        PRRTE_INFO_LVL_9,
                                        PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                        &prrte_process_info.num_nodes);

    /* get the number of times this proc has restarted */
    prrte_process_info.num_restarts = 0;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "num_restarts",
                                        "Number of times this proc has restarted",
                                        PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                        PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                        PRRTE_INFO_LVL_9,
                                        PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                        &prrte_process_info.num_restarts);

    prrte_process_info.app_rank = 0;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "app_rank",
                                        "Rank of this proc within its app_context",
                                        PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                        PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                        PRRTE_INFO_LVL_9,
                                        PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                        &prrte_process_info.app_rank);

    /* get my node rank in case we are using static ports - this won't
     * be present for daemons, so don't error out if we don't have it
     */
    prrte_ess_node_rank = PRRTE_NODE_RANK_INVALID;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "ess_node_rank", "Process node rank",
                                        PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                        PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                        PRRTE_INFO_LVL_9,
                                        PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                        &prrte_ess_node_rank);
    prrte_process_info.my_node_rank = (prrte_node_rank_t) prrte_ess_node_rank;

    return PRRTE_SUCCESS;
}


int prrte_proc_info_finalize(void)
{
    if (!init) {
        return PRRTE_SUCCESS;
    }

    if (NULL != prrte_process_info.tmpdir_base) {
        free(prrte_process_info.tmpdir_base);
        prrte_process_info.tmpdir_base = NULL;
    }

    if (NULL != prrte_process_info.top_session_dir) {
        free(prrte_process_info.top_session_dir);
        prrte_process_info.top_session_dir = NULL;
    }

    if (NULL != prrte_process_info.jobfam_session_dir) {
        free(prrte_process_info.jobfam_session_dir);
        prrte_process_info.jobfam_session_dir = NULL;
    }

    if (NULL != prrte_process_info.job_session_dir) {
        free(prrte_process_info.job_session_dir);
        prrte_process_info.job_session_dir = NULL;
    }

    if (NULL != prrte_process_info.proc_session_dir) {
        free(prrte_process_info.proc_session_dir);
        prrte_process_info.proc_session_dir = NULL;
    }

    if (NULL != prrte_process_info.nodename) {
        free(prrte_process_info.nodename);
        prrte_process_info.nodename = NULL;
    }

    if (NULL != prrte_process_info.cpuset) {
        free(prrte_process_info.cpuset);
        prrte_process_info.cpuset = NULL;
    }

    if (NULL != prrte_process_info.sock_stdin) {
        free(prrte_process_info.sock_stdin);
        prrte_process_info.sock_stdin = NULL;
    }

    if (NULL != prrte_process_info.sock_stdout) {
        free(prrte_process_info.sock_stdout);
        prrte_process_info.sock_stdout = NULL;
    }

    if (NULL != prrte_process_info.sock_stderr) {
        free(prrte_process_info.sock_stderr);
        prrte_process_info.sock_stderr = NULL;
    }

    prrte_process_info.proc_type = PRRTE_PROC_TYPE_NONE;

    prrte_argv_free(prrte_process_info.aliases);

    init = false;
    return PRRTE_SUCCESS;
}
