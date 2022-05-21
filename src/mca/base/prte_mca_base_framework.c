/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/prte_config.h"

#include "src/include/constants.h"
#include "src/include/pmix_prefetch.h"
#include "src/util/output.h"
#include "src/util/pmix_printf.h"

#include "prte_mca_base_framework.h"
#include "prte_mca_base_var.h"
#include "src/mca/base/base.h"

bool prte_mca_base_framework_is_registered(struct prte_mca_base_framework_t *framework)
{
    return !!(framework->framework_flags & PRTE_MCA_BASE_FRAMEWORK_FLAG_REGISTERED);
}

bool prte_mca_base_framework_is_open(struct prte_mca_base_framework_t *framework)
{
    return !!(framework->framework_flags & PRTE_MCA_BASE_FRAMEWORK_FLAG_OPEN);
}

static void framework_open_output(struct prte_mca_base_framework_t *framework)
{
    if (0 < framework->framework_verbose) {
        if (-1 == framework->framework_output) {
            framework->framework_output = prte_output_open(NULL);
        }
        prte_output_set_verbosity(framework->framework_output, framework->framework_verbose);
    } else if (-1 != framework->framework_output) {
        prte_output_close(framework->framework_output);
        framework->framework_output = -1;
    }
}

static void framework_close_output(struct prte_mca_base_framework_t *framework)
{
    if (-1 != framework->framework_output) {
        prte_output_close(framework->framework_output);
        framework->framework_output = -1;
    }
}

int prte_mca_base_framework_register(struct prte_mca_base_framework_t *framework,
                                     prte_mca_base_register_flag_t flags)
{
    char *desc;
    int ret;

    assert(NULL != framework);

    framework->framework_refcnt++;

    if (prte_mca_base_framework_is_registered(framework)) {
        return PRTE_SUCCESS;
    }

    PMIX_CONSTRUCT(&framework->framework_components, pmix_list_t);
    PMIX_CONSTRUCT(&framework->framework_failed_components, pmix_list_t);

    if (framework->framework_flags & PRTE_MCA_BASE_FRAMEWORK_FLAG_NO_DSO) {
        flags |= PRTE_MCA_BASE_REGISTER_STATIC_ONLY;
    }

    if (!(PRTE_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER & framework->framework_flags)) {
        /* register this framework with the MCA variable system */
        ret = prte_mca_base_var_group_register(framework->framework_project,
                                               framework->framework_name, NULL,
                                               framework->framework_description);
        if (0 > ret) {
            return ret;
        }

        pmix_asprintf(&desc,
                      "Default selection set of components for the %s framework (<none>"
                      " means use all components that can be found)",
                      framework->framework_name);
        ret = prte_mca_base_var_register(framework->framework_project, framework->framework_name,
                                         NULL, NULL, desc, PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                         PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_2,
                                         PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                         &framework->framework_selection);
        free(desc);
        if (0 > ret) {
            return ret;
        }

        /* register a verbosity variable for this framework */
        ret = pmix_asprintf(&desc, "Verbosity level for the %s framework (default: 0)",
                            framework->framework_name);
        if (0 > ret) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        framework->framework_verbose = PRTE_MCA_BASE_VERBOSE_ERROR;
        ret = prte_mca_base_framework_var_register(framework, "verbose", desc,
                                                   PRTE_MCA_BASE_VAR_TYPE_INT,
                                                   &prte_mca_base_var_enum_verbose, 0,
                                                   PRTE_MCA_BASE_VAR_FLAG_SETTABLE, PRTE_INFO_LVL_8,
                                                   PRTE_MCA_BASE_VAR_SCOPE_LOCAL,
                                                   &framework->framework_verbose);
        free(desc);
        if (0 > ret) {
            return ret;
        }

        /* check the initial verbosity and open the output if necessary. we
           will recheck this on open */
        framework_open_output(framework);

        /* register framework variables */
        if (NULL != framework->framework_register) {
            ret = framework->framework_register(flags);
            if (PRTE_SUCCESS != ret) {
                return ret;
            }
        }

        /* register components variables */
        ret = prte_mca_base_framework_components_register(framework, flags);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }
    }

    framework->framework_flags |= PRTE_MCA_BASE_FRAMEWORK_FLAG_REGISTERED;

    /* framework did not provide a register function */
    return PRTE_SUCCESS;
}

int prte_mca_base_framework_register_list(prte_mca_base_framework_t **frameworks,
                                          prte_mca_base_register_flag_t flags)
{
    if (NULL == frameworks) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; frameworks[i]; ++i) {
        int ret = prte_mca_base_framework_register(frameworks[i], flags);
        if (PMIX_UNLIKELY(PRTE_SUCCESS != ret && PRTE_ERR_NOT_AVAILABLE != ret)) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}

