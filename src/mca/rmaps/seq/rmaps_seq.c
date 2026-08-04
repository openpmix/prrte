/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <ctype.h>
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "rmaps_seq.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int prte_rmaps_seq_map(prte_job_t *jdata,
                              prte_rmaps_options_t *options);

/* define the module */
prte_rmaps_base_module_t prte_rmaps_seq_module = {
    .map_job = prte_rmaps_seq_map
};

/* local object for tracking rank locations */
typedef struct {
    pmix_list_item_t super;
    char *hostname;
    char *cpuset;
} seq_node_t;
static void sn_con(seq_node_t *p)
{
    p->hostname = NULL;
    p->cpuset = NULL;
}
static void sn_des(seq_node_t *p)
{
    if (NULL != p->hostname) {
        free(p->hostname);
        p->hostname = NULL;
    }
    if (NULL != p->cpuset) {
        free(p->cpuset);
        p->cpuset = NULL;
    }
}
PMIX_CLASS_INSTANCE(seq_node_t, pmix_list_item_t, sn_con, sn_des);

static int process_file(char *path, pmix_list_t *list);

/*
 * Bind a proc to the cpu list its sequence entry named.
 *
 * A sequence file line is "hostname [cpuset [numa mempolicy]]", so an entry
 * may say not just which node a rank goes on but which cpus it gets there.
 * That list is the binding for that rank - it is not a hint the object-based
 * binder then refines - so this mirrors what the rankfile mapper does with a
 * slot list: parse it against this node's topology, hand the result to the
 * proc, and take those cpus out of what the node has left so the next rank
 * does not get them too.
 *
 * The locale (proc->obj) is deliberately left NULL: a cpu list may span
 * several objects, so there is no single one to point at.
 */
static int bind_to_entry_cpuset(prte_job_t *jdata, prte_proc_t *proc,
                                prte_node_t *node, char *cpuset,
                                prte_rmaps_options_t *options)
{
    hwloc_cpuset_t bits;
    char *avail = NULL, *overlap = NULL;
    hwloc_cpuset_t missing;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(jdata);

    if (NULL == node->topology || NULL == node->topology->topo) {
        /* without a topology there is nothing to resolve the list against */
        prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-topology", true, node->name);
        return PRTE_ERR_SILENT;
    }

    bits = hwloc_bitmap_alloc();
    rc = prte_hwloc_base_cpu_list_parse(cpuset, node->topology->topo,
                                        options->use_hwthreads, bits);
    if (PRTE_SUCCESS != rc) {
        char *tmp = prte_hwloc_base_cset2str(hwloc_topology_get_allowed_cpuset(node->topology->topo),
                                             false, false, node->topology->topo);
        prte_show_help("help-prte-rmaps-seq.txt", "seq:bad-cpuset", true,
                       cpuset, node->name, tmp);
        free(tmp);
        hwloc_bitmap_free(bits);
        return PRTE_ERR_SILENT;
    }

    /* the cpus have to actually be free on this node, unless the job said
     * it was content to overload them */
    if (!options->overload && !hwloc_bitmap_isincluded(bits, node->available)) {
        missing = hwloc_bitmap_alloc();
        hwloc_bitmap_andnot(missing, bits, node->available);
        /* The user wrote "cpuset" in logical cpu ids - that is what
         * prte_hwloc_base_cpu_list_parse() accepts - so the two sets we
         * show alongside it have to be in the same terms. Rendering the
         * bitmaps directly prints PU *OS* indices, which put three
         * different numbering schemes in one message on any node whose
         * firmware does not number its cpus in hwloc's order. */
        avail = prte_hwloc_base_cpuset2ranges(node->topology->topo, node->available,
                                              options->use_hwthreads, false);
        overlap = prte_hwloc_base_cpuset2ranges(node->topology->topo, missing,
                                                options->use_hwthreads, false);
        prte_show_help("help-prte-rmaps-seq.txt", "seq:cpuset-not-available", true,
                       PRTE_NAME_PRINT(&proc->name), node->name, cpuset,
                       (NULL == avail) ? "NONE" : avail,
                       (NULL == overlap) ? "NONE" : overlap);
        free(avail);
        free(overlap);
        hwloc_bitmap_free(missing);
        hwloc_bitmap_free(bits);
        return PRTE_ERR_SILENT;
    }

    if (NULL != proc->cpuset) {
        free(proc->cpuset);
    }
    hwloc_bitmap_list_asprintf(&proc->cpuset, bits);
    /* consume them so the next entry naming the same node cannot reuse them */
    hwloc_bitmap_andnot(node->available, node->available, bits);
    hwloc_bitmap_free(bits);

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:seq: bound proc %s on node %s to entry cpus <%s>",
                        PRTE_NAME_PRINT(&proc->name), node->name, proc->cpuset);
    return PRTE_SUCCESS;
}

