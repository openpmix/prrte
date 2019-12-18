/*
 * Copyright (c) 2009-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#include <stdio.h>
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#include "prrte_stdint.h"
#include "src/util/alfg.h"
#include "src/util/output.h"

#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/sensor/base/base.h"
#include "src/mca/sensor/base/sensor_private.h"
#include "sensor_ft_tester.h"

/* declare the API functions */
static void sample(void);

/* instantiate the module */
prrte_sensor_base_module_t prrte_sensor_ft_tester_module = {
    NULL,
    NULL,
    NULL,
    NULL,
    sample,
    NULL
};

static void sample(void)
{
    float prob;
    prrte_proc_t *child;
    int i;

    PRRTE_OUTPUT_VERBOSE((1, prrte_sensor_base_framework.framework_output,
                         "%s sample:ft_tester considering killing something",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* are we including ourselves? */
    if (PRRTE_PROC_IS_DAEMON &&
        0 < mca_sensor_ft_tester_component.daemon_fail_prob) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_sensor_base_framework.framework_output,
                             "%s sample:ft_tester considering killing me!",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        /* roll the dice */
        prob = (double)prrte_rand(&prrte_sensor_ft_rng_buff) / (double)UINT32_MAX;
        if (prob < mca_sensor_ft_tester_component.daemon_fail_prob) {
            /* commit suicide */
            PRRTE_OUTPUT_VERBOSE((1, prrte_sensor_base_framework.framework_output,
                                 "%s sample:ft_tester committing suicide",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            prrte_errmgr.abort(1, NULL);
            return;
        }
    }

    if (0 < mca_sensor_ft_tester_component.fail_prob) {
        /* see if we should kill a child */
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                continue;
            }
            if (!child->alive || 0 == child->pid ||
                PRRTE_PROC_STATE_UNTERMINATED < child->state) {
                PRRTE_OUTPUT_VERBOSE((1, prrte_sensor_base_framework.framework_output,
                                     "%s sample:ft_tester ignoring child: %s alive %s pid %lu state %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name),
                                     child->alive ? "TRUE" : "FALSE",
                                     (unsigned long)child->pid, prrte_proc_state_to_str(child->state)));
                continue;
            }
            /* roll the dice */
            prob = (double)prrte_rand(&prrte_sensor_ft_rng_buff) / (double)UINT32_MAX;
            PRRTE_OUTPUT_VERBOSE((1, prrte_sensor_base_framework.framework_output,
                                 "%s sample:ft_tester child: %s dice: %f prob %f",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&child->name),
                                 prob, mca_sensor_ft_tester_component.fail_prob));
            if (prob < mca_sensor_ft_tester_component.fail_prob) {
                /* you shall die... */
                PRRTE_OUTPUT_VERBOSE((1, prrte_sensor_base_framework.framework_output,
                                     "%s sample:ft_tester killing %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name)));
                kill(child->pid, SIGTERM);
                /* are we allowing multiple deaths */
                if (!mca_sensor_ft_tester_component.multi_fail) {
                    break;
                }
            }
        }
    }
}
