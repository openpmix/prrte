/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#include "reachable_netlink.h"
#include "src/include/constants.h"
#include "src/mca/prtereachable/prtereachable.h"
#include "src/util/proc_info.h"

/*
 * Public string showing the reachable netlink component version number
 */
const char *prte_prtereachable_netlink_component_version_string
    = "PRTE netlink prtereachable MCA component version " PRTE_VERSION;

/*
 * Local function
 */
static int reachable_netlink_open(void);
static int reachable_netlink_close(void);
static int reachable_netlink_component_query(prte_mca_base_module_t **module, int *priority);
static int component_register(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prte_reachable_base_component_t prte_prtereachable_netlink_component = {

    /* First, the mca_component_t struct containing meta information
       about the component itself */

    .base_version = {
        /* Indicate that we are a reachable v1.1.0 component (which also
           implies a specific MCA version) */

        PRTE_REACHABLE_BASE_VERSION_2_0_0,

        /* Component name and version */

        .mca_component_name = "netlink",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PMIX_RELEASE_VERSION),

        /* Component open and close functions */

        .mca_open_component = reachable_netlink_open,
        .mca_close_component = reachable_netlink_close,
        .mca_query_component = reachable_netlink_component_query,
        .mca_register_component_params = component_register,
    },
    /* Next the MCA v1.0.0 component meta data */
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    }
};

static int reachable_netlink_open(void)
{
    /* construct the component fields */

    return PRTE_SUCCESS;
}

static int reachable_netlink_close(void)
{
    return PRTE_SUCCESS;
}

static int component_register(void)
{
    return PRTE_SUCCESS;
}

static int reachable_netlink_component_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = 50;
    *module = (prte_mca_base_module_t *) &prte_prtereachable_netlink_module;
    return PRTE_SUCCESS;
}
