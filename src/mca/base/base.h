/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_MCA_BASE_H
#define PRTE_MCA_BASE_H

#include "prte_config.h"

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"

/*
 * These units are large enough to warrant their own .h files
 */
#include "src/mca/base/prte_mca_base_framework.h"
#include "src/mca/base/prte_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

BEGIN_C_DECLS

/*
 * Structure for making plain lists of components
 */
struct prte_mca_base_component_list_item_t {
    pmix_list_item_t super;
    const prte_mca_base_component_t *cli_component;
};
typedef struct prte_mca_base_component_list_item_t prte_mca_base_component_list_item_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_mca_base_component_list_item_t);

/*
 * Structure for making priority lists of components
 */
struct prte_mca_base_component_priority_list_item_t {
    prte_mca_base_component_list_item_t super;
    int cpli_priority;
};
typedef struct prte_mca_base_component_priority_list_item_t
    prte_mca_base_component_priority_list_item_t;

PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_mca_base_component_priority_list_item_t);

/*
 * Public variables
 */
PRTE_EXPORT extern char *prte_mca_base_component_path;
PRTE_EXPORT extern bool prte_mca_base_component_show_load_errors;
PRTE_EXPORT extern bool prte_mca_base_component_track_load_errors;
PRTE_EXPORT extern bool prte_mca_base_component_disable_dlopen;
PRTE_EXPORT extern char *prte_mca_base_system_default_path;
PRTE_EXPORT extern char *prte_mca_base_user_default_path;

/*
 * Standard verbosity levels
 */
enum {
    /** total silence */
    PRTE_MCA_BASE_VERBOSE_NONE = -1,
    /** only errors are printed */
    PRTE_MCA_BASE_VERBOSE_ERROR = 0,
    /** emit messages about component selection, open, and unloading */
    PRTE_MCA_BASE_VERBOSE_COMPONENT = 10,
    /** also emit warnings */
    PRTE_MCA_BASE_VERBOSE_WARN = 20,
    /** also emit general, user-relevant information, such as rationale as to why certain choices
     * or code paths were taken, information gleaned from probing the local system, etc. */
    PRTE_MCA_BASE_VERBOSE_INFO = 40,
    /** also emit relevant tracing information (e.g., which functions were invoked /
     * call stack entry/exit info) */
    PRTE_MCA_BASE_VERBOSE_TRACE = 60,
    /** also emit PRTE-developer-level (i.e,. highly detailed) information */
    PRTE_MCA_BASE_VERBOSE_DEBUG = 80,
    /** also output anything else that might be useful */
    PRTE_MCA_BASE_VERBOSE_MAX = 100,
};

/*
 * Public functions
 */

/**
 * First function called in the MCA.
 *
 * @return PRTE_SUCCESS Upon success
 * @return PRTE_ERROR Upon failure
 *
 * This function starts up the entire MCA.  It initializes a bunch
 * of built-in MCA parameters, and initialized the MCA component
 * repository.
 *
 * It must be the first MCA function invoked.  It is normally
 * invoked during the initialization stage and specifically
 * invoked in the special case of the *_info command.
 */
PRTE_EXPORT int prte_mca_base_open(void);

/**
 * Last function called in the MCA
 *
 * @return PRTE_SUCCESS Upon success
 * @return PRTE_ERROR Upon failure
 *
 * This function closes down the entire MCA.  It clears all MCA
 * parameters and closes down the MCA component respository.
 *
 * It must be the last MCA function invoked.  It is normally invoked
 * during the finalize stage.
 */
PRTE_EXPORT void prte_mca_base_close(void);

/**
 * A generic select function
 *
 */
PRTE_EXPORT int prte_mca_base_select(const char *type_name, int output_id,
                                     pmix_list_t *components_available,
                                     prte_mca_base_module_t **best_module,
                                     prte_mca_base_component_t **best_component, int *priority_out);

/**
 * A function for component query functions to discover if they have
 * been explicitly required to or requested to be selected.
 *
 * exclusive: If the specified component is the only component that is
 *            available for selection.
 *
 */
