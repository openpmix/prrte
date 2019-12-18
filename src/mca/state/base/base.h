/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_STATE_BASE_H
#define PRRTE_MCA_STATE_BASE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "constants.h"

#include "src/class/prrte_list.h"
#include "src/util/printf.h"

#include "src/mca/mca.h"
#include "src/mca/state/state.h"


BEGIN_C_DECLS

/* select a component */
PRRTE_EXPORT    int prrte_state_base_select(void);

/* debug tools */
PRRTE_EXPORT void prrte_state_base_print_job_state_machine(void);

PRRTE_EXPORT void prrte_state_base_print_proc_state_machine(void);

PRRTE_EXPORT extern int prrte_state_base_parent_fd;

END_C_DECLS

#endif
