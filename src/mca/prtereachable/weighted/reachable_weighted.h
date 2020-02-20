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

#ifndef PRRTE_MCA_REACHABLE_WEIGHTED_H
#define PRRTE_MCA_REACHABLE_WEIGHTED_H

#include "prrte_config.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "src/mca/prtereachable/prtereachable.h"
#include "src/mca/mca.h"
#include "src/event/event-internal.h"
#include "src/util/proc_info.h"

BEGIN_C_DECLS

typedef struct {
    prrte_reachable_base_component_t super;
} prrte_prtereachable_weighted_component_t;

PRRTE_EXPORT extern prrte_prtereachable_weighted_component_t prrte_prtereachable_weighted_component;

PRRTE_EXPORT extern const prrte_reachable_base_module_t prrte_prtereachable_weighted_module;


END_C_DECLS

#endif /* MCA_REACHABLE_WEIGHTED_H */