PRTE_EXPORT int prte_mca_base_is_component_required(pmix_list_t *components_available,
                                                    prte_mca_base_component_t *component,
                                                    bool exclusive, bool *is_required);

/* prte_mca_base_component_compare.c */

PRTE_EXPORT int
prte_mca_base_component_compare_priority(prte_mca_base_component_priority_list_item_t *a,
                                         prte_mca_base_component_priority_list_item_t *b);
PRTE_EXPORT int prte_mca_base_component_compare(const prte_mca_base_component_t *a,
                                                const prte_mca_base_component_t *b);
PRTE_EXPORT int prte_mca_base_component_compatible(const prte_mca_base_component_t *a,
                                                   const prte_mca_base_component_t *b);
PRTE_EXPORT char *prte_mca_base_component_to_string(const prte_mca_base_component_t *a);

/* prte_mca_base_component_find.c */

PRTE_EXPORT int prte_mca_base_component_find(const char *directory,
                                             prte_mca_base_framework_t *framework,
                                             bool ignore_requested, bool open_dso_components);

/**
 * Parse the requested component string and return an pmix_argv of the requested
 * (or not requested) components.
 */
int prte_mca_base_component_parse_requested(const char *requested, bool *include_mode,
                                            char ***requested_component_names);

/**
 * Filter a list of components based on a comma-delimted list of names and/or
 * a set of meta-data flags.
 *
 * @param[in,out] components List of components to filter
 * @param[in] output_id Output id to write to for error/warning/debug messages
 * @param[in] filter_names Comma delimited list of components to use. Negate with ^.
 * May be NULL.
 * @param[in] filter_flags Metadata flags components are required to have set (CR ready)
 *
 * @returns PRTE_SUCCESS On success
 * @returns PRTE_ERR_NOT_FOUND If some component in {filter_names} is not found in
 * {components}. Does not apply to negated filters.
 * @returns prte error code On other error.
 *
 * This function closes and releases any components that do not match the filter_name and
 * filter flags.
 */
PRTE_EXPORT int prte_mca_base_components_filter(prte_mca_base_framework_t *framework,
                                                uint32_t filter_flags);

/* Safely release some memory allocated by prte_mca_base_component_find()
   (i.e., is safe to call even if you never called
   prte_mca_base_component_find()). */
PRTE_EXPORT int prte_mca_base_component_find_finalize(void);

/* prte_mca_base_components_register.c */
PRTE_EXPORT int
prte_mca_base_framework_components_register(struct prte_mca_base_framework_t *framework,
                                            prte_mca_base_register_flag_t flags);

/* prte_mca_base_components_open.c */
PRTE_EXPORT int prte_mca_base_framework_components_open(struct prte_mca_base_framework_t *framework,
                                                        prte_mca_base_open_flag_t flags);

PRTE_EXPORT int prte_mca_base_components_open(const char *type_name, int output_id,
                                              const prte_mca_base_component_t **static_components,
                                              pmix_list_t *components_available,
                                              bool open_dso_components);

/* prte_mca_base_components_close.c */
/**
 * Close and release a component.
 *
 * @param[in] component Component to close
 * @param[in] output_id Output id for debugging output
 *
 * After calling this function the component may no longer be used.
 */
PRTE_EXPORT void prte_mca_base_component_close(const prte_mca_base_component_t *component,
                                               int output_id);

/**
 * Release a component without closing it.
 * @param[in] component Component to close
 * @param[in] output_id Output id for debugging output
 *
 * After calling this function the component may no longer be used.
 */
void prte_mca_base_component_unload(const prte_mca_base_component_t *component, int output_id);

PRTE_EXPORT int prte_mca_base_components_close(int output_id, pmix_list_t *components_available,
                                               const prte_mca_base_component_t *skip);

PRTE_EXPORT int
prte_mca_base_framework_components_close(struct prte_mca_base_framework_t *framework,
                                         const prte_mca_base_component_t *skip);

END_C_DECLS

#endif /* MCA_BASE_H */
