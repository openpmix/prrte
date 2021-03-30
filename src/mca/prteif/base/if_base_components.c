/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#include "constants.h"
#include "src/mca/mca.h"
#include "src/mca/prteif/base/base.h"
#include "src/mca/prteif/base/static-components.h"
#include "src/mca/prteif/prteif.h"
#include "src/util/output.h"

/* instantiate the global list of interfaces */
prte_list_t prte_if_list = {{0}};
bool prte_if_retain_loopback = false;

static int prte_if_base_register(prte_mca_base_register_flag_t flags);
static int prte_if_base_open(prte_mca_base_open_flag_t flags);
static int prte_if_base_close(void);
static void prte_if_construct(prte_if_t *obj);

static bool frameopen = false;

/* instance the prte_if_t object */
PRTE_CLASS_INSTANCE(prte_if_t, prte_list_item_t, prte_if_construct, NULL);

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, prteif, NULL, prte_if_base_register, prte_if_base_open,
                                prte_if_base_close, prte_prteif_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static int prte_if_base_register(prte_mca_base_register_flag_t flags)
{
    prte_if_retain_loopback = false;
    (void) prte_mca_base_framework_var_register(&prte_prteif_base_framework, "retain_loopback",
                                                "If nonzero, retain loopback interfaces",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                                &prte_if_retain_loopback);

    return PRTE_SUCCESS;
}

static int prte_if_base_open(prte_mca_base_open_flag_t flags)
{
    if (frameopen) {
        return PRTE_SUCCESS;
    }
    frameopen = true;

    /* setup the global list */
    PRTE_CONSTRUCT(&prte_if_list, prte_list_t);

    return prte_mca_base_framework_components_open(&prte_prteif_base_framework, flags);
}

static int prte_if_base_close(void)
{
    prte_list_item_t *item;

    if (!frameopen) {
        return PRTE_SUCCESS;
    }
    frameopen = false;

    while (NULL != (item = prte_list_remove_first(&prte_if_list))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_if_list);

    return prte_mca_base_framework_components_close(&prte_prteif_base_framework, NULL);
}

static void prte_if_construct(prte_if_t *obj)
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
