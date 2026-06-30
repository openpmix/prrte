/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_RAS_PMIX_H
#define PRTE_RAS_PMIX_H

#include "prte_config.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/ras/ras.h"

BEGIN_C_DECLS

struct prte_ras_pmix_component_t {
    prte_ras_base_component_t super;
    bool connect_to_system_scheduler;
    pmix_proc_t server;
    char *uri;
    char *connection_order;
    pid_t server_pid;
    char *server_host;
    uint32_t max_retries;
    uint32_t retry_delay;
};
typedef struct prte_ras_pmix_component_t prte_ras_pmix_component_t;

PRTE_EXPORT extern prte_ras_pmix_component_t prte_mca_ras_pmix_component;
PRTE_EXPORT extern prte_ras_base_module_t prte_ras_pmix_module;

END_C_DECLS

#endif
