/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/proc_info.h"

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/prte_bootstrap.h"

#include "src/mca/ess/base/base.h"

/* Two addresses are the same host address (family-specific comparison). */
static bool same_inaddr(const struct sockaddr_storage *a,
                        const struct sockaddr_storage *b, int family)
{
    if (AF_INET6 == family) {
        return 0 == memcmp(&((const struct sockaddr_in6 *) a)->sin6_addr,
                           &((const struct sockaddr_in6 *) b)->sin6_addr,
                           sizeof(struct in6_addr));
    }
    return ((const struct sockaddr_in *) a)->sin_addr.s_addr
           == ((const struct sockaddr_in *) b)->sin_addr.s_addr;
}

/* Parse a single DVMNetworks token "addr/prefix" of the given family into a
 * network address and prefix length.  Returns PRTE_SUCCESS only for a CIDR of
 * the requested family; an interface name (no '/') or the wrong family fails,
 * so such tokens are simply ignored for address disambiguation. */
static int parse_cidr(const char *token, int family, struct sockaddr_storage *net,
                      uint32_t *prefix)
{
    char *copy, *slash;
    int rc = PRTE_ERR_BAD_PARAM;

    if (NULL == strchr(token, '/')) {
        return PRTE_ERR_BAD_PARAM;
    }
    copy = strdup(token);
    if (NULL == copy) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    slash = strchr(copy, '/');
    *slash = '\0';
    *prefix = (uint32_t) strtoul(slash + 1, NULL, 10);
    memset(net, 0, sizeof(*net));
    net->ss_family = family;
    if (AF_INET6 == family) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *) net;
        if (1 == inet_pton(AF_INET6, copy, &s6->sin6_addr)) {
            rc = PRTE_SUCCESS;
        }
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *) net;
        if (1 == inet_pton(AF_INET, copy, &s4->sin_addr)) {
            rc = PRTE_SUCCESS;
        }
    }
    free(copy);
    return rc;
}

/* Resolve @c host to a single address of @c family, disambiguating a multi-homed
 * host by the DVMNetworks CIDR(s) when present.  On success @c *chosen holds the
 * selected address and @c *chosen_prefix the CIDR prefix that selected it, or -1
 * when no CIDR applied (a single-homed host).  When the choice is ambiguous - a
 * host with several addresses and no CIDR to pick among them, or a CIDR that
 * still matches more than one - a help message is shown and the call fails, so a
 * silently-wrong interface is never baked into a synthesized URI. */
static int pick_host_address(prte_bootstrap_config_t *cfg, const char *host,
                             int family, struct sockaddr_storage *chosen,
                             int *chosen_prefix)
{
    struct addrinfo hints, *res = NULL, *ai;
    struct sockaddr_storage addrs[16];
    struct sockaddr_storage cidrs[16];
    uint32_t prefixes[16];
    int naddr = 0, ncidr = 0;
    int nmatch = 0, i, j;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    if (0 != getaddrinfo(host, NULL, &hints, &res) || NULL == res) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-unresolved-host", true,
                       prte_process_info.nodename, host);
        return PRTE_ERR_NOT_FOUND;
    }
    /* collect the unique resolved addresses of the requested family */
    for (ai = res; NULL != ai && naddr < (int) (sizeof(addrs) / sizeof(addrs[0]));
         ai = ai->ai_next) {
        struct sockaddr_storage cand;
        bool dup = false;
        if (ai->ai_family != family) {
            continue;
        }
        memset(&cand, 0, sizeof(cand));
        memcpy(&cand, ai->ai_addr, ai->ai_addrlen);
        for (i = 0; i < naddr; i++) {
            if (same_inaddr(&addrs[i], &cand, family)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            addrs[naddr++] = cand;
        }
    }
    freeaddrinfo(res);
    if (0 == naddr) {
        return PRTE_ERR_NOT_FOUND;
    }

    /* gather the DVMNetworks CIDR tokens of this family, if any */
    if (NULL != cfg->dvm_networks) {
        char **toks = PMIx_Argv_split(cfg->dvm_networks, ',');
        for (i = 0; NULL != toks && NULL != toks[i]
                    && ncidr < (int) (sizeof(cidrs) / sizeof(cidrs[0]));
             i++) {
            if (PRTE_SUCCESS == parse_cidr(toks[i], family, &cidrs[ncidr], &prefixes[ncidr])) {
                ncidr++;
            }
        }
        if (NULL != toks) {
            PMIx_Argv_free(toks);
        }
    }

    if (0 < ncidr) {
        /* keep only addresses that fall within one of the configured networks */
        for (i = 0; i < naddr; i++) {
            for (j = 0; j < ncidr; j++) {
                if (pmix_net_samenetwork(&addrs[i], &cidrs[j], prefixes[j])) {
                    if (0 == nmatch) {
                        *chosen = addrs[i];
                        *chosen_prefix = (int) prefixes[j];
                    }
                    nmatch++;
                    break;
                }
            }
        }
        if (0 == nmatch) {
            pmix_show_help("help-prte-runtime.txt", "bootstrap-no-matching-address", true,
                           prte_process_info.nodename, host, cfg->dvm_networks);
            return PRTE_ERR_NOT_FOUND;
        }
        if (1 < nmatch) {
            pmix_show_help("help-prte-runtime.txt", "bootstrap-ambiguous-address", true,
                           prte_process_info.nodename, host, cfg->dvm_networks);
            return PRTE_ERR_BAD_PARAM;
        }
        return PRTE_SUCCESS;
    }

    /* no usable CIDR: a single-homed host is unambiguous; anything more is not */
    if (1 == naddr) {
        *chosen = addrs[0];
        *chosen_prefix = -1;
        return PRTE_SUCCESS;
    }
    pmix_show_help("help-prte-runtime.txt", "bootstrap-ambiguous-address", true,
                   prte_process_info.nodename, host,
                   (NULL != cfg->dvm_networks) ? cfg->dvm_networks : "(none)");
    return PRTE_ERR_BAD_PARAM;
}

