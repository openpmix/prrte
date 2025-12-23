/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      UT-Battelle, LLC. All rights reserved.
 *
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int map_colocate(prte_job_t *jdata,
                        bool daemons, bool pernode,
                        pmix_data_array_t *darray,
                        uint16_t procs_per_target,
                        prte_rmaps_options_t *options);

static void inherit_env_directives(prte_job_t *jdata,
                                   prte_job_t *parent,
                                   pmix_proc_t *proxy);

void prte_rmaps_base_map_job(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    prte_node_t *node;
    pmix_proc_t *pptr;
    int rc = PRTE_SUCCESS;
    int n;
    bool did_map, pernode = false;
    prte_rmaps_base_selected_module_t *mod;
    prte_job_t *parent = NULL;
    prte_app_context_t *app;
    bool inherit = false;
    pmix_proc_t *nptr = NULL, *target_proc;
    char *tmp, **ck, **env;
    uint16_t u16 = 0, procs_per_target = 0;
    uint16_t *u16ptr = &u16;
    bool colocate_daemons = false;
    bool colocate = false;
    prte_schizo_base_module_t *schizo;
    prte_rmaps_options_t options;
    pmix_data_array_t *darray = NULL;
    pmix_list_t nodes;
    int slots, len;
    bool flag, *fptr;

    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    // init options
    memset(&options, 0, sizeof(prte_rmaps_options_t));
    options.stream = prte_rmaps_base_framework.framework_output;
    options.verbosity = 5;  // usual value for base-level functions
    // set and check convenience vars
    jdata = caddy->jdata;
    schizo = (prte_schizo_base_module_t*)jdata->schizo;
    if (NULL == schizo) {
        pmix_show_help("help-prte-rmaps-base.txt", "missing-personality", true,
                       PRTE_JOBID_PRINT(jdata->nspace));
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }
    if (NULL == jdata->map) {
        jdata->map = PMIX_NEW(prte_job_map_t);
    }
    jdata->state = PRTE_JOB_STATE_MAP;
    fptr = &flag;

    /* check and set some general options */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        options.donotlaunch = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        options.dobind = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_BINDING_LIMIT, (void**) &u16ptr, PMIX_UINT16)) {
        options.limit = u16;
        // reset any prior counters
        prte_hwloc_base_reset_counters();
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /*
     * Check for Colaunch
     */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_NODE, (void **) &u16ptr, PMIX_UINT16)) {
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_DEBUG_DAEMONS_PER_NODE value %u == 0\n", procs_per_target);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = true;
        colocate_daemons = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DEBUG_DAEMONS_PER_PROC, (void **) &u16ptr, PMIX_UINT16)) {
        if (procs_per_target > 0) {
            pmix_output(0, "Error: Both PRTE_JOB_DEBUG_DAEMONS_PER_PROC and "
                           "PRTE_JOB_DEBUG_DAEMONS_PER_NODE provided.");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_DEBUG_DAEMONS_PER_PROC value %u == 0\n", procs_per_target);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = false;
        colocate_daemons = true;
    }
    if (colocate_daemons) {
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DEBUG_TARGET, (void **) &target_proc, PMIX_PROC)) {
            pmix_output(0, "Error: PRTE_JOB_DEBUG_DAEMONS_PER_PROC/NODE provided without a Debug Target\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* store the target as a pmix_data_array_t */
        PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_PROC);
        pptr = (pmix_proc_t*)darray->array;
        PMIX_XFER_PROCID(&pptr[0], target_proc);
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_PROCS, (void**)&darray, PMIX_DATA_ARRAY)) {
        if (colocate_daemons) {
            pmix_output(0, "Error: Both colocate daemons and colocate procs were provided\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        if (NULL == darray) {
            pmix_output(0, "Error: Colocate failed to provide procs\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        colocate = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_NPERNODE, (void **) &u16ptr, PMIX_UINT16)) {
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_COLOCATE_NUM_PROC WITH ZERO PROCS/TARGET\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = true;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_COLOCATE_NPERPROC, (void **) &u16ptr, PMIX_UINT16)) {
        if (procs_per_target > 0) {
            pmix_output(0, "Error: Both PRTE_JOB_COLOCATE_NUM_PROC and "
                        "PRTE_JOB_COLOCATE_NUM_NODE provided.");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        procs_per_target = u16;
        if (procs_per_target == 0) {
            pmix_output(0, "Error: PRTE_JOB_COLOCATE_NUM_PROC WITH ZERO PROCS/TARGET\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        pernode = false;
    }

    if (colocate || colocate_daemons) {
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_COLOCATE);
        goto ranking;
    }

    /* if this is a dynamic job launch and they didn't explicitly
     * request inheritance, then don't inherit the launch directives */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &nptr, PMIX_PROC)) {
        if (NULL == nptr) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* if the launch proxy is me, then this is the initial launch from
         * a proxy scenario, so we don't really have a parent */
        if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, nptr->nspace)) {
            parent = NULL;
            /* we do allow inheritance of the defaults */
            inherit = true;
        } else if (NULL != (parent = prte_get_job_data_object(nptr->nspace))) {
            if (PRTE_FLAG_TEST(parent, PRTE_JOB_FLAG_TOOL)) {
                // we don't inherit anything from tools as they were not
                // mapped by us
                inherit = false;
                parent = NULL;

            } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_INHERIT, NULL, PMIX_BOOL)) {
                inherit = true;
                // if they didn't specifically direct it not inherit, then pass this on to the child
                if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_NOINHERIT, NULL, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_INHERIT, NULL, PMIX_BOOL);
                }

            } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_NOINHERIT, NULL, PMIX_BOOL)) {
                inherit = false;
                parent = NULL;

            } else {
                inherit = prte_rmaps_base.inherit;
            }
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps: dynamic job %s %s inherit launch directives - parent %s",
                                PRTE_JOBID_PRINT(jdata->nspace), inherit ? "will" : "will not",
                                (NULL == parent) ? "N/A" : PRTE_JOBID_PRINT((parent->nspace)));
        } else {
            inherit = true;
        }
    } else {
        /* initial launch always takes on default MCA params for non-specified policies */
        inherit = true;
    }

    if (inherit) {
        if (NULL != parent) {
            /* if not already assigned, inherit the parent's ppr */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, NULL, PMIX_STRING)) {
                /* get the parent job's ppr, if it had one */
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_PPR, (void **) &tmp, PMIX_STRING)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, tmp, PMIX_STRING);
                    free(tmp);
                }
            }
            /* if not already assigned, inherit the parent's pes/proc */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16)) {
                /* get the parent job's pes/proc, if it had one */
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr, PMIX_UINT16)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, u16ptr, PMIX_UINT16);
                } else if (0 < prte_rmaps_base.default_pes) {
                    u16 = prte_rmaps_base.default_pes;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, u16ptr, PMIX_UINT16);
                }
            }
            /* if not already assigned, inherit the parent's cpu designation */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL) &&
                !prte_get_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                /* get the parent job's designation, if it had one */
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                } else {
                    /* default */
                    if (prte_rmaps_base.hwthread_cpus) {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                    } else {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                    }
                }
            }
            /* if not already assigned, inherit the parent's GPU support directive */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_GPU_SUPPORT, NULL, PMIX_BOOL)) {
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_GPU_SUPPORT, (void **) &fptr, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_GPU_SUPPORT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                }
            }
            /* if not already assigned, inherit the parent's output directives */
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, NULL, PMIX_BOOL)) {
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_TAG_OUTPUT, (void **) &fptr, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                }
            }
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, NULL, PMIX_BOOL)) {
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, (void **) &fptr, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                }
            }
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, NULL, PMIX_BOOL)) {
                if (prte_get_attribute(&parent->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, (void **) &fptr, PMIX_BOOL)) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, PRTE_ATTR_GLOBAL, fptr, PMIX_BOOL);
                }
            }

            // copy over any env directives, but do not overwrite anything already specified
            inherit_env_directives(jdata, parent, nptr);
        } else {
            if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL) &&
                !prte_get_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                /* inherit the base defaults */
                if (prte_rmaps_base.hwthread_cpus) {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                } else {
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                }
            }
        }
    }
    if (NULL != nptr) {
        PMIX_PROC_RELEASE(nptr);
    }

    /* we always inherit a parent's oversubscribe flag unless the job assigned it */
    if (NULL != parent &&
        !(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(parent->map->mapping)) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }

    // forward the environment if requested to do so
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, (void **) &fptr, PMIX_BOOL)) {
        if (flag) {
            // forward the environment
            for (n = 0; n < jdata->apps->size; n++) {
                app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
                if (NULL == app) {
                    continue;
                }
                env = pmix_environ_merge(prte_launch_environ, app->env);
                PMIX_ARGV_FREE_COMPAT(app->env);
                app->env = env;
            }
        }
    } else if (NULL != parent) {
        /* we always inherit a parent's fwd environment directive unless the job assigned it */
        if (prte_get_attribute(&parent->attributes, PRTE_JOB_FWD_ENVIRONMENT, (void **) &fptr, PMIX_BOOL)) {
            if (flag) {
                // update the child's flag so any subsequent children can inherit it
                prte_set_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
                // forward the environment
                for (n = 0; n < jdata->apps->size; n++) {
                    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
                    if (NULL == app) {
                        continue;
                    }
                    env = pmix_environ_merge(prte_launch_environ, app->env);
                    PMIX_ARGV_FREE_COMPAT(app->env);
                    app->env = env;
                }
            }
        }
    }

    /* set some convenience params */
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void**)&options.cpuset, PMIX_STRING);
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr, PMIX_UINT16)) {
        options.cpus_per_rank = u16;
    } else {
        options.cpus_per_rank = 1;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        options.use_hwthreads = true;
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_PROCESSORS, (void*)&tmp, PMIX_STRING)) {
        prte_ras_base_display_cpus(jdata, tmp);
        free(tmp);
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: setting mapping policies for job %s inherit %s hwtcpus %s",
                        PRTE_JOBID_PRINT(jdata->nspace),
                        inherit ? "TRUE" : "FALSE",
                        options.use_hwthreads ? "TRUE" : "FALSE");

    /* set the default mapping policy IFF it wasn't provided */
    if (!PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
        did_map = false;
        if (inherit) {
            if (NULL != parent) {
                jdata->map->mapping = parent->map->mapping;
                did_map = true;
            } else if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps mapping given by MCA param");
                jdata->map->mapping = prte_rmaps_base.mapping;
                if (0 < prte_rmaps_base.default_pes) {
                    u16 = prte_rmaps_base.default_pes;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, PRTE_ATTR_GLOBAL, u16ptr, PMIX_UINT16);
                    options.cpus_per_rank = u16;
                }
                if (PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
                    tmp = strchr(prte_rmaps_base.default_mapping_policy, ':');
                    ++tmp;
                    prte_set_attribute(&jdata->attributes, PRTE_JOB_PPR,
                                       PRTE_ATTR_GLOBAL, tmp, PMIX_STRING);
                }
                did_map = true;
            }
        }
        if (!did_map) {
            // let the job's personality set the default mapping behavior
            if (NULL != schizo->set_default_mapping) {
                rc = schizo->set_default_mapping(jdata, &options);
            } else {
                rc = prte_rmaps_base_set_default_mapping(jdata, &options);
            }
            if (PRTE_SUCCESS != rc) {
                // the error message should have been printed
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
        }
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, (void **) &tmp, PMIX_STRING)) {
        ck = PMIX_ARGV_SPLIT_COMPAT(tmp, ':');
        if (2 != PMIX_ARGV_COUNT_COMPAT(ck)) {
            /* must provide a specification */
            pmix_show_help("help-prte-rmaps-ppr.txt", "invalid-ppr", true, tmp);
            PMIX_ARGV_FREE_COMPAT(ck);
            free(tmp);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* compute the #procs per resource */
        options.pprn = strtoul(ck[0], NULL, 10);
        len = strlen(ck[1]);
        if (0 == strncasecmp(ck[1], "node", len)) {
            options.maptype = HWLOC_OBJ_MACHINE;
            options.mapdepth = PRTE_BIND_TO_NONE;
        } else if (0 == strncasecmp(ck[1], "hwthread", len) ||
                   0 == strncasecmp(ck[1], "thread", len)) {
            options.maptype = HWLOC_OBJ_PU;
            options.mapdepth = PRTE_BIND_TO_HWTHREAD;
        } else if (0 == strncasecmp(ck[1], "core", len)) {
            options.maptype = HWLOC_OBJ_CORE;
            options.mapdepth = PRTE_BIND_TO_CORE;
        } else if (0 == strncasecmp(ck[1], "package", len) ||
                   0 == strncasecmp(ck[1], "skt", len) ||
                   0 == strncasecmp(ck[1], "socket", len)) {
            options.maptype = HWLOC_OBJ_PACKAGE;
            options.mapdepth = PRTE_BIND_TO_PACKAGE;
        } else if (0 == strncasecmp(ck[1], "numa", len) ||
                   0 == strncasecmp(ck[1], "nm", len)) {
            options.maptype = HWLOC_OBJ_NUMANODE;
            options.mapdepth = PRTE_BIND_TO_NUMA;
        } else if (0 == strncasecmp(ck[1], "l1cache", len)) {
            options.maptype = HWLOC_OBJ_L1CACHE;
            options.mapdepth = PRTE_BIND_TO_L1CACHE;
        } else if (0 == strncasecmp(ck[1], "l2cache", len)) {
            options.maptype = HWLOC_OBJ_L2CACHE;
            options.mapdepth = PRTE_BIND_TO_L2CACHE;
        } else if (0 == strncasecmp(ck[1], "l3cache", len)) {
            options.maptype = HWLOC_OBJ_L3CACHE;
            options.mapdepth = PRTE_BIND_TO_L3CACHE;
        } else {
            /* unknown spec */
            pmix_show_help("help-prte-rmaps-ppr.txt", "unrecognized-ppr-option", true,
                           ck[1], tmp);
            free(tmp);
            PMIX_ARGV_FREE_COMPAT(ck);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        free(tmp);
        PMIX_ARGV_FREE_COMPAT(ck);
    }

    /* add up all the expected procs */
    for (n = 0; n < jdata->apps->size; n++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
        if (NULL == app ) {
            continue;
        }
        if (0 < app->num_procs) {
            options.nprocs += app->num_procs;
            continue;
        }
        PRTE_FLAG_SET(app, PRTE_APP_FLAG_COMPUTED);

        if (PRTE_MAPPING_SEQ == PRTE_GET_MAPPING_POLICY(jdata->map->mapping) ||
            PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(jdata->map->mapping) ||
            PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
            // these mappers compute their #procs as they go
            continue;
        }

       if (NULL != options.cpuset) {
            ck = PMIX_ARGV_SPLIT_COMPAT(options.cpuset, ',');
            app->num_procs = PMIX_ARGV_COUNT_COMPAT(ck);
            PMIX_ARGV_FREE_COMPAT(ck);
        } else {
            if (1 < options.cpus_per_rank) {
                /*
                 * get the target nodes for this app - the base function
                 * will take any host or hostfile directive into account
                 */
                PMIX_CONSTRUCT(&nodes, pmix_list_t);
                rc = prte_rmaps_base_get_target_nodes(&nodes, &slots,
                                                      jdata, app, jdata->map->mapping,
                                                      true, true, false);
                if (PRTE_SUCCESS != rc) {
                    PMIX_LIST_DESTRUCT(&nodes);
                    jdata->exit_code = rc;
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                    goto cleanup;
                }
                // compute the number of cpus on each node
                len = 0;
                PMIX_LIST_FOREACH (node, &nodes, prte_node_t) {
                    if (options.use_hwthreads) {
                        len += prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                  HWLOC_OBJ_PU) / options.cpus_per_rank;
                    } else {
                        len += prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                  HWLOC_OBJ_CORE) / options.cpus_per_rank;
                    }
                }
                PMIX_LIST_DESTRUCT(&nodes);
                app->num_procs = len;
                // ensure we always wind up with at least one proc
                if (0 == app->num_procs) {
                    app->num_procs = 1;
                } else if (slots < app->num_procs) {
                    app->num_procs = slots;
                }
            } else {
                app->num_procs = slots;
            }
        }
        options.nprocs += app->num_procs;
    }

    /* check for oversubscribe directives */
    if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                   & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }
    if (!(PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        options.oversubscribe = true;
    }

    /* check for no-use-local directive */
    if (prte_ras_base.launch_orted_on_hn) {
        /* must override any setting */
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
    } else if (!(PRTE_MAPPING_LOCAL_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (inherit
            && (PRTE_MAPPING_NO_USE_LOCAL & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
        }
    }

ranking:
    options.map = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
    if (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        options.mapspan = true;
    }
    if (PRTE_MAPPING_ORDERED & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        options.ordered = true;
    }

    switch (options.map) {
        case PRTE_MAPPING_BYNODE:
        case PRTE_MAPPING_BYSLOT:
        case PRTE_MAPPING_BYDIST:
        case PRTE_MAPPING_PELIST:
        case PRTE_MAPPING_COLOCATE:
            options.mapdepth = PRTE_BIND_TO_NONE;
            options.maptype = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_MAPPING_BYUSER:
        case PRTE_MAPPING_SEQ:
            options.mapdepth = PRTE_BIND_TO_NONE;
            options.userranked = true;
            options.maptype = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_MAPPING_BYNUMA:
            options.mapdepth = PRTE_BIND_TO_NUMA;
            options.maptype = HWLOC_OBJ_NUMANODE;
            break;
        case PRTE_MAPPING_BYPACKAGE:
            options.mapdepth = PRTE_BIND_TO_PACKAGE;
            options.maptype = HWLOC_OBJ_PACKAGE;
            break;
        case PRTE_MAPPING_BYL3CACHE:
            options.mapdepth = PRTE_BIND_TO_L3CACHE;
            options.maptype = HWLOC_OBJ_L3CACHE;
            break;
        case PRTE_MAPPING_BYL2CACHE:
            options.mapdepth = PRTE_BIND_TO_L2CACHE;
            options.maptype = HWLOC_OBJ_L2CACHE;
            break;
        case PRTE_MAPPING_BYL1CACHE:
            options.mapdepth = PRTE_BIND_TO_L1CACHE;
            options.maptype = HWLOC_OBJ_L1CACHE;
            break;
        case PRTE_MAPPING_BYCORE:
            if (1 < options.cpus_per_rank &&
                !options.use_hwthreads) {
                /* we cannot support this operation as there is only one
                 * cpu in a core */
                pmix_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true,
                               options.cpus_per_rank, 1,
                               prte_rmaps_base_print_mapping(options.map));
                jdata->exit_code = PRTE_ERR_SILENT;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            options.mapdepth = PRTE_BIND_TO_CORE;
            options.maptype = HWLOC_OBJ_CORE;
            break;
        case PRTE_MAPPING_BYHWTHREAD:
            if (1 < options.cpus_per_rank) {
                /* we cannot support this operation as there is only one
                 * cpu in a core */
                pmix_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true,
                               options.cpus_per_rank, 1,
                               prte_rmaps_base_print_mapping(options.map));
                jdata->exit_code = PRTE_ERR_SILENT;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            options.mapdepth = PRTE_BIND_TO_HWTHREAD;
            options.maptype = HWLOC_OBJ_PU;
            break;
        case PRTE_MAPPING_PPR:
            break;
        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
    }

    if (options.userranked) {
        /* must rank by user */
        PRTE_SET_RANKING_POLICY(jdata->map->ranking, PRTE_RANKING_BYUSER);
    } else {
        /* set the default ranking policy IFF it wasn't provided */
        if (!PRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
            did_map = false;
            if (inherit) {
                if (NULL != parent) {
                    jdata->map->ranking = parent->map->ranking;
                    did_map = true;
                } else if (PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(prte_rmaps_base.ranking)) {
                    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps ranking given by MCA param");
                    jdata->map->ranking = prte_rmaps_base.ranking;
                    did_map = true;
                }
            }
            if (!did_map) {
                // let the job's personality set the default ranking behavior
                if (NULL != schizo->set_default_ranking) {
                    rc = schizo->set_default_ranking(jdata, &options);
                } else {
                    rc = prte_rmaps_base_set_default_ranking(jdata, &options);
                }
                if (PRTE_SUCCESS != rc) {
                    // the error message should have been printed
                    jdata->exit_code = rc;
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                    goto cleanup;
                }
            }
        }
    }
    options.rank = PRTE_GET_RANKING_POLICY(jdata->map->ranking);
    /* if we are ranking by FILL or SPAN, then we must map by an object */
    if ((PRTE_RANK_BY_SPAN == options.rank ||
         PRTE_RANK_BY_FILL == options.rank) &&
        PRTE_MAPPING_PPR != options.map) {
        if (options.map < PRTE_MAPPING_BYNUMA ||
            options.map > PRTE_MAPPING_BYHWTHREAD) {
            pmix_show_help("help-prte-rmaps-base.txt", "must-map-by-obj",
                           true, prte_rmaps_base_print_mapping(options.map),
                           prte_rmaps_base_print_ranking(options.rank));
            jdata->exit_code = PRTE_ERR_SILENT;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }
    /* define the binding policy for this job - if the user specified one
     * already (e.g., during the call to comm_spawn), then we don't
     * override it */
    if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
        did_map = false;
        if (options.oversubscribe) {
            /* if we are oversubscribing, then do not bind */
            jdata->map->binding = PRTE_BIND_TO_NONE;
            did_map = true;
        } else if (inherit) {
            if (NULL != parent) {
                jdata->map->binding = parent->map->binding;
                did_map = true;
            } else if (PRTE_BINDING_POLICY_IS_SET(prte_hwloc_default_binding_policy)) {
                /* if the user specified a default binding policy via
                 * MCA param, then we use it - this can include a directive
                 * to overload */
                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] default binding policy given", __LINE__);
                jdata->map->binding = prte_hwloc_default_binding_policy;
                did_map = true;
            }
        }
        if (!did_map) {
            // let the job's personality set the default binding behavior
            if (NULL != schizo->set_default_binding) {
                rc = schizo->set_default_binding(jdata, &options);
            } else {
                rc = prte_hwloc_base_set_default_binding(jdata, &options);
            }
            if (PRTE_SUCCESS != rc) {
                // the error message should have been printed
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
        }
    }
    options.overload = PRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding);
    options.bind = PRTE_GET_BINDING_POLICY(jdata->map->binding);
    /* sanity check */
    if (options.mapdepth > options.bind &&
        PRTE_BIND_TO_NONE != options.bind) {
        /* we cannot bind to objects higher in the
         * topology than where we mapped */
        pmix_show_help("help-prte-hwloc-base.txt", "bind-upwards", true,
                       prte_rmaps_base_print_mapping(options.map),
                       prte_hwloc_base_print_binding(options.bind));
        jdata->exit_code = rc;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }
    switch (options.bind) {
        case PRTE_BIND_TO_NONE:
            options.hwb = HWLOC_OBJ_MACHINE;
            break;
        case PRTE_BIND_TO_PACKAGE:
            options.hwb = HWLOC_OBJ_PACKAGE;
            break;
        case PRTE_BIND_TO_NUMA:
            options.hwb = HWLOC_OBJ_NUMANODE;
            break;
        case PRTE_BIND_TO_L3CACHE:
            options.hwb = HWLOC_OBJ_L3CACHE;
            break;
        case PRTE_BIND_TO_L2CACHE:
            options.hwb = HWLOC_OBJ_L2CACHE;
            break;
        case PRTE_BIND_TO_L1CACHE:
            options.hwb = HWLOC_OBJ_L1CACHE;
            break;
        case PRTE_BIND_TO_CORE:
            options.hwb = HWLOC_OBJ_CORE;
            break;
        case PRTE_BIND_TO_HWTHREAD:
            options.hwb = HWLOC_OBJ_PU;
            break;
        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
    }
    if (1 < options.cpus_per_rank ||
        NULL != options.job_cpuset ||
        options.ordered) {
        /* REQUIRES binding to cpu */
        if (PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            if (PRTE_BIND_TO_CORE != options.bind &&
                PRTE_BIND_TO_HWTHREAD != options.bind) {
                pmix_show_help("help-prte-rmaps-base.txt", "unsupported-combination", true,
                               "binding", prte_hwloc_base_print_binding(options.bind));
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                jdata->exit_code = PRTE_ERR_BAD_PARAM;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            /* ensure the cpu usage setting matches the provided bind directive */
            if (PRTE_BIND_TO_HWTHREAD == options.bind) {
                options.use_hwthreads = true;
            } else {
                options.use_hwthreads = false;
            }
        } else {
            if (options.use_hwthreads) {
                PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
                options.bind = PRTE_BIND_TO_HWTHREAD;
            } else {
                PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
                options.bind = PRTE_BIND_TO_CORE;
            }
        }
    }

    /* if we are not going to launch, then we need to set any
     * undefined topologies to match our own so the mapper
     * can operate
     */
    if (options.donotlaunch) {
        prte_topology_t *t0;
        if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PMIX_RELEASE(caddy);
            jdata->exit_code = PRTE_ERR_NOT_FOUND;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        t0 = node->topology;
        for (int i = 1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                node->topology = t0;
            }
        }
    }

    if (colocate_daemons || colocate) {
        /* This is a colocation request, so we don't run any mapping modules */
        if (procs_per_target == 0) {
            pmix_output(0, "Error: COLOCATION REQUESTED WITH ZERO PROCS/TARGET\n");
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        rc = map_colocate(jdata, colocate_daemons, pernode, darray, procs_per_target, &options);
        PMIX_DATA_ARRAY_FREE(darray);
        if (PRTE_SUCCESS != rc) {
            jdata->exit_code = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(jdata->exit_code);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        did_map = true;
    } else {
        /* cycle thru the available mappers until one agrees to map
         * the job
         */
        did_map = false;
        if (1 == pmix_list_get_size(&prte_rmaps_base.selected_modules)) {
            /* forced selection */
            mod = (prte_rmaps_base_selected_module_t *) pmix_list_get_first(
                &prte_rmaps_base.selected_modules);
            jdata->map->req_mapper = strdup(mod->component->pmix_mca_component_name);
        }
        PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules, prte_rmaps_base_selected_module_t)
        {
            if (PRTE_SUCCESS == (rc = mod->module->map_job(jdata, &options)) ||
                PRTE_ERR_RESOURCE_BUSY == rc) {
                did_map = true;
                break;
            }
            /* mappers return "next option" if they didn't attempt to
             * map the job. anything else is a true error.
             */
            if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                jdata->exit_code = rc;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
        }
    }

    if (did_map && PRTE_ERR_RESOURCE_BUSY == rc) {
        /* the map was done but nothing could be mapped
         * for launch as all the resources were busy
         */
        pmix_show_help("help-prte-rmaps-base.txt", "cannot-launch", true);
        jdata->exit_code = rc;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* if we get here without doing the map, or with zero procs in
     * the map, then that's an error
     */
    if (!did_map || 0 == jdata->num_procs || 0 == jdata->map->num_nodes) {
        pmix_show_help("help-prte-rmaps-base.txt", "failed-map", true,
                       PRTE_ERROR_NAME(rc),
                       "N/A",
                       jdata->num_procs,
                       prte_rmaps_base_print_mapping(options.map),
                       prte_hwloc_base_print_binding(options.bind));
        jdata->exit_code = -PRTE_JOB_STATE_MAP_FAILED;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* set the offset so shared memory components can potentially
     * connect to any spawned jobs
     */
    jdata->offset = prte_total_procs;
    /* track the total number of procs launched by us */
    prte_total_procs += jdata->num_procs;

    /* if it is a dynamic spawn, save the bookmark on the parent's job too */
    if (!PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        if (NULL != (parent = prte_get_job_data_object(jdata->originator.nspace))) {
            parent->bookmark = jdata->bookmark;
        }
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        /* display the map */
        prte_rmaps_base_display_map(jdata);
    } else if (options.donotlaunch &&
               prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_BINDINGS, NULL, PMIX_BOOL)) {
        prte_rmaps_base_report_bindings(jdata, &options);
    }

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_COMPLETE);

cleanup:
    /* reset any node map flags we used so the next job will start clean */
    for (int i = 0; i < jdata->map->nodes->size; i++) {
        if (NULL != (node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, i))) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }
    if (NULL != options.job_cpuset) {
        hwloc_bitmap_free(options.job_cpuset);
        options.job_cpuset = NULL;
    }
    if (NULL != options.target) {
        hwloc_bitmap_free(options.target);
        options.target = NULL;
    }
    /* cleanup */
    PMIX_RELEASE(caddy);
}

void prte_rmaps_base_display_map(prte_job_t *jdata)
{
    pmix_proc_t source;
    char *tmp;

    prte_map_print(&tmp, jdata);
    PMIX_LOAD_PROCID(&source, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, tmp);
}

void prte_rmaps_base_report_bindings(prte_job_t *jdata,
                                     prte_rmaps_options_t *options)
{
    int n;
    prte_proc_t *proc;
    char **cache = NULL;
    char *out, *tmp;
    pmix_proc_t source;
    bool physical;

    // see if we are to report physical (vs logical) cpu IDs
    physical = prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL, PMIX_BOOL);
    for (n=0; n < jdata->procs->size; n++) {
        proc = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, n);
        if (NULL == proc) {
            continue;
        }
        if (NULL == proc->cpuset) {
            pmix_asprintf(&out, "Proc %s Node %s is UNBOUND",
                          PRTE_NAME_PRINT(&proc->name), proc->node->name);
        } else {
            hwloc_bitmap_list_sscanf(prte_rmaps_base.available, proc->cpuset);
            tmp = prte_hwloc_base_cset2str(prte_rmaps_base.available,
                                           options->use_hwthreads,
                                           physical,
                                           proc->node->topology->topo);
            pmix_asprintf(&out, "Proc %s Node %s bound to %s",
                          PRTE_NAME_PRINT(&proc->name),
                          proc->node->name, tmp);
            free(tmp);
        }
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&cache, out);
        free(out);
    }

    if (NULL == cache) {
        out = strdup("Error: job has no procs");
    } else {
        /* add a blank line with \n on it so IOF will output the last line */
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&cache, "");
        out = PMIX_ARGV_JOIN_COMPAT(cache, '\n');
        PMIX_ARGV_FREE_COMPAT(cache);
    }
    PMIX_LOAD_PROCID(&source, jdata->nspace, PMIX_RANK_WILDCARD);
    prte_iof_base_output(&source, PMIX_FWD_STDOUT_CHANNEL, out);
}

