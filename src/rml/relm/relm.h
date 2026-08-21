/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file:
 *
 * Reliable Messaging (RELM)
 *
 * This is the whole of RELM's interface to the rest of the tree: the RML's
 * send path calls prte_relm_start_msg, the routing layer calls
 * prte_relm_fault_handler, and prte_rml_open/close bracket the subsystem.
 * Everything else in this directory runs in response to received RELM
 * control messages.
 */

#ifndef PRTE_RELM_H
#define PRTE_RELM_H

#include "prte_config.h"
#include "constants.h"

#include "src/rml/rml_types.h"

BEGIN_C_DECLS

/* Configuration and output channel, set by prte_relm_register from the
 * relm_base MCA parameters */
typedef struct {
    int output;
    int verbosity;
    int cache_ms;
    int cache_max_count;
} prte_relm_base_t;
PRTE_EXPORT extern prte_relm_base_t prte_relm_base;

PRTE_EXPORT void prte_relm_register(void);
PRTE_EXPORT void prte_relm_open(void);
PRTE_EXPORT void prte_relm_close(void);

/* Reliably send a non-blocking message to a specific destination.
 *
 * @param[in] dst   Name of receiving process
 * @param[in] buf   Pointer to buffer to be sent (takes ownership)
 * @param[in] tag   User defined tag for matching send/recv
 *
 * @retval PRTE_SUCCESS               The message was successfully started
 * @retval PRTE_ERR_BAD_PARAM         One of the parameters was invalid
 * @retval PRTE_ERR_ADDRESSEE_UNKNOWN Contact information for dst is unavailable
 * @retval PRTE_ERR_NODE_DOWN         Provided dst is believed to have failed
 * @retval PRTE_ERROR                 An unspecified error occurred
 */
PRTE_EXPORT int prte_relm_start_msg(
    pmix_rank_t dst, pmix_data_buffer_t* buf, prte_rml_tag_t tag
);

/* Respond to failed daemons */
PRTE_EXPORT void prte_relm_fault_handler(
    const prte_rml_recovery_status_t* status
);

END_C_DECLS

#endif
