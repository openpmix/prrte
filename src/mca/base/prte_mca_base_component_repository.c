/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "constants.h"
#include "src/class/prte_hash_table.h"
#include "src/class/prte_list.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/mca/prtedl/base/base.h"
#include "src/util/basename.h"
#include "src/util/printf.h"
#include "src/util/string_copy.h"

#if PRTE_HAVE_DL_SUPPORT

/*
 * Private types
 */
static void ri_constructor(prte_mca_base_component_repository_item_t *ri);
static void ri_destructor(prte_mca_base_component_repository_item_t *ri);
PRTE_CLASS_INSTANCE(prte_mca_base_component_repository_item_t, prte_list_item_t, ri_constructor,
                    ri_destructor);

#endif /* PRTE_HAVE_DL_SUPPORT */

static void clf_constructor(prte_object_t *obj);
static void clf_destructor(prte_object_t *obj);

PRTE_CLASS_INSTANCE(prte_mca_base_failed_component_t, prte_list_item_t, clf_constructor,
                    clf_destructor);

static void clf_constructor(prte_object_t *obj)
{
    prte_mca_base_failed_component_t *cli = (prte_mca_base_failed_component_t *) obj;
    cli->comp = NULL;
    cli->error_msg = NULL;
}

static void clf_destructor(prte_object_t *obj)
{
    prte_mca_base_failed_component_t *cli = (prte_mca_base_failed_component_t *) obj;
    cli->comp = NULL;
    if (NULL != cli->error_msg) {
        free(cli->error_msg);
        cli->error_msg = NULL;
    }
}

/*
 * Private variables
 */
static bool initialized = false;

#if PRTE_HAVE_DL_SUPPORT

static prte_hash_table_t prte_mca_base_component_repository;

/* two-level macro for stringifying a number */
#    define STRINGIFYX(x) #    x
#    define STRINGIFY(x)  STRINGIFYX(x)

