/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/class/prrte_list.h"
#include "src/mca/base/base.h"

#include "src/mca/rtc/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/rtc/base/static-components.h"

/*
 * Global variables
 */
prrte_rtc_API_module_t prrte_rtc = {
    prrte_rtc_base_assign,
    prrte_rtc_base_set,
    prrte_rtc_base_get_avail_vals
};
prrte_rtc_base_t prrte_rtc_base = {{{0}}};

static int prrte_rtc_base_close(void)
{
    prrte_list_item_t *item;

    /* cleanup globals */
    while (NULL != (item = prrte_list_remove_first(&prrte_rtc_base.actives))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_rtc_base.actives);

    return prrte_mca_base_framework_components_close(&prrte_rtc_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_rtc_base_open(prrte_mca_base_open_flag_t flags)
{
    /* init the globals */
    PRRTE_CONSTRUCT(&prrte_rtc_base.actives, prrte_list_t);

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_rtc_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, rtc, "PRRTE Mapping Subsystem",
                                 NULL, prrte_rtc_base_open, prrte_rtc_base_close,
                                 prrte_rtc_base_static_components, 0);

static void mdes(prrte_rtc_base_selected_module_t *active)
{
    if (NULL != active->module->finalize) {
        active->module->finalize();
    }
}
PRRTE_CLASS_INSTANCE(prrte_rtc_base_selected_module_t,
                   prrte_list_item_t,
                   NULL, mdes);

static void rcon(prrte_rtc_resource_t *p)
{
    p->component = NULL;
    p->category = NULL;
    PRRTE_CONSTRUCT(&p->control, prrte_value_t);
}
static void rdes(prrte_rtc_resource_t *p)
{
    if (NULL != p->component) {
        free(p->component);
    }
    if (NULL != p->category) {
        free(p->category);
    }
    PRRTE_DESTRUCT(&p->control);
}
PRRTE_CLASS_INSTANCE(prrte_rtc_resource_t,
                   prrte_list_item_t,
                   rcon, rdes);
