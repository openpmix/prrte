/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_MCA_STATE_BASE_H
#define PRTE_MCA_STATE_BASE_H

/*
 * includes
 */
#include "prte_config.h"
#include "constants.h"

#include "src/class/pmix_list.h"
#include "src/util/pmix_printf.h"

#include "src/mca/mca.h"
#include "src/mca/state/state.h"

BEGIN_C_DECLS

/* select a component */
PRTE_EXPORT int prte_state_base_select(void);

/* debug tools */
PRTE_EXPORT void prte_state_base_print_job_state_machine(void);

PRTE_EXPORT void prte_state_base_print_proc_state_machine(void);

PRTE_EXPORT extern int prte_state_base_parent_fd;
PRTE_EXPORT extern bool prte_state_base_ready_msg;

END_C_DECLS

#endif