/*
 * Sequentially map the ranks according to the placement in the
 * specified hostfile
 */
static int prte_rmaps_seq_map(prte_job_t *jdata,
                              prte_rmaps_options_t *options)
{
    prte_job_map_t *map;
    prte_app_context_t *app;
    int i, n;
    int32_t j;
    prte_node_t *node, *nd;
    seq_node_t *sq, *save = NULL, *seq, *seq2;
    pmix_rank_t vpid, apprank;
    int32_t num_nodes;
    int rc;
    pmix_list_t default_seq_list;
    /* seq_list points at either the shared default list or this app's own
     * sq_list; NULL until one has been built, so the error path knows
     * whether there is a per-app list to hand back */
    pmix_list_t node_list, *seq_list = NULL, sq_list;
    prte_proc_t *proc;
    char *hosts = NULL;
    bool match;
    prte_binding_policy_t savebind;
    char *savecpuset;

    PMIX_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:seq called on job %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* this mapper can only handle initial launch
     * when seq mapping is desired - allow
     * restarting of failed apps
     */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: job %s is being restarted - seq cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    /* The policy is what selects the mapper. This reads the resolved options
     * rather than the job map: in per-app dispatch each app answers for
     * itself, and asking the job meant a per-app "--map-by seq" never
     * reached this mapper at all - the job's policy is whatever default was
     * resolved for the apps that gave no directive */
    if (PRTE_MAPPING_SEQ != PRTE_GET_MAPPING_POLICY(options->map)) {
        /* I don't know how to do these - defer */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: job %s not using seq mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:seq: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* convenience def */
    map = jdata->map;

    /* if there is a default hostfile, go and get its ordered list of nodes */
    PMIX_CONSTRUCT(&default_seq_list, pmix_list_t);
    if (NULL != prte_default_hostfile) {
        rc = process_file(prte_default_hostfile, &default_seq_list);
        if (PRTE_SUCCESS != rc) {
            PMIX_LIST_DESTRUCT(&default_seq_list);
            return rc;
        }
    }

    /* start at the beginning of this dispatch's ranks - zero for a whole
     * job, and in per-app dispatch the first rank no earlier app has taken.
     * Numbering every app from zero gave two apps the same ranks, and the
     * second app's procs then replaced the first's in jdata->procs */
    vpid = (pmix_rank_t) options->start_vpid;
    /* Zero the job-wide count only when we were handed the whole job. The
     * per-app (MPMD) dispatch calls us once per app, and resetting here
     * would leave jdata->num_procs holding just the last app's count while
     * jdata->procs holds every app's procs - a mismatch the job packer and
     * unpacker disagree about, corrupting the launch message. */
    if (options->app_idx < 0) {
        jdata->num_procs = 0;
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }
        if (options->app_idx >= 0 && (int)i != options->app_idx) {
            continue;
        }
        apprank = 0;

        /* per-app map file trumps job-level file; fall back to job-level */
        hosts = NULL;
        if (!prte_get_attribute(&app->attributes, PRTE_APP_MAP_FILE, (void **) &hosts, PMIX_STRING)) {
            prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING);
        }
        if (NULL != hosts) {
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using hostfile %s nodes on app %s", hosts,
                                app->app);
            PMIX_CONSTRUCT(&sq_list, pmix_list_t);
            rc = process_file(hosts, &sq_list);
            free(hosts);
            hosts = NULL;
            if (PRTE_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&sq_list);
                goto error;
            }
            seq_list = &sq_list;
            goto process;
        }

        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts, PMIX_STRING)) {
            if (NULL == hosts) {
                rc = PRTE_ERR_NOT_FOUND;
                goto error;
            }
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using dash-host nodes on app %s", app->app);
            PMIX_CONSTRUCT(&node_list, pmix_list_t);
            /* dash host entries cannot specify cpusets, so used the std function to retrieve the
             * list */
            rc = prte_util_get_ordered_dash_host_list(&node_list, hosts);
            free(hosts);
            hosts = NULL;
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&node_list);
                goto error;
            }
            /* transfer the list to a seq_node_t list */
            PMIX_CONSTRUCT(&sq_list, pmix_list_t);
            while (NULL != (nd = (prte_node_t *) pmix_list_remove_first(&node_list))) {
                sq = PMIX_NEW(seq_node_t);
                sq->hostname = strdup(nd->name);
                pmix_list_append(&sq_list, &sq->super);
                PMIX_RELEASE(nd);
            }
            PMIX_DESTRUCT(&node_list);
            seq_list = &sq_list;
            goto process;
        }

        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts, PMIX_STRING)) {
            if (NULL == hosts) {
                rc = PRTE_ERR_NOT_FOUND;
                goto error;
            }
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using hostfile %s nodes on app %s", hosts,
                                app->app);
            PMIX_CONSTRUCT(&sq_list, pmix_list_t);
            rc = process_file(hosts, &sq_list);
            free(hosts);
            hosts = NULL;
            if (PRTE_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&sq_list);
                goto error;
            }
            seq_list = &sq_list;
            goto process;
        }

        /* if we get here, then there were no app-level directives, so try to use the
         * default hostfile list */

        if (0 < pmix_list_get_size(&default_seq_list)) {
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using default hostfile nodes on app %s", app->app);
            seq_list = &default_seq_list;
        } else {
            /* can't do anything - no nodes available! */
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-available-resources",
                           true);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