/* Synthesize the RML contact URI of the daemon at @c rank on @c host entirely
 * from the configuration, so a daemon can phone home to it before any nidmap has
 * been distributed.  Every daemon listens on the same DVMPort, so only the host
 * and rank vary.  The result has the same shape the OOB itself produces:
 *
 *    <process-name>;tcp://<ipv4>:<port>:<mask>       (IPv4)
 *    <process-name>;tcp6://[<ipv6>]:<port>:<mask>    (IPv6)
 *
 * The mask field is a prefix length; an explicit DVMNetmask wins, otherwise the
 * prefix of the DVMNetworks CIDR that selected the address is used, otherwise it
 * is left empty (which the OOB treats as universally reachable). */
static int synth_uri(prte_bootstrap_config_t *cfg, const char *dvm_nspace, int family,
                     pmix_rank_t rank, const char *host, char **uri)
{
    pmix_proc_t proc;
    char *namestr = NULL;
    char ipstr[INET6_ADDRSTRLEN];
    char maskbuf[16];
    const char *mask;
    struct sockaddr_storage ss;
    int prefix = -1;
    int rc;

    *uri = NULL;

    PMIX_LOAD_PROCID(&proc, dvm_nspace, rank);
    rc = prte_util_convert_process_name_to_string(&namestr, &proc);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    rc = pick_host_address(cfg, host, family, &ss, &prefix);
    if (PRTE_SUCCESS != rc) {
        free(namestr);
        return rc;
    }

    if (AF_INET6 == family) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *) &ss)->sin6_addr, ipstr, sizeof(ipstr));
    } else {
        inet_ntop(AF_INET, &((struct sockaddr_in *) &ss)->sin_addr, ipstr, sizeof(ipstr));
    }

    if (NULL != cfg->dvm_netmask) {
        mask = cfg->dvm_netmask;
    } else if (0 <= prefix) {
        pmix_snprintf(maskbuf, sizeof(maskbuf), "%d", prefix);
        mask = maskbuf;
    } else {
        mask = "";
    }

    if (AF_INET6 == family) {
        pmix_asprintf(uri, "%s;tcp6://[%s]:%u:%s", namestr, ipstr,
                      (unsigned) cfg->port, mask);
    } else {
        pmix_asprintf(uri, "%s;tcp://%s:%u:%s", namestr, ipstr,
                      (unsigned) cfg->port, mask);
    }
    free(namestr);
    return PRTE_SUCCESS;
}

/* The parsed configuration is shared between the two bootstrap phases below.
 * Phase 1 (prte_ess_base_bootstrap_params) parses the file and publishes the
 * global MCA parameters; phase 2 (prte_ess_base_bootstrap) reuses the same
 * parsed data to resolve identity once the local hostname is known. */
static prte_bootstrap_config_t bootstrap_cfg;
static bool bootstrap_cfg_valid = false;

