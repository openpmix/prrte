/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/class/prte_list.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"

#include "src/mca/rtc/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */

#include "src/mca/rtc/base/static-components.h"

/*
 * Global variables
 */
prte_rtc_API_module_t prte_rtc = {prte_rtc_base_assign, prte_rtc_base_set,
                                  prte_rtc_base_get_avail_vals};
prte_rtc_base_t prte_rtc_base = {{{0}}};

static int prte_rtc_base_close(void)
{
    prte_list_item_t *item;

    /* cleanup globals */
    while (NULL != (item = prte_list_remove_first(&prte_rtc_base.actives))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_rtc_base.actives);

    return prte_mca_base_framework_components_close(&prte_rtc_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_rtc_base_open(prte_mca_base_open_flag_t flags)
{
    /* init the globals */
    PRTE_CONSTRUCT(&prte_rtc_base.actives, prte_list_t);

    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_rtc_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, rtc, "PRTE Mapping Subsystem", NULL, prte_rtc_base_open,
                                prte_rtc_base_close, prte_rtc_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static void mdes(prte_rtc_base_selected_module_t *active)
{
    if (NULL != active->module->finalize) {
        active->module->finalize();
    }
}
PRTE_CLASS_INSTANCE(prte_rtc_base_selected_module_t, prte_list_item_t, NULL, mdes);

static void rcon(prte_rtc_resource_t *p)
{
    p->component = NULL;
    p->category = NULL;
    PRTE_CONSTRUCT(&p->control, prte_value_t);
}
static void rdes(prte_rtc_resource_t *p)
{
    if (NULL != p->component) {
        free(p->component);
    }
    if (NULL != p->category) {
        free(p->category);
    }
    PRTE_DESTRUCT(&p->control);
}
PRTE_CLASS_INSTANCE(prte_rtc_resource_t, prte_list_item_t, rcon, rdes);