process:
        /* check for nolocal and remove the head node, if required */
        if (PRTE_GET_MAPPING_DIRECTIVE(map->mapping) & PRTE_MAPPING_NO_USE_LOCAL) {
            PMIX_LIST_FOREACH_SAFE(seq, seq2, seq_list, seq_node_t) {
                /* need to check ifislocal because the name in the
                 * hostfile may not have been FQDN, while name returned
                 * by gethostname may have been (or vice versa)
                 */
                if (prte_check_host_is_local(seq->hostname)) {
                    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:seq: removing head node %s", seq->hostname);
                    pmix_list_remove_item(seq_list, &seq->super);
                    PMIX_RELEASE(seq); /* "un-retain" it */
                }
            }
        }

        if (NULL == seq_list || 0 == (num_nodes = (int32_t) pmix_list_get_size(seq_list))) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-available-resources",
                           true);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

        /* set #procs to the number of entries */
        if (0 == app->num_procs) {
            app->num_procs = num_nodes;
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: setting num procs to %s for app %s",
                                PRTE_VPID_PRINT(app->num_procs), app->app);
        } else if (num_nodes < app->num_procs) {
            prte_show_help("help-prte-rmaps-seq.txt", "seq:not-enough-resources", true,
                           app->num_procs, num_nodes);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

        if (seq_list == &default_seq_list) {
            /* we don't want to start in the same place for every app, so take
             * the next item in the list - if this is the first time we are
             * using the default list, then start at its beginning */
            if (NULL == save) {
                sq = (seq_node_t *) pmix_list_get_first(&default_seq_list);
            } else {
                sq = save;
            }
        } else {
            sq = (seq_node_t *) pmix_list_get_first(seq_list);
        }
        for (n = 0; n < app->num_procs; n++) {
            /* the sequence has to be long enough to hold every rank. It is
             * checked against num_nodes above, but the shared default list is
             * entered at the cursor a previous app left behind, so there may
             * be fewer entries left than that count promises - and walking off
             * the end would read the list's sentinel as if it were an entry */
            if (NULL == sq || pmix_list_get_end(seq_list) == &sq->super) {
                /* "n" entries were consumed getting this far, so that is how
                 * many the sequence turned out to be worth to this app */
                prte_show_help("help-prte-rmaps-seq.txt", "seq:not-enough-resources", true,
                               (int) app->num_procs, n);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            /* find this node on the global array - this is necessary so
             * that our mapping gets saved on that array as the objects
             * returned by the hostfile function are -not- on the array
             */
            match = false;
            for (j = 0; j < prte_node_pool->size; j++) {
                node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, j);
                if (NULL == node) {
                    continue;
                }
                if (prte_quickmatch(node, sq->hostname)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                /* wasn't found - that is an error */
                prte_show_help("help-prte-rmaps-seq.txt", "prte-rmaps-seq:resource-not-found", true,
                               sq->hostname);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            /* check availability */
            prte_rmaps_base_get_cpuset(jdata, node, options);
            if (NULL == options->job_cpuset) {
                // the prior function will have printed out the error
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            /* NULL node list: seq_list holds seq_node_t entries, not the
             * prte_node_t we are placing on, so check_avail must not try to
             * drop the node from it */
            if (!prte_rmaps_base_check_avail(jdata, app, node, NULL, NULL, options)) {
                /* This entry's node cannot take a proc. Step to the next
                 * entry and try the same rank there rather than dropping it:
                 * re-offering the same entry to every remaining rank got
                 * nowhere, and giving up on the rank left the job claiming
                 * more procs than the map holds - a count the launch message
                 * packer and unpacker then disagree about. The sentinel check
                 * at the top of the loop bounds the retry. */
                sq = (seq_node_t *) pmix_list_get_next(&sq->super);
                --n;
                continue;
            }

            /* map the proc. When the sequence entry names the cpus for this
             * rank, that list IS the binding - so keep the ordinary binder
             * out of it and apply the list ourselves below, the way the
             * rankfile mapper applies a slot list. Otherwise the proc would
             * be bound twice: once by policy and once by the file. */
            savebind = options->bind;
            savecpuset = options->cpuset;
            if (NULL != sq->cpuset) {
                options->bind = PRTE_BIND_TO_NONE;
                options->cpuset = NULL;
            }
            proc = prte_rmaps_base_setup_proc(jdata, i, node, NULL, options);
            options->bind = savebind;
            options->cpuset = savecpuset;
            if (NULL == proc) {
                prte_show_help("help-prte-rmaps-seq.txt", "proc-failed-to-map", true,
                               sq->hostname, app->app);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            if (NULL != sq->cpuset) {
                rc = bind_to_entry_cpuset(jdata, proc, node, sq->cpuset, options);
                if (PRTE_SUCCESS != rc) {
                    PMIX_RELEASE(proc);
                    goto error;
                }
            }
            proc->name.rank = vpid;
            vpid++;
            proc->app_rank = apprank;
            apprank++;
            PMIX_RETAIN(proc);
            rc = pmix_pointer_array_set_item(jdata->procs, proc->name.rank, proc);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(proc);
                goto error;
            }
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_SUCCESS != rc &&
                PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PMIX_RELEASE(proc);
                goto error;
            }
            pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: assigned proc %s to node %s for app %s",
                                PRTE_VPID_PRINT(proc->name.rank), sq->hostname, app->app);

            /* move to next node */
            sq = (seq_node_t *) pmix_list_get_next(&sq->super);
            PMIX_RELEASE(proc);
        }

        /** track the total number of processes we mapped */
        jdata->num_procs += app->num_procs;

        /* cleanup the node list if it came from this app_context */
        if (seq_list != &default_seq_list) {
            PMIX_LIST_DESTRUCT(seq_list);
            seq_list = NULL;
        } else {
            save = sq;
        }
    }
    /* the default list was built from the default hostfile for this map and
     * is of no use to anyone afterwards */
    PMIX_LIST_DESTRUCT(&default_seq_list);
    /* compute local/app ranks - in per-app dispatch mode (app_idx >= 0)
     * the base computes the ranks with the correct cross-app numbering,
     * so skip it here */
    if (options->app_idx < 0) {
        rc = prte_rmaps_base_compute_vpids(jdata, options, -1, NULL);
    }
    return rc;

error:
    if (NULL != seq_list && seq_list != &default_seq_list) {
        PMIX_LIST_DESTRUCT(seq_list);
    }
    PMIX_LIST_DESTRUCT(&default_seq_list);
    if (NULL != hosts) {
        free(hosts);
    }
    if (PRTE_ERR_SILENT != rc) {
        prte_show_help("help-prte-rmaps-base.txt",
                       "failed-map", true,
                       PRTE_ERROR_NAME(rc),
                       (NULL == app) ? "N/A" : app->app,
                       (NULL == app) ? -1 : app->num_procs,
                       prte_rmaps_base_print_mapping(options->map),
                       prte_hwloc_base_print_binding(options->bind));
    }
    return PRTE_ERR_SILENT;
}