static int map_colocate(prte_job_t *jdata,
                        bool daemons, bool pernode,
                        pmix_data_array_t *darray,
                        uint16_t procs_per_target,
                        prte_rmaps_options_t *options)
{
    char *tmp;
    pmix_status_t rc;
    size_t n, nprocs;
    pmix_proc_t *procs;
    prte_job_t *target_jdata;
    prte_job_map_t *target_map, *map;
    prte_app_context_t *app;
    int i, j, ret, cnt;
    pmix_list_t targets;
    prte_proc_t *proc;
    prte_node_t *node, *nptr, *n2;

    if (4 < pmix_output_get_verbosity(prte_rmaps_base_framework.framework_output)) {
        rc = PMIx_Data_print(&tmp, NULL, darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            pmix_output(0, "%s rmaps: mapping job %s: Colocate with UNPRINTABLE (%s)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_JOBID_PRINT(jdata->nspace),
                        PMIx_Error_string(rc));
        } else {
            pmix_output(0, "%s rmaps: mapping job %s: Colocate with\n  %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_JOBID_PRINT(jdata->nspace), tmp);
        }
        free(tmp);
    }
    procs = (pmix_proc_t*)darray->array;
    nprocs = darray->size;
    map = jdata->map;
    if (daemons) {
        /* daemons are never bound and always rank by-slot */
        PRTE_SET_BINDING_POLICY(map->binding, PRTE_BIND_TO_NONE);
        PRTE_SET_RANKING_POLICY(map->ranking, PRTE_RANK_BY_SLOT);
    }
    jdata->num_procs = 0;

    /* create a list of the target nodes */
    PMIX_CONSTRUCT(&targets, pmix_list_t);
    /* need to ensure each node only appears _once_ on the list */
    for (n=0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD == procs[n].rank) {
            target_jdata = prte_get_job_data_object(procs[n].nspace);
            if (NULL == target_jdata) {
                pmix_output(0, "Unable to find app job %s\n", procs[n].nspace);
                ret = PRTE_ERR_BAD_PARAM;
                goto done;
            }
            target_map = target_jdata->map;
            for (i = 0; i < target_map->nodes->size; i++) {
                node = (prte_node_t*)pmix_pointer_array_get_item(target_map->nodes, i);
                if (NULL == node) {
                    continue;
                }
                if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                    PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
                    PMIX_RETAIN(node);
                    pmix_list_append(&targets, &node->super);
                }
            }
            continue;
        }
        /* not a wildcard rank */
        proc = prte_get_proc_object(&procs[n]);
        if (NULL == proc) {
            pmix_output(0, "Unable to find target process %s\n", PMIX_NAME_PRINT(&procs[n]));
            ret = PRTE_ERR_BAD_PARAM;
            goto done;
        }
        if (NULL == proc->node) {
            pmix_output(0, "Target process %s has not been mapped to a node\n", PMIX_NAME_PRINT(&procs[n]));
            ret = PRTE_ERR_BAD_PARAM;
            goto done;
        }
        node = proc->node;
        if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
            /* add this node to the list */
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
            PMIX_RETAIN(node);
            pmix_list_append(&targets, &node->super);
        }
    }

    // clear the flags
    PMIX_LIST_FOREACH(nptr, &targets, prte_node_t) {
        PRTE_FLAG_UNSET(nptr, PRTE_NODE_FLAG_MAPPED);
    }


    if (pernode) {
        /* cycle across the target nodes and place the specified
         * number of procs on each one */
        PMIX_LIST_FOREACH_SAFE(nptr, n2, &targets, prte_node_t) {
            // setup the mapping options
            options->ncpus = prte_rmaps_base_get_ncpus(nptr, NULL, options);
            /* the available cpus are in the scratch location */
            options->target = hwloc_bitmap_dup(prte_rmaps_base.available);
            options->nprocs = procs_per_target;
           // Assign N procs per node for each app_context
            for (i=0; i < jdata->apps->size; i++) {
                app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, i);
                if (NULL == app) {
                    continue;
                }
                // is there room on this node? daemons don't count
                if (!daemons) {
                    cnt = nptr->slots_inuse + procs_per_target;
                    // first check the absolute limit
                    if (0 != nptr->slots_max && nptr->slots_max < cnt) {
                        // violates the max limit
                        ret = PRTE_ERR_OUT_OF_RESOURCE;
                        goto done;
                    }
                    if (nptr->slots < cnt) {
                        // oversubscribed - we can still fit if they allow oversubscription
                        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(map->mapping)) {
                            pmix_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                           app->num_procs, app->app, prte_process_info.nodename);
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                            ret = PRTE_ERR_SILENT;
                            goto done;
                        }
                        // we can use it oversubscribed - so mark it
                        PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                        PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                    }
                }
                // Map the node to this job
                if (!PRTE_FLAG_TEST(nptr, PRTE_NODE_FLAG_MAPPED)) {
                    PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_MAPPED);
                    PMIX_RETAIN(nptr);
                    pmix_pointer_array_add(map->nodes, nptr);
                    map->num_nodes += 1;
                    options->nnodes++;
                }
                // map the procs
                for (j = 0; j < procs_per_target; ++j) {
                    proc = prte_rmaps_base_setup_proc(jdata, app->idx, nptr, NULL, options);
                    if (NULL == proc) {
                        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                        ret = PRTE_ERR_OUT_OF_RESOURCE;
                        goto done;
                    }
                    jdata->num_procs += 1;
                    app->num_procs += 1;
                    PMIX_RELEASE(proc);
                }
            }
        }

        /* calculate the ranks for this job */
        ret = prte_rmaps_base_compute_vpids(jdata, options);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }
        ret = PRTE_SUCCESS;
        goto done;
    }

    /* handle the case of colocate by process */
    PMIX_LIST_FOREACH_SAFE(nptr, n2, &targets, prte_node_t) {
        // setup the mapping options
        options->ncpus = prte_rmaps_base_get_ncpus(nptr, NULL, options);
        /* the available cpus are in the scratch location */
        options->target = hwloc_bitmap_dup(prte_rmaps_base.available);
        // count the number of target procs on this node
        cnt = 0;
        for (i=0; i < nptr->procs->size; i++) {
            proc = (prte_proc_t*)pmix_pointer_array_get_item(nptr->procs, i);
            if (NULL == proc) {
                continue;
            }
            for (n=0; n < nprocs; n++) {
                if (PMIX_CHECK_PROCID(&procs[n], &proc->name)) {
                    ++cnt;
                    break;
                }
            }
        }
        if (0 == cnt) {
            // should not happen
            continue;
        }
        // assign the number of procs to be placed on this node
        options->nprocs = cnt * procs_per_target; // total number of procs to place on this node;
        // Assign procs for each app_context
        for (i=0; i < jdata->apps->size; i++) {
            app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, i);
            if (NULL == app) {
                continue;
            }
            // is there room on this node? daemons don't count
            if (!daemons) {
                cnt = nptr->slots_inuse + options->nprocs;
                // first check the absolute limit
                if (0 != nptr->slots_max && nptr->slots_max < cnt) {
                    // violates the max limit
                    ret = PRTE_ERR_OUT_OF_RESOURCE;
                    goto done;
                }
                // check oversubscribed
                if (nptr->slots < cnt) {
                    // oversubscribed - we can still fit if they allow oversubscription
                    if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(map->mapping)) {
                        pmix_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                       app->num_procs, app->app, prte_process_info.nodename);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        ret = PRTE_ERR_SILENT;
                        goto done;
                    }
                    // we can use it oversubscribed - so mark it
                    PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                }
            }
            // Map the node to this job
            if (!PRTE_FLAG_TEST(nptr, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_FLAG_SET(nptr, PRTE_NODE_FLAG_MAPPED);
                PMIX_RETAIN(nptr);
                pmix_pointer_array_add(map->nodes, nptr);
                map->num_nodes += 1;
                options->nnodes++;
            }
            // map the procs
            for (j = 0; j < options->nprocs; ++j) {
                proc = prte_rmaps_base_setup_proc(jdata, i, nptr, NULL, options);
                if (NULL == proc) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    ret = PRTE_ERR_OUT_OF_RESOURCE;
                    goto done;
                }
                jdata->num_procs += 1;
                app->num_procs += 1;
                PMIX_RELEASE(proc);
            }
        }
    }
    ret = prte_rmaps_base_compute_vpids(jdata, options);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }
    ret = PRTE_SUCCESS;

