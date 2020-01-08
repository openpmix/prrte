/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Amazon.com, Inc. or its affiliates.
 *                         All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_REACHABLE_WEIGHTED_H
#define MCA_REACHABLE_WEIGHTED_H

#include "prrte_config.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "src/mca/reachable/reachable.h"
#include "src/mca/mca.h"
#include "src/event/event-internal.h"
#include "src/util/proc_info.h"

BEGIN_C_DECLS

typedef struct {
    prrte_reachable_base_component_t super;
} prrte_reachable_weighted_component_t;

PRRTE_EXPORT extern prrte_reachable_weighted_component_t prrte_reachable_weighted_component;

PRRTE_EXPORT extern const prrte_reachable_base_module_t prrte_reachable_weighted_module;


END_C_DECLS

#endif /* MCA_REACHABLE_WEIGHTED_H */
