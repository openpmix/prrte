/*
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
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
    double hb_timeout;     /* the timeout before we start suspecting observed process as dead (delta) */
    double hb_period;      /* the time spacing between heartbeat emission (eta) */
    double hb_sstamp;      /* the date at which the last hb emission was done */
    int failed_node_count; /* the number of failed nodes in the ring */
    int *daemons_state;    /* a list of failed daemons' vpid */
} prte_errmgr_detector_t;
static prte_errmgr_detector_t prte_errmgr_world_detector;

/*
 * Local Component structures
 */

PRTE_MODULE_EXPORT extern prte_errmgr_base_component_t prte_errmgr_detector_component;

PRTE_EXPORT extern prte_errmgr_base_module_t prte_errmgr_detector_module;

/*
 * Propagator functions
 */
int prte_errmgr_failure_propagate(prte_jobid_t *job, prte_process_name_t *daemon, prte_proc_state_t state);
int prte_errmgr_failure_propagate_recv(prte_buffer_t* buffer);
int prte_errmgr_init_failure_propagate(void);
int prte_errmgr_finalize_failure_propagate(void);
bool errmgr_get_daemon_status(prte_process_name_t daemon);
void errmgr_set_daemon_status(prte_process_name_t daemon);
extern int prte_errmgr_enable_detector(bool flag);
END_C_DECLS

#endif /* MCA_ERRMGR_DETECTOR_EXPORT_H */