static int process_repository_item(const char *filename, void *data)
{
    char name[PRTE_MCA_BASE_MAX_COMPONENT_NAME_LEN + 1];
    char type[PRTE_MCA_BASE_MAX_TYPE_NAME_LEN + 1];
    prte_mca_base_component_repository_item_t *ri;
    prte_list_t *component_list;
    char *base;
    int ret;

    base = prte_basename(filename);
    if (NULL == base) {
        return PRTE_ERROR;
    }

    /* check if the plugin has the appropriate prefix */
    if (0 != strncmp(base, "mca_", 4)) {
        free(base);
        return PRTE_SUCCESS;
    }

    /* read framework and component names. framework names may not include an _
     * but component names may */
    ret = sscanf(base,
                 "mca_%" STRINGIFY(PRTE_MCA_BASE_MAX_TYPE_NAME_LEN) "[^_]_%" STRINGIFY(
                     PRTE_MCA_BASE_MAX_COMPONENT_NAME_LEN) "s",
                 type, name);
    if (0 > ret) {
        /* does not patch the expected template. skip */
        free(base);
        return PRTE_SUCCESS;
    }

    /* lookup the associated framework list and create if it doesn't already exist */
    ret = prte_hash_table_get_value_ptr(&prte_mca_base_component_repository, type, strlen(type),
                                        (void **) &component_list);
    if (PRTE_SUCCESS != ret) {
        component_list = PRTE_NEW(prte_list_t);
        if (NULL == component_list) {
            free(base);
            /* OOM. nothing to do but fail */
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        ret = prte_hash_table_set_value_ptr(&prte_mca_base_component_repository, type, strlen(type),
                                            (void *) component_list);
        if (PRTE_SUCCESS != ret) {
            free(base);
            PRTE_RELEASE(component_list);
            return ret;
        }
    }

    /* check for duplicate components */
    PRTE_LIST_FOREACH(ri, component_list, prte_mca_base_component_repository_item_t)
    {
        if (0 == strcmp(ri->ri_name, name)) {
            /* already scanned this component */
            free(base);
            return PRTE_SUCCESS;
        }
    }

    ri = PRTE_NEW(prte_mca_base_component_repository_item_t);
    if (NULL == ri) {
        free(base);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    ri->ri_base = base;

    ri->ri_path = strdup(filename);
    if (NULL == ri->ri_path) {
        PRTE_RELEASE(ri);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    prte_string_copy(ri->ri_type, type, PRTE_MCA_BASE_MAX_TYPE_NAME_LEN);
    prte_string_copy(ri->ri_name, name, PRTE_MCA_BASE_MAX_COMPONENT_NAME_LEN);

    prte_list_append(component_list, &ri->super);

    return PRTE_SUCCESS;
}

static int file_exists(const char *filename, const char *ext)
{
    char *final;
    int ret;

    if (NULL == ext) {
        return access(filename, F_OK) == 0;
    }

    ret = prte_asprintf(&final, "%s.%s", filename, ext);
    if (0 > ret || NULL == final) {
        return 0;
    }

    ret = access(final, F_OK);
    free(final);
    return (0 == ret);
}

#endif /* PRTE_HAVE_DL_SUPPORT */

int prte_mca_base_component_repository_add(const char *path)
{
#if PRTE_HAVE_DL_SUPPORT
    char *path_to_use = NULL, *dir, *ctx;
    const char sep[] = {PRTE_ENV_SEP, '\0'};

    if (NULL == path) {
        /* nothing to do */
        return PRTE_SUCCESS;
    }

    path_to_use = strdup(path);

    dir = strtok_r(path_to_use, sep, &ctx);
    do {
        if (0 == strcmp(dir, "USER_DEFAULT") || 0 == strcmp(dir, "USR_DEFAULT")) {
            if (NULL == prte_mca_base_user_default_path) {
                continue;
            }
            dir = prte_mca_base_user_default_path;
        } else if (0 == strcmp(dir, "SYS_DEFAULT") || 0 == strcmp(dir, "SYSTEM_DEFAULT")) {
            if (NULL == prte_mca_base_system_default_path) {
                continue;
            }
            dir = prte_mca_base_system_default_path;
        }

        if (0 != prte_dl_foreachfile(dir, process_repository_item, NULL)) {
            break;
        }
    } while (NULL != (dir = strtok_r(NULL, sep, &ctx)));

    free(path_to_use);

#endif /* PRTE_HAVE_DL_SUPPORT */

    return PRTE_SUCCESS;
}

/*
 * Initialize the repository
 */
int prte_mca_base_component_repository_init(void)
{
    /* Setup internal structures */

    if (!initialized) {
#if PRTE_HAVE_DL_SUPPORT

        /* Initialize the dl framework */
        int ret = prte_mca_base_framework_open(&prte_prtedl_base_framework,
                                               PRTE_MCA_BASE_OPEN_DEFAULT);
        if (PRTE_SUCCESS != ret) {
            prte_output(0,
                        "%s %d:%s failed -- process will likely abort (open the dl framework "
                        "returned %d instead of PRTE_SUCCESS)\n",
                        __FILE__, __LINE__, __func__, ret);
            return ret;
        }
        prte_dl_base_select();

        PRTE_CONSTRUCT(&prte_mca_base_component_repository, prte_hash_table_t);
        ret = prte_hash_table_init(&prte_mca_base_component_repository, 128);
        if (PRTE_SUCCESS != ret) {
            (void) prte_mca_base_framework_close(&prte_prtedl_base_framework);
            return ret;
        }

        ret = prte_mca_base_component_repository_add(prte_mca_base_component_path);
        if (PRTE_SUCCESS != ret) {
            prte_output(0, "ERROR ON REPO ADD");
            PRTE_DESTRUCT(&prte_mca_base_component_repository);
            (void) prte_mca_base_framework_close(&prte_prtedl_base_framework);
            return ret;
        }
#endif

        initialized = true;
    }

    /* All done */

    return PRTE_SUCCESS;
}

int prte_mca_base_component_repository_get_components(prte_mca_base_framework_t *framework,
                                                      prte_list_t **framework_components)
{
    *framework_components = NULL;
#if PRTE_HAVE_DL_SUPPORT
    return prte_hash_table_get_value_ptr(&prte_mca_base_component_repository,
                                         framework->framework_name,
                                         strlen(framework->framework_name),
                                         (void **) framework_components);
#else
    return PRTE_ERR_NOT_FOUND;
#endif
}

#if PRTE_HAVE_DL_SUPPORT
static void
prte_mca_base_component_repository_release_internal(prte_mca_base_component_repository_item_t *ri)
{
    int group_id;

    group_id = prte_mca_base_var_group_find(NULL, ri->ri_type, ri->ri_name);
    if (0 <= group_id) {
        /* ensure all variables are deregistered before we dlclose the component */
        prte_mca_base_var_group_deregister(group_id);
    }

    /* Close the component (and potentially unload it from memory */
    if (ri->ri_dlhandle) {
        prte_dl_close(ri->ri_dlhandle);
        ri->ri_dlhandle = NULL;
    }
}
#endif

#if PRTE_HAVE_DL_SUPPORT
static prte_mca_base_component_repository_item_t *find_component(const char *type, const char *name)
{
    prte_mca_base_component_repository_item_t *ri;
    prte_list_t *component_list;
    int ret;

    ret = prte_hash_table_get_value_ptr(&prte_mca_base_component_repository, type, strlen(type),
                                        (void **) &component_list);
    if (PRTE_SUCCESS != ret) {
        /* component does not exist in the repository */
        return NULL;
    }

    PRTE_LIST_FOREACH(ri, component_list, prte_mca_base_component_repository_item_t)
    {
        if (0 == strcmp(ri->ri_name, name)) {
            return ri;
        }
    }

    return NULL;
}
#endif

void prte_mca_base_component_repository_release(const prte_mca_base_component_t *component)
{
#if PRTE_HAVE_DL_SUPPORT
    prte_mca_base_component_repository_item_t *ri;

    ri = find_component(component->mca_type_name, component->mca_component_name);
    if (NULL != ri && !(--ri->ri_refcnt)) {
        prte_mca_base_component_repository_release_internal(ri);
    }
#endif
}

int prte_mca_base_component_repository_retain_component(const char *type, const char *name)
{
#if PRTE_HAVE_DL_SUPPORT
    prte_mca_base_component_repository_item_t *ri = find_component(type, name);

    if (NULL != ri) {
        ++ri->ri_refcnt;
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_NOT_FOUND;
#else
    return PRTE_ERR_NOT_SUPPORTED;
#endif
}

int prte_mca_base_component_repository_open(prte_mca_base_framework_t *framework,
                                            prte_mca_base_component_repository_item_t *ri)
{
#if PRTE_HAVE_DL_SUPPORT
    prte_mca_base_component_t *component_struct;
    prte_mca_base_component_list_item_t *mitem = NULL;
    char *struct_name = NULL;
    int vl, ret;

    prte_output_verbose(PRTE_MCA_BASE_VERBOSE_INFO, 0,
                        "mca_base_component_repository_open: examining dynamic "
                        "%s MCA component \"%s\" at path %s",
                        ri->ri_type, ri->ri_name, ri->ri_path);

    vl = prte_mca_base_component_show_load_errors ? PRTE_MCA_BASE_VERBOSE_ERROR
                                                  : PRTE_MCA_BASE_VERBOSE_INFO;

    /* Ensure that this component is not already loaded (should only happen
       if it was statically loaded).  It's an error if it's already
       loaded because we're evaluating this file -- not this component.
       Hence, returning PRTE_ERR_PARAM indicates that the *file* failed
       to load, not the component. */

    PRTE_LIST_FOREACH(mitem, &framework->framework_components, prte_mca_base_component_list_item_t)
    {
        if (0 == strcmp(mitem->cli_component->mca_component_name, ri->ri_name)) {
            prte_output_verbose(PRTE_MCA_BASE_VERBOSE_INFO, 0,
                                "mca_base_component_repository_open: already loaded (ignored)");
            return PRTE_ERR_BAD_PARAM;
        }
    }

    /* silence coverity issue (invalid free) */
    mitem = NULL;

    if (NULL != ri->ri_dlhandle) {
        prte_output_verbose(
            PRTE_MCA_BASE_VERBOSE_INFO, 0,
            "mca_base_component_repository_open: already loaded. returning cached component");
        mitem = PRTE_NEW(prte_mca_base_component_list_item_t);
        if (NULL == mitem) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        mitem->cli_component = ri->ri_component_struct;
        prte_list_append(&framework->framework_components, &mitem->super);

        return PRTE_SUCCESS;
    }

    if (0 != strcmp(ri->ri_type, framework->framework_name)) {
        /* shouldn't happen. attempting to open a component belonging to
         * another framework. if this happens it is likely a MCA base
         * bug so assert */
        assert(0);
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* Now try to load the component */

    char *err_msg = NULL;
    if (PRTE_SUCCESS != prte_dl_open(ri->ri_path, true, false, &ri->ri_dlhandle, &err_msg)) {
        if (NULL == err_msg) {
            err_msg = strdup("prte_dl_open() error message was NULL!");
        } else if (file_exists(ri->ri_path, "lo") || file_exists(ri->ri_path, "so")
                   || file_exists(ri->ri_path, "dylib") || file_exists(ri->ri_path, "dll")) {
            /* Because libltdl erroneously says "file not found" for any
             * type of error -- which is especially misleading when the file
             * is actually there but cannot be opened for some other reason
             * (e.g., missing symbol) -- do some simple huersitics and if
             * the file [probably] does exist, print a slightly better error
             * message. */
            err_msg = strdup(
                "perhaps a missing symbol, or compiled for a different version of PRRTE?");
        }
        prte_output_verbose(
            vl, 0, "prte_mca_base_component_repository_open: unable to open %s: %s (ignored)",
            ri->ri_base, err_msg);

        if (prte_mca_base_component_track_load_errors) {
            prte_mca_base_failed_component_t *f_comp = PRTE_NEW(prte_mca_base_failed_component_t);
            f_comp->comp = ri;
            if (0 > asprintf(&(f_comp->error_msg), "%s", err_msg)) {
                PRTE_RELEASE(f_comp);
                free(err_msg);
                return PRTE_ERR_BAD_PARAM;
            }
            prte_list_append(&framework->framework_failed_components, &f_comp->super);
        }

        free(err_msg);
        return PRTE_ERR_BAD_PARAM;
    }

    /* Successfully opened the component; now find the public struct.
       Malloc out enough space for it. */

    do {
        ret = prte_asprintf(&struct_name, "prte_%s_%s_component", ri->ri_type, ri->ri_name);
        if (0 > ret) {
            ret = PRTE_ERR_OUT_OF_RESOURCE;
            break;
        }

        mitem = PRTE_NEW(prte_mca_base_component_list_item_t);
        if (NULL == mitem) {
            ret = PRTE_ERR_OUT_OF_RESOURCE;
            break;
        }

        err_msg = NULL;
        ret = prte_dl_lookup(ri->ri_dlhandle, struct_name, (void **) &component_struct, &err_msg);
        if (PRTE_SUCCESS != ret || NULL == component_struct) {
            if (NULL == err_msg) {
                err_msg = "prte_dl_loookup() error message was NULL!";
            }
            prte_output_verbose(
                vl, 0,
                "mca_base_component_repository_open: \"%s\" does not appear to be a valid "
                "%s MCA dynamic component (ignored): %s. ret %d",
                ri->ri_base, ri->ri_type, err_msg, ret);

            ret = PRTE_ERR_BAD_PARAM;
            break;
        }

        /* done with the structure name */
        free(struct_name);
        struct_name = NULL;

        /* We found the public struct.  Make sure its MCA major.minor
           version is the same as ours. TODO -- add checks for project version (from framework) */
        if (!(PRTE_MCA_BASE_VERSION_MAJOR == component_struct->mca_major_version
              && PRTE_MCA_BASE_VERSION_MINOR == component_struct->mca_minor_version)) {
            prte_output_verbose(
                vl, 0,
                "mca_base_component_repository_open: %s \"%s\" uses an MCA interface that is "
                "not recognized (component MCA v%d.%d.%d != supported MCA v%d.%d.%d) -- ignored",
                ri->ri_type, ri->ri_path, component_struct->mca_major_version,
                component_struct->mca_minor_version, component_struct->mca_release_version,
                PRTE_MCA_BASE_VERSION_MAJOR, PRTE_MCA_BASE_VERSION_MINOR,
                PRTE_MCA_BASE_VERSION_RELEASE);
            ret = PRTE_ERR_BAD_PARAM;
            break;
        }

        /* Also check that the component struct framework and component
           names match the expected names from the filename */
        if (0 != strcmp(component_struct->mca_type_name, ri->ri_type)
            || 0 != strcmp(component_struct->mca_component_name, ri->ri_name)) {
            prte_output_verbose(
                vl, 0,
                "Component file data does not match filename: %s (%s / %s) != %s %s -- ignored",
                ri->ri_path, ri->ri_type, ri->ri_name, component_struct->mca_type_name,
                component_struct->mca_component_name);
            ret = PRTE_ERR_BAD_PARAM;
            break;
        }

        /* Alles gut.  Save the component struct, and register this
           component to be closed later. */

        ri->ri_component_struct = mitem->cli_component = component_struct;
        ri->ri_refcnt = 1;
        prte_list_append(&framework->framework_components, &mitem->super);

        prte_output_verbose(PRTE_MCA_BASE_VERBOSE_INFO, 0,
                            "mca_base_component_repository_open: opened dynamic %s MCA "
                            "component \"%s\"",
                            ri->ri_type, ri->ri_name);

        return PRTE_SUCCESS;
    } while (0);

    if (mitem) {
        PRTE_RELEASE(mitem);
    }

    if (struct_name) {
        free(struct_name);
    }

    prte_dl_close(ri->ri_dlhandle);
    ri->ri_dlhandle = NULL;

    return ret;
#else

    /* no dlopen support */
    return PRTE_ERR_NOT_SUPPORTED;
#endif
}

/*
 * Finalize the repository -- close everything that's still open.
 */
void prte_mca_base_component_repository_finalize(void)
{
    if (!initialized) {
        return;
    }

    initialized = false;

#if PRTE_HAVE_DL_SUPPORT
    prte_list_t *component_list;
    void *node, *key;
    size_t key_size;
    int ret;

    ret = prte_hash_table_get_first_key_ptr(&prte_mca_base_component_repository, &key, &key_size,
                                            (void **) &component_list, &node);
    while (PRTE_SUCCESS == ret) {
        PRTE_LIST_RELEASE(component_list);
        ret = prte_hash_table_get_next_key_ptr(&prte_mca_base_component_repository, &key, &key_size,
                                               (void **) &component_list, node, &node);
    }

    (void) prte_mca_base_framework_close(&prte_prtedl_base_framework);
    PRTE_DESTRUCT(&prte_mca_base_component_repository);
#endif
}

#if PRTE_HAVE_DL_SUPPORT

/*
 * Basic sentinel values, and construct the inner list
 */
static void ri_constructor(prte_mca_base_component_repository_item_t *ri)
{
    memset(ri->ri_type, 0, sizeof(ri->ri_type));
    ri->ri_dlhandle = NULL;
    ri->ri_component_struct = NULL;
    ri->ri_path = NULL;
}

/*
 * Close a component
 */
static void ri_destructor(prte_mca_base_component_repository_item_t *ri)
{
    /* dlclose the component if it is still open */
    prte_mca_base_component_repository_release_internal(ri);

    /* It should be obvious, but I'll state it anyway because it bit me
       during debugging: after the dlclose(), the prte_mca_base_component_t
       pointer is no longer valid because it has [potentially] been
       unloaded from memory.  So don't try to use it.  :-) */

    if (ri->ri_path) {
        free(ri->ri_path);
    }

    if (ri->ri_base) {
        free(ri->ri_base);
    }
}

#endif /* PRTE_HAVE_DL_SUPPORT */