/* Phase 1: parse the configuration and publish the DVM-wide MCA parameters.
 *
 * This MUST run before prte_register_params() (i.e., before prte_init_util),
 * because that is where the RML/OOB and other global parameters are first
 * registered - and an MCA variable reads its environment only on that first
 * registration.  None of the work here needs the local hostname, so it can run
 * this early; identity resolution (which does need the hostname) is deferred to
 * phase 2. */
int prte_ess_base_bootstrap_params(void)
{
    const char *port_param;
    char valstr[64];
    int rc;

    if (bootstrap_cfg_valid) {
        return PRTE_SUCCESS;
    }

    /* read and validate the configuration file */
    rc = prte_bootstrap_parse(&bootstrap_cfg);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    bootstrap_cfg_valid = true;

    /* resolve the address family (DVMIPVersion) into the static-port
     * parameter and the family enable/disable flags */
    if (6 == bootstrap_cfg.ip_version) {
#if PRTE_ENABLE_IPV6
        port_param = "PRTE_MCA_prte_static_ipv6_ports";
        PMIx_Setenv("PRTE_MCA_prte_disable_ipv6_family", "0", true, &environ);
        PMIx_Setenv("PRTE_MCA_prte_disable_ipv4_family", "1", true, &environ);
#else
        pmix_show_help("help-prte-runtime.txt", "bootstrap-ipv6-unavailable", true,
                       prte_process_info.nodename);
        return PRTE_ERR_SILENT;
#endif
    } else {
        port_param = "PRTE_MCA_prte_static_ipv4_ports";
    }

    /* apply the inter-node network selection, if given */
    if (NULL != bootstrap_cfg.dvm_networks) {
        PMIx_Setenv("PRTE_MCA_prte_if_include", bootstrap_cfg.dvm_networks, true, &environ);
    }

    /* apply the FQDN matching choice */
    PMIx_Setenv("PRTE_MCA_prte_keep_fqdn_hostnames", bootstrap_cfg.keep_fqdn ? "1" : "0", true,
                &environ);

    /* every process listens on the shared well-known DVM port */
    pmix_snprintf(valstr, sizeof(valstr), "%u", (unsigned) bootstrap_cfg.port);
    PMIx_Setenv(port_param, valstr, true, &environ);

    /* seed the connection-retry backoff.  retry_delay defaults to 0 (no
     * retry), so we must positively enable it; these are bootstrap's own
     * defaults, set only if the operator has not (overwrite=false), while the
     * cap comes from the configuration file (overwrite=true, config trumps). */
    PMIx_Setenv("PRTE_MCA_prte_retry_delay", "1", false, &environ);
    PMIx_Setenv("PRTE_MCA_prte_max_recon_attempts", "-1", false, &environ);
    pmix_snprintf(valstr, sizeof(valstr), "%u", (unsigned) bootstrap_cfg.retry_max_delay);
    PMIx_Setenv("PRTE_MCA_prte_retry_max_delay", valstr, true, &environ);

    /* wire ourselves into the radix routing tree at boot so that every daemon
     * phones home to its parent rather than piling directly onto the
     * controller; the radix must match across the DVM */
    pmix_snprintf(valstr, sizeof(valstr), "%d", bootstrap_cfg.radix);
    PMIx_Setenv("PRTE_MCA_rml_base_radix", valstr, true, &environ);

    /* bound the time a daemon will wait for a given parent to appear before it
     * heals up to the next ancestor (the controller is retried forever) */
    pmix_snprintf(valstr, sizeof(valstr), "%u", (unsigned) bootstrap_cfg.connect_max_time);
    PMIx_Setenv("PRTE_MCA_prte_connect_max_time", valstr, true, &environ);

    /* apply the DVM temporary-directory base */
    if (NULL != bootstrap_cfg.dvmtmpdir) {
        PMIx_Setenv("PRTE_MCA_prte_tmpdir_base", bootstrap_cfg.dvmtmpdir, true, &environ);
    }
    /* NOTE: SessionTmpDir and the *Log* options are parsed and carried in the
     * configuration, but their runtime plumbing (a dedicated session-dir
     * override and the DVM state-logging facility) is not yet implemented, so
     * they are intentionally not published here.  Wiring them is deferred to
     * when those facilities exist, per the bootstrap implementation plan. */

    return PRTE_SUCCESS;
}

/* Phase 2: resolve this node's identity and publish it.
 *
 * By now the local hostname has been established (prte_setup_hostname ran
 * inside prte_init_util), so we can match ourselves against the controller
 * host and the DVMNodes list.  The ess_base_* and hnp_uri parameters published
 * here are consumed later, during prte_init's framework selection, so setting
 * them at this point is correctly timed. */