static int process_file(char *path, pmix_list_t *list)
{
    char *hstname = NULL;
    FILE *fp;
    seq_node_t *sq;
    char *sep, *eptr, *membind_opt;

    /* open the file */
    fp = fopen(path, "r");
    if (NULL == fp) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    while (NULL != (hstname = pmix_getline(fp))) {
        if (0 == strlen(hstname)) {
            free(hstname);
            /* blank line - ignore */
            continue;
        }
        if ('#' == hstname[0]) {
            free(hstname);
            /* Comment line - ignore */
            continue;
        }
        sq = PMIX_NEW(seq_node_t);
        if (NULL != (sep = strchr(hstname, ' '))) {
            *sep = '\0';
            sep++;
            /* remove any trailing space */
            eptr = sep + strlen(sep) - 1;
            while (eptr > sep && isspace(*eptr)) {
                eptr--;
            }
            *(eptr + 1) = 0;
            /*
             * If the submitted LSF job has memory binding related resource requirement, after
             * the cpu id list there are memory binding options.
             *
             * The following is the format of LSB_AFFINITY_HOSTFILE file:
             *
             * Host1 0,1,2,3 0 2
             * Host1 4,5,6,7 1 2
             *
             * Each line includes: host_name, cpu_id_list, NUMA_node_id_list, and memory_policy.
             * In this fix we will drop the last two sections (NUMA_node_id_list and memory_policy)
             * of each line and keep them in 'membind_opt' for future use.
             */
            if (NULL != (membind_opt = strchr(sep, ' '))) {
                *membind_opt = '\0';
                membind_opt++;
            }
            sq->cpuset = strdup(sep);
        }

        sq->hostname = hstname;
        pmix_list_append(list, &sq->super);
    }
    fclose(fp);
    return PRTE_SUCCESS;
}