done:
    // ensure all the nodes are marked as not mapped
    for (i=0; i < map->nodes->size; i++) {
        node = (prte_node_t*)pmix_pointer_array_get_item(map->nodes, i);
        if (NULL != node) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }
    PMIX_LIST_DESTRUCT(&targets);
    return ret;
}

static void inherit_env_directives(prte_job_t *jdata,
                                   prte_job_t *parent,
                                   pmix_proc_t *proxy)
{
    prte_app_context_t *app, *app2;
    prte_proc_t *p;
    prte_attribute_t *attr, *attr2;
    pmix_value_t *val, *val2;
    pmix_envar_t *envar, *envar2;
    int n;
    bool exists;

    // deal with job-level attributes first
    PMIX_LIST_FOREACH(attr, &parent->attributes, prte_attribute_t) {
        if (PMIX_ENVAR != attr->data.type) {
            continue;
        }
        val = &attr->data;
        envar = &val->data.envar;

        // do we have a matching attribute in the new job?
        exists = false;
        PMIX_LIST_FOREACH(attr2, &jdata->attributes, prte_attribute_t) {
            if (PMIX_ENVAR != attr->data.type) {
                continue;
            }
            val2 = &attr2->data;
            envar2 = &val2->data.envar;

            if (attr->key == attr2->key) {
                // operation is same - check if the target envars match
                if (0 == strcmp(envar->envar, envar2->envar)) {
                    // these match, so don't overwrite it
                    exists = true;
                    break;
                }
            }
        }

        if (exists) {
            // leave this alone
            continue;
        }

        // if it doesn't exist, then inherit it
        prte_set_attribute(&jdata->attributes, attr->key, PRTE_ATTR_GLOBAL,
                           envar, PMIX_ENVAR);
    }

    /* There is no one-to-one correlation between the apps, but we can
     * inherit the directives from the proc that called spawn, so do that
     * much here */
    p = prte_get_proc_object(proxy);
    if (NULL == p) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    app = (prte_app_context_t*)pmix_pointer_array_get_item(parent->apps, p->app_idx);
    if (NULL == app) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    for (n=0; n < jdata->apps->size; n++) {
        app2 = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, n);
        if (NULL == app2) {
            continue;
        }
        PMIX_LIST_FOREACH(attr, &app->attributes, prte_attribute_t) {
            if (PMIX_ENVAR != attr->data.type) {
                continue;
            }
            val = &attr->data;
            envar = &val->data.envar;

            exists = false;
            PMIX_LIST_FOREACH(attr2, &app2->attributes, prte_attribute_t) {
                if (PMIX_ENVAR != attr->data.type) {
                    continue;
                }
                val2 = &attr2->data;
                envar2 = &val2->data.envar;

                if (attr->key == attr2->key) {
                    // operation is same - check if the target envars match
                    if (0 == strcmp(envar->envar, envar2->envar)) {
                        // these match, so don't overwrite it
                        exists = true;
                        break;
                    }
                }
            }

            if (exists) {
                // leave this alone
                continue;
            }

            // if it doesn't exist, then inherit it
            prte_set_attribute(&app2->attributes, attr->key, PRTE_ATTR_GLOBAL,
                               envar, PMIX_ENVAR);
        }
    }

}