int prte_ess_base_bootstrap(bool *is_controller)
{
    char *dvm_nspace = NULL;
    char *ctrl_uri = NULL;
    char valstr[64];
    bool ctrl = false;
    pmix_rank_t rank = PMIX_RANK_INVALID;
    pmix_rank_t ndaemons;
    int family;
    int rc;

    *is_controller = false;

    /* phase 1 should already have parsed the file; parse defensively if not */
    if (!bootstrap_cfg_valid) {
        rc = prte_ess_base_bootstrap_params();
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    family = (6 == bootstrap_cfg.ip_version) ? AF_INET6 : AF_INET;

    /* determine our role and rank from the configuration */
    rc = prte_bootstrap_my_identity(&bootstrap_cfg, &ctrl, &rank);
    if (PRTE_SUCCESS != rc) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-node-not-member", true,
                       prte_process_info.nodename, bootstrap_cfg.ctrlhost);
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }
    ndaemons = prte_bootstrap_num_daemons(&bootstrap_cfg);

    /* the DVM namespace is shared by every daemon */
    pmix_asprintf(&dvm_nspace, "%s-prte-dvm", bootstrap_cfg.cluster);

    if (ctrl) {
        /* we are the DVM controller: adopt the deterministic namespace and
         * rank 0, and let prte_plm_base_set_hnp_name() pick them up verbatim */
        PMIx_Setenv("PMIX_SERVER_NSPACE", dvm_nspace, true, &environ);
        PMIx_Setenv("PMIX_SERVER_RANK", "0", true, &environ);
    } else {
        /* we are an ordinary daemon: publish our identity and the count of
         * daemons in the DVM through the ess/env plumbing */
        PMIx_Setenv("PRTE_MCA_ess_base_nspace", dvm_nspace, true, &environ);
        pmix_snprintf(valstr, sizeof(valstr), "%u", (unsigned) rank);
        PMIx_Setenv("PRTE_MCA_ess_base_vpid", valstr, true, &environ);
        pmix_snprintf(valstr, sizeof(valstr), "%u", (unsigned) ndaemons);
        PMIx_Setenv("PRTE_MCA_ess_base_num_procs", valstr, true, &environ);

        /* synthesize the controller's contact URI so we can phone home
         * (synth_uri/pick_host_address show their own specific diagnostic on
         * an unresolved, ambiguous, or non-matching address) */
        rc = synth_uri(&bootstrap_cfg, dvm_nspace, family, 0, bootstrap_cfg.ctrlhost, &ctrl_uri);
        if (PRTE_SUCCESS != rc) {
            rc = PRTE_ERR_SILENT;
            goto cleanup;
        }
        PMIx_Setenv("PRTE_MCA_prte_hnp_uri", ctrl_uri, true, &environ);
    }

    *is_controller = ctrl;
    rc = PRTE_SUCCESS;

cleanup:
    if (NULL != dvm_nspace) {
        free(dvm_nspace);
    }
    if (NULL != ctrl_uri) {
        free(ctrl_uri);
    }
    /* Deliberately retain bootstrap_cfg (do not free / invalidate it here): a
     * daemon in a deep radix tree synthesizes its parent's URI later, from
     * prted, once prte_init has established the parent's rank - see
     * prte_ess_base_bootstrap_parent_uri.  The parsed config is small and the
     * daemon is persistent, so holding it for the process lifetime is cheap and
     * avoids re-reading and re-validating the file. */
    return rc;
}

int prte_ess_base_bootstrap_parent_uri(pmix_rank_t parent_rank, char **uri)
{
    const char *host = NULL;
    char *dvm_nspace = NULL;
    int family, rc;

    *uri = NULL;

    /* reuse the config parsed during the bootstrap phases; parse defensively if
     * it is somehow not available */
    if (!bootstrap_cfg_valid) {
        rc = prte_ess_base_bootstrap_params();
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    rc = prte_bootstrap_host_of_rank(&bootstrap_cfg, parent_rank, &host);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    family = (6 == bootstrap_cfg.ip_version) ? AF_INET6 : AF_INET;
    pmix_asprintf(&dvm_nspace, "%s-prte-dvm", bootstrap_cfg.cluster);

    rc = synth_uri(&bootstrap_cfg, dvm_nspace, family, parent_rank, host, uri);

    free(dvm_nspace);
    return rc;
}
