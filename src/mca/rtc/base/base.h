/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 * rtc framework base functionality.
 */

#ifndef PRRTE_MCA_RTC_BASE_H
#define PRRTE_MCA_RTC_BASE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "types.h"

#include "src/class/prrte_list.h"
#include "src/util/printf.h"
#include "src/mca/mca.h"

#include "src/mca/rtc/rtc.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_rtc_base_framework;
/* select a component */
PRRTE_EXPORT    int prrte_rtc_base_select(void);

/*
 * Global functions for MCA overall collective open and close
 */

/**
 * Struct to hold data global to the rtc framework
 */
typedef struct {
    /* list of selected modules */
    prrte_list_t actives;
} prrte_rtc_base_t;

/**
 * Global instance of rtc-wide framework data
 */
PRRTE_EXPORT extern prrte_rtc_base_t prrte_rtc_base;

/**
 * Select an rtc component / module
 */
typedef struct {
    prrte_list_item_t super;
    int pri;
    prrte_rtc_base_module_t *module;
    prrte_mca_base_component_t *component;
} prrte_rtc_base_selected_module_t;
PRRTE_CLASS_DECLARATION(prrte_rtc_base_selected_module_t);

PRRTE_EXPORT void prrte_rtc_base_assign(prrte_job_t *jdata);
PRRTE_EXPORT void prrte_rtc_base_set(prrte_job_t *jdata, prrte_proc_t *proc,
                                     char ***env, int error_fd);
PRRTE_EXPORT void prrte_rtc_base_get_avail_vals(prrte_list_t *vals);

/* Called from the child to send a warning show_help message up the
   pipe to the waiting parent. */
PRRTE_EXPORT int prrte_rtc_base_send_warn_show_help(int fd, const char *file,
                                                    const char *topic, ...);

/* Called from the child to send an error message up the pipe to the
   waiting parent. */
PRRTE_EXPORT void prrte_rtc_base_send_error_show_help(int fd, int exit_status,
                                                      const char *file,
                                                      const char *topic, ...);


END_C_DECLS

#endif