int prte_mca_base_framework_open(struct prte_mca_base_framework_t *framework,
                                 prte_mca_base_open_flag_t flags)
{
    int ret;

    assert(NULL != framework);

    /* register this framework before opening it */
    ret = prte_mca_base_framework_register(framework, PRTE_MCA_BASE_REGISTER_DEFAULT);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /* check if this framework is already open */
    if (prte_mca_base_framework_is_open(framework)) {
        return PRTE_SUCCESS;
    }

    if (PRTE_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER & framework->framework_flags) {
        flags |= PRTE_MCA_BASE_OPEN_FIND_COMPONENTS;

        if (PRTE_MCA_BASE_FRAMEWORK_FLAG_NO_DSO & framework->framework_flags) {
            flags |= PRTE_MCA_BASE_OPEN_STATIC_ONLY;
        }
    }

    /* lock all of this frameworks's variables */
    ret = prte_mca_base_var_group_find(framework->framework_project, framework->framework_name,
                                       NULL);
    prte_mca_base_var_group_set_var_flag(ret, PRTE_MCA_BASE_VAR_FLAG_SETTABLE, false);

    /* check the verbosity level and open (or close) the output */
    framework_open_output(framework);

    if (NULL != framework->framework_open) {
        ret = framework->framework_open(flags);
    } else {
        ret = prte_mca_base_framework_components_open(framework, flags);
    }

    if (PRTE_SUCCESS != ret) {
        framework->framework_refcnt--;
    } else {
        framework->framework_flags |= PRTE_MCA_BASE_FRAMEWORK_FLAG_OPEN;
    }

    return ret;
}

int prte_mca_base_framework_open_list(prte_mca_base_framework_t **frameworks,
                                      prte_mca_base_open_flag_t flags)
{
    if (NULL == frameworks) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; frameworks[i]; ++i) {
        int ret = prte_mca_base_framework_open(frameworks[i], flags);
        if (PMIX_UNLIKELY(PRTE_SUCCESS != ret && PRTE_ERR_NOT_AVAILABLE != ret)) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}

int prte_mca_base_framework_close(struct prte_mca_base_framework_t *framework)
{
    bool is_open = prte_mca_base_framework_is_open(framework);
    bool is_registered = prte_mca_base_framework_is_registered(framework);
    int ret, group_id;

    assert(NULL != framework);

    if (!(is_open || is_registered)) {
        return PRTE_SUCCESS;
    }

    assert(framework->framework_refcnt);
    if (--framework->framework_refcnt) {
        return PRTE_SUCCESS;
    }

    /* find and deregister all component groups and variables */
    group_id = prte_mca_base_var_group_find(framework->framework_project, framework->framework_name,
                                            NULL);
    if (0 <= group_id) {
        (void) prte_mca_base_var_group_deregister(group_id);
    }

    /* close the framework and all of its components */
    if (is_open) {
        if (NULL != framework->framework_close) {
            ret = framework->framework_close();
        } else {
            ret = prte_mca_base_framework_components_close(framework, NULL);
        }

        if (PRTE_SUCCESS != ret) {
            return ret;
        }
    } else {
        pmix_list_item_t *item;
        while (NULL != (item = pmix_list_remove_first(&framework->framework_components))) {
            prte_mca_base_component_list_item_t *cli;
            cli = (prte_mca_base_component_list_item_t *) item;
            prte_mca_base_component_unload(cli->cli_component, framework->framework_output);
            PMIX_RELEASE(item);
        }
        while (NULL != (item = pmix_list_remove_first(&framework->framework_failed_components))) {
            PMIX_RELEASE(item);
        }
        ret = PRTE_SUCCESS;
    }

    framework->framework_flags &= ~(PRTE_MCA_BASE_FRAMEWORK_FLAG_REGISTERED
                                    | PRTE_MCA_BASE_FRAMEWORK_FLAG_OPEN);

    PMIX_DESTRUCT(&framework->framework_components);
    PMIX_DESTRUCT(&framework->framework_failed_components);

    framework_close_output(framework);

    return ret;
}

int prte_mca_base_framework_close_list(prte_mca_base_framework_t **frameworks)
{
    if (NULL == frameworks) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; frameworks[i]; ++i) {
        int ret = prte_mca_base_framework_close(frameworks[i]);
        if (PMIX_UNLIKELY(PRTE_SUCCESS != ret)) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}
