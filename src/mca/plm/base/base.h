/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
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

#ifndef MCA_PLM_BASE_H
#define MCA_PLM_BASE_H

/*
 * includes
 */
#include "prte_config.h"

#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"
#include "src/util/pmix_printf.h"

#include "src/mca/plm/plm.h"

BEGIN_C_DECLS

/*
 * MCA framework
 */
PRTE_EXPORT extern pmix_mca_base_framework_t prte_plm_base_framework;
/*
 * Select an available component.
 */
PRTE_EXPORT int prte_plm_base_select(void);

/**
 * Functions that other frameworks may need to call directly
 * Specifically, the ODLS needs to access some of these
 * to avoid recursive callbacks
 */
PRTE_EXPORT void prte_plm_base_set_slots(prte_node_t *node);
PRTE_EXPORT void prte_plm_base_setup_job(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_complete_setup(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_daemons_reported(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_allocation_complete(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_daemons_launched(int fd, short args, void *cbdata);
/* NOTE: the INIT_COMPLETE and VM_READY handlers are state/dvm's
 * init_complete() and vm_ready(); the copies that used to be declared here
 * were unregistered duplicates and are gone */
PRTE_EXPORT void prte_plm_base_mapping_complete(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_launch_apps(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_send_launch_msg(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_post_launch(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_registered(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_plm_base_wrap_args(char **args);
PRTE_EXPORT int prte_plm_base_spawn_response(int32_t status, prte_job_t *jdata);

/* Build the body of a PRTE_PLM_UPDATE_PROC_STATE message.
 *
 * These are the writers for the format prte_plm_base_receive() reads, and
 * they live beside it deliberately: the wire carries no version (mixed-
 * version DVMs are forbidden), so a field added, dropped or retyped has to
 * change reader and writer in one commit, and that is only enforceable if
 * there is one writer to find.  There used to be three - errmgr/prted,
 * state/prted, and a hand-rolled copy in prted_abort() - so a reader change
 * meant grepping for the pattern and hoping.
 *
 * The message is: the command (PMIX_UINT8), then for each job its nspace
 * (PMIX_PROC_NSPACE), then {rank, pid, state, exit_code} per proc, then a
 * PMIX_RANK_INVALID rank saying that job is complete.
 *
 * pack_state_update() packs one job's nspace, its local children, and the
 * terminator.  With skip_reported set it packs only children not already
 * flagged PRTE_PROC_FLAG_TERM_REPORTED, and flags the ones it packs - the
 * normal-termination path needs that; an error report does not.
 *
 * Covered by the round-trip test in test/unit/plm.
 */
PRTE_EXPORT int prte_plm_base_pack_state_for_proc(pmix_data_buffer_t *alert,
                                                  prte_proc_t *child);
PRTE_EXPORT int prte_plm_base_pack_state_update(pmix_data_buffer_t *alert,
                                                prte_job_t *jobdat,
                                                bool skip_reported);

END_C_DECLS

#endif
