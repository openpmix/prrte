/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Shared parsing of the DVM bootstrap configuration file (prte.conf).
 *
 * Both the daemon-side ess bootstrap (src/mca/ess/base/ess_base_bootstrap.c)
 * and the controller-side ras bootstrap (src/mca/ras/bootstrap/) read the same
 * configuration file and must interpret it identically.  This is the single
 * shared implementation of the Key=Value reader, the DVMNodes expander, and
 * the canonical rank-ordering rule.
 */

#ifndef PRTE_BOOTSTRAP_H
#define PRTE_BOOTSTRAP_H

#include "prte_config.h"

#include <stdbool.h>
#include <stdint.h>

#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/* All values parsed from prte.conf.  Strings and the node array are owned by
 * the struct and released by prte_bootstrap_config_free(). */
typedef struct {
    char *cluster;         /* ClusterName (default "cluster") */
    char *ctrlhost;        /* DVMControllerHost (required) */
    uint32_t port;         /* DVMPort (default 7817) */
    int ip_version;        /* DVMIPVersion: 4 (default) or 6 */
    int radix;             /* DVMRadix (default 64) */
    uint32_t connect_max_time; /* DVMConnectMaxTime seconds (default 30) */
    char **nodes;          /* expanded DVMNodes (required) */
    bool keep_fqdn;        /* KeepFQDNHostnames (default false) */
    char *dvm_networks;    /* DVMNetworks (default NULL -> all) */
    char *dvm_netmask;     /* DVMNetmask (default NULL -> empty) */
    uint32_t retry_max_delay; /* DVMRetryMaxDelay seconds (default 5) */
    char *dvmtmpdir;       /* DVMTempDir */
    char *sessiontmpdir;   /* SessionTmpDir */
    char *ctrllogpath;     /* ControllerLogPath */
    char *prtedlogpath;    /* PRTEDLogPath */
    bool ctrl_log_jobstate;
    bool ctrl_log_procstate;
    bool prted_log_jobstate;
    bool prted_log_procstate;
} prte_bootstrap_config_t;

/**
 * Read <sysconfdir>/prte.conf, validate the required keys, and expand
 * DVMNodes into the node array.  Emits the help-prte-runtime.txt bootstrap
 * diagnostics on error.
 *
 * @param cfg  caller-provided struct; populated on success
 * @return PRTE_SUCCESS, or PRTE_ERR_SILENT if the file is missing/malformed
 *         (a help message has already been shown in that case)
 */
PRTE_EXPORT int prte_bootstrap_parse(prte_bootstrap_config_t *cfg);

/**
 * Release all memory owned by a parsed configuration.  Safe on a zeroed
 * struct.
 */
PRTE_EXPORT void prte_bootstrap_config_free(prte_bootstrap_config_t *cfg);

/**
 * The number of daemons in the DVM: the cardinality of DVMNodes when the
 * controller host is one of those nodes (it is counted as a compute node),
 * or that cardinality plus one when the controller is a separate node.
 */
PRTE_EXPORT pmix_rank_t prte_bootstrap_num_daemons(prte_bootstrap_config_t *cfg);

/**
 * Compute the canonical rank of the node named @c name.  The controller host
 * is always rank 0; the remaining DVMNodes entries are ranked in listed order
 * starting at 1, skipping the controller's entry if it appears in the list.
 *
 * @param cfg   parsed configuration
 * @param name  a node name to look up
 * @param rank  set to the computed rank on success
 * @return PRTE_SUCCESS if @c name is a member of the DVM, else PRTE_ERR_NOT_FOUND
 */
PRTE_EXPORT int prte_bootstrap_rank_of(prte_bootstrap_config_t *cfg,
                                       const char *name, pmix_rank_t *rank);

/**
 * Determine the local node's role in the DVM by matching this host (its
 * nodename and aliases) against the controller host and the DVMNodes entries.
 *
 * @param cfg            parsed configuration
 * @param is_controller  set true iff the local node is the DVM controller
 * @param rank           set to the local node's rank on success
 * @return PRTE_SUCCESS if the local node is a member, else PRTE_ERR_NOT_FOUND
 */
PRTE_EXPORT int prte_bootstrap_my_identity(prte_bootstrap_config_t *cfg,
                                           bool *is_controller, pmix_rank_t *rank);

END_C_DECLS

#endif /* PRTE_BOOTSTRAP_H */
