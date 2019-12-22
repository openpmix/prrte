/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      Inria.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"

#include "rtc_hwloc.h"

/*
 * Local functions
 */

static int rtc_hwloc_query(prrte_mca_base_module_t **module, int *priority);
static int rtc_hwloc_register(void);

static int my_priority;

prrte_rtc_hwloc_component_t prrte_rtc_hwloc_component = {
    .super = {
        .base_version = {
            PRRTE_RTC_BASE_VERSION_1_0_0,

            .mca_component_name = "hwloc",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),
            .mca_query_component = rtc_hwloc_query,
            .mca_register_component_params = rtc_hwloc_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    },
    .kind = VM_HOLE_BIGGEST
};

static char *biggest = "biggest";
static char *vmhole;

static int rtc_hwloc_register(void)
{
    prrte_mca_base_component_t *c = &prrte_rtc_hwloc_component.super.base_version;

    /* set as the default */
    my_priority = 70;
    (void) prrte_mca_base_component_var_register (c, "priority", "Priority of the HWLOC rtc component",
                                            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &my_priority);

    prrte_rtc_hwloc_component.kind = VM_HOLE_BIGGEST;
    vmhole = biggest;
    (void) prrte_mca_base_component_var_register(c, "vmhole",
                                           "Kind of VM hole to identify - none, begin, biggest, libs, heap, stack (default=biggest)",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &vmhole);
    if (0 == strcasecmp(vmhole, "none")) {
        prrte_rtc_hwloc_component.kind = VM_HOLE_NONE;
    } else if (0 == strcasecmp(vmhole, "begin")) {
        prrte_rtc_hwloc_component.kind = VM_HOLE_BEGIN;
    } else if (0 == strcasecmp(vmhole, "biggest")) {
        prrte_rtc_hwloc_component.kind = VM_HOLE_BIGGEST;
    } else if (0 == strcasecmp(vmhole, "libs")) {
        prrte_rtc_hwloc_component.kind = VM_HOLE_IN_LIBS;
    } else if (0 == strcasecmp(vmhole, "heap")) {
        prrte_rtc_hwloc_component.kind = VM_HOLE_AFTER_HEAP;
    } else if (0 == strcasecmp(vmhole, "stack")) {
        prrte_rtc_hwloc_component.kind = VM_HOLE_BEFORE_STACK;
    } else {
        prrte_output(0, "INVALID VM HOLE TYPE");
        return PRRTE_ERROR;
    }

    return PRRTE_SUCCESS;
}


static int rtc_hwloc_query(prrte_mca_base_module_t **module, int *priority)
{
    /* Only run on the HNP */

    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_rtc_hwloc_module;

    return PRRTE_SUCCESS;
}
