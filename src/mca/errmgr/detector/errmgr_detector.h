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

#include "prrte_config.h"

#include "src/mca/errmgr/errmgr.h"

BEGIN_C_DECLS

typedef struct {
    prrte_event_t fd_event; /* to trigger timeouts with prrte_events */
    int hb_observing;      /* the deamon vpid of the process we observe */
    int hb_observer;       /* the daemon vpid of the process that observes us */
    double hb_rstamp;      /* the date of the last hb reception */
    double hb_timeout;     /* the timeout before we start suspecting observed process as dead (delta) */
    double hb_period;      /* the time spacing between heartbeat emission (eta) */
    double hb_sstamp;      /* the date at which the last hb emission was done */
    int failed_node_count; /* the number of failed nodes in the ring */
    int *daemons_state;    /* a list of failed daemons' vpid */
} prrte_errmgr_detector_t;
static prrte_errmgr_detector_t prrte_errmgr_world_detector;

/*
 * Local Component structures
 */

PRRTE_MODULE_EXPORT extern prrte_errmgr_base_component_t prrte_errmgr_detector_component;

PRRTE_EXPORT extern prrte_errmgr_base_module_t prrte_errmgr_detector_module;

/*
 * Propagator functions
 */
int prrte_errmgr_failure_propagate(prrte_jobid_t *job, prrte_process_name_t *daemon, prrte_proc_state_t state);
int prrte_errmgr_failure_propagate_recv(prrte_buffer_t* buffer);
int prrte_errmgr_init_failure_propagate(void);
int prrte_errmgr_finalize_failure_propagate(void);
bool errmgr_get_daemon_status(prrte_process_name_t daemon);
void errmgr_set_daemon_status(prrte_process_name_t daemon);
extern int prrte_errmgr_enable_detector(bool flag);
END_C_DECLS

#endif /* MCA_ERRMGR_DETECTOR_EXPORT_H */
