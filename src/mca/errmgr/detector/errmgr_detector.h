/*
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 */

#ifndef MCA_ERRMGR_DETECTOR_EXPORT_H
#define MCA_ERRMGR_DETECTOR_EXPORT_H

#include "prte_config.h"

#include "src/mca/errmgr/errmgr.h"

BEGIN_C_DECLS

typedef struct {
    prte_event_t fd_event; /* to trigger timeouts with prte_events */
    int hb_observing;      /* the deamon vpid of the process we observe */
    int hb_observer;       /* the daemon vpid of the process that observes us */
    double hb_rstamp;      /* the date of the last hb reception */
    double hb_timeout; /* the timeout before we start suspecting observed process as dead (delta) */
    double hb_period;  /* the time spacing between heartbeat emission (eta) */
    double hb_sstamp;  /* the date at which the last hb emission was done */
    int failed_node_count; /* the number of failed nodes in the ring */
    int *daemons_state;    /* a list of failed daemons' vpid */
} prte_errmgr_detector_t;

/*
 * Local Component structures
 */

typedef struct {
    prte_errmgr_base_component_t super;
    double heartbeat_period;
    double heartbeat_timeout;
} prte_errmgr_detector_component_t;

PRTE_MODULE_EXPORT extern prte_errmgr_detector_component_t prte_errmgr_detector_component;

PRTE_EXPORT extern prte_errmgr_base_module_t prte_errmgr_detector_module;

/*
 * Propagator functions
 */
int prte_errmgr_failure_propagate(pmix_nspace_t *job, pmix_proc_t *daemon, prte_proc_state_t state);
int prte_errmgr_failure_propagate_recv(pmix_data_buffer_t *buffer);
int prte_errmgr_init_failure_propagate(void);
int prte_errmgr_finalize_failure_propagate(void);
bool errmgr_get_daemon_status(pmix_proc_t daemon);
void errmgr_set_daemon_status(pmix_proc_t daemon);
END_C_DECLS

#endif /* MCA_ERRMGR_DETECTOR_EXPORT_H */
