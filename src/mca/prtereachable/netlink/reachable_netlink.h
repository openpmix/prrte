/*
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_MCA_REACHABLE_NETLINK_H
#define PRRTE_MCA_REACHABLE_NETLINK_H

#include "prrte_config.h"

#include "src/mca/prtereachable/prtereachable.h"

BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_reachable_base_component_t
    prrte_prtereachable_netlink_component;

PRRTE_EXPORT extern const prrte_reachable_base_module_t
    prrte_prtereachable_netlink_module;

END_C_DECLS

#endif /* MCA_REACHABLE_NETLINK_H */
