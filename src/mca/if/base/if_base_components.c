/*
 * Copyright (c) 2010-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"

#include "constants.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/if/if.h"
#include "src/mca/if/base/base.h"
#include "src/mca/if/base/static-components.h"

/* instantiate the global list of interfaces */
prrte_list_t prrte_if_list = {{0}};
bool prrte_if_do_not_resolve = false;
bool prrte_if_retain_loopback = false;

static int prrte_if_base_register (prrte_mca_base_register_flag_t flags);
static int prrte_if_base_open (prrte_mca_base_open_flag_t flags);
static int prrte_if_base_close(void);
static void prrte_if_construct(prrte_if_t *obj);

static bool frameopen = false;

/* instance the prrte_if_t object */
PRRTE_CLASS_INSTANCE(prrte_if_t, prrte_list_item_t, prrte_if_construct, NULL);

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, if, NULL, prrte_if_base_register,
                                 prrte_if_base_open, prrte_if_base_close,
                                 prrte_if_base_static_components, 0);

static int prrte_if_base_register (prrte_mca_base_register_flag_t flags)
{
    prrte_if_do_not_resolve = false;
    (void) prrte_mca_base_framework_var_register (&prrte_if_base_framework, "do_not_resolve",
                                                  "If nonzero, do not attempt to resolve interfaces",
                                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                                  &prrte_if_do_not_resolve);

    prrte_if_retain_loopback = false;
    (void) prrte_mca_base_framework_var_register (&prrte_if_base_framework, "retain_loopback",
                                                  "If nonzero, retain loopback interfaces",
                                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                                  &prrte_if_retain_loopback);

    return PRRTE_SUCCESS;
}


static int prrte_if_base_open (prrte_mca_base_open_flag_t flags)
{
    if (frameopen) {
        return PRRTE_SUCCESS;
    }
    frameopen = true;

    /* setup the global list */
    PRRTE_CONSTRUCT(&prrte_if_list, prrte_list_t);

    return prrte_mca_base_framework_components_open(&prrte_if_base_framework, flags);
}


static int prrte_if_base_close(void)
{
    prrte_list_item_t *item;

    if (!frameopen) {
        return PRRTE_SUCCESS;
    }
    frameopen = false;

    while (NULL != (item = prrte_list_remove_first(&prrte_if_list))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_if_list);

    return prrte_mca_base_framework_components_close(&prrte_if_base_framework, NULL);
}

static void prrte_if_construct(prrte_if_t *obj)
{
    memset(obj->if_name, 0, sizeof(obj->if_name));
    obj->if_index = -1;
    obj->if_kernel_index = (uint16_t) -1;
    obj->af_family = PF_UNSPEC;
    obj->if_flags = 0;
    obj->if_speed = 0;
    memset(&obj->if_addr, 0, sizeof(obj->if_addr));
    obj->if_mask = 0;
    obj->if_bandwidth = 0;
    memset(obj->if_mac, 0, sizeof(obj->if_mac));
    obj->ifmtu = 0;
}
