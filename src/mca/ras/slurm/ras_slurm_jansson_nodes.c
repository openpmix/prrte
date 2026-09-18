/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* A job's allocated nodes: the walk down to
 * job_resources.nodes.allocation, and the two callers that consume it. */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>

#if PRTE_TESTBUILD_LAUNCHERS
#    include "src/mca/ras/base/testbuild_jansson.h"
#else
#    include <jansson.h>
#endif

#include "src/mca/errmgr/errmgr.h"
#include "src/util/pmix_output.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/prte_json_window.h"

#include "ras_slurm.h"
#include "ras_slurm_jansson.h"
#include "src/mca/common/slurm/common_slurm.h"

/*
 * How a job's allocated nodes are reported.
 *
 * The nodes sit three levels below the job, and each level skips every member
 * it was not asked for:
 *
 *     jobs[0]
 *       job_resources
 *         nodes
 *           count         what Slurm says the array holds
 *           allocation[]  one element per node, each carrying a bit for
 *                         every socket and every core of that node
 *
 * That is one handler per level, and then one call per node:
 *
 *     job_nodes_member  keeps threads_per_core, descends into job_resources
 *     resources_member  descends into nodes
 *     nodes_member      takes count, walks allocation
 *     alloc_element     parses one node and hands it to node_cb
 *
 * count_cb runs once with the count, before the first node, and may refuse.
 * node_cb runs once per node with that node's record, which is released as
 * soon as it returns.
 */
typedef int (*prte_ras_slurm_alloc_count_fn_t)(size_t node_count, void *cbdata);
typedef int (*prte_ras_slurm_alloc_node_fn_t)(size_t index, json_t *node_obj, void *cbdata);

typedef struct {
    prte_ras_slurm_alloc_count_fn_t count_cb;
    prte_ras_slurm_alloc_node_fn_t node_cb;
    void *cbdata;
    prte_ras_slurm_json_keep_t keep;
    size_t stated;              /* nodes/count, when the record gave one  */
    size_t seen;                /* elements handed to node_cb             */
    bool have_count;
    bool seen_resources;
    bool seen_allocation;
} prte_ras_slurm_alloc_walk_t;

static int prte_ras_slurm_json_alloc_element(prte_json_window_t *win, size_t index, void *cbdata)
{
    prte_ras_slurm_alloc_walk_t *walk = (prte_ras_slurm_alloc_walk_t *) cbdata;
    json_t *node_obj = NULL;
    int err = prte_ras_slurm_json_load(win, "an allocated node", &node_obj);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    walk->seen++;
    err = walk->node_cb(index, node_obj, walk->cbdata);
    json_decref(node_obj);
    return err;
}

/*
 * Take the count Slurm states for the array.
 *
 * This is the only thing that lets a caller size anything up front. It is
 * Slurm's claim about the array and not a fact about it, so the walk checks
 * it against the elements it actually reads.
 */
static int prte_ras_slurm_json_take_node_count(prte_json_window_t *win,
                                               prte_ras_slurm_alloc_walk_t *walk)
{
    json_t *value = NULL;
    int err;

    if (walk->have_count) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    /* Named as a literal: key points into the window, and loading the value
     * is what lets the window reuse the room the name sits in. */
    err = prte_ras_slurm_json_load(win, "the node count", &value);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    if (!json_is_integer(value) || 0 > json_integer_value(value)) {
        json_decref(value);
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    walk->stated = (size_t) json_integer_value(value);
    walk->have_count = true;
    json_decref(value);

    if (NULL != walk->count_cb) {
        return walk->count_cb(walk->stated, walk->cbdata);
    }

    return PRTE_SUCCESS;
}

/* Walk the array of allocated nodes, one element at a time. */
static int prte_ras_slurm_json_walk_allocation(prte_json_window_t *win,
                                               prte_ras_slurm_alloc_walk_t *walk)
{
    if (walk->seen_allocation) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    /* A caller that sizes something from the count needs it before the first
     * element, and Slurm prints it first. */
    if (NULL != walk->count_cb && !walk->have_count) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    walk->seen_allocation = true;
    return prte_json_window_walk_array(win, prte_ras_slurm_json_alloc_element, walk);
}

static int prte_ras_slurm_json_nodes_member(prte_json_window_t *win, const char *key, void *cbdata)
{
    prte_ras_slurm_alloc_walk_t *walk = (prte_ras_slurm_alloc_walk_t *) cbdata;

    if (0 == strcmp(key, "count")) {
        return prte_ras_slurm_json_take_node_count(win, walk);
    }

    if (0 == strcmp(key, "allocation")) {
        return prte_ras_slurm_json_walk_allocation(win, walk);
    }

    return prte_json_window_skip(win);
}

static int prte_ras_slurm_json_resources_member(prte_json_window_t *win, const char *key,
                                                void *cbdata)
{
    if (0 == strcmp(key, "nodes")) {
        return prte_json_window_walk_object(win, prte_ras_slurm_json_nodes_member, cbdata);
    }

    return prte_json_window_skip(win);
}

static int prte_ras_slurm_json_job_nodes_member(prte_json_window_t *win, const char *key,
                                                void *cbdata)
{
    prte_ras_slurm_alloc_walk_t *walk = (prte_ras_slurm_alloc_walk_t *) cbdata;

    if (0 == strcmp(key, "job_resources")) {
        if (walk->seen_resources) {
            return PRTE_ERR_JSON_PARSE_FAILURE;
        }

        walk->seen_resources = true;
        return prte_json_window_walk_object(win, prte_ras_slurm_json_resources_member, cbdata);
    }

    return prte_ras_slurm_json_keep_member(win, key, &walk->keep);
}

/*
 * Report a job's allocated nodes without holding its record.
 *
 * Each node's record is handed to node_cb and released before the next is
 * read, so the cost is one node rather than the job. count_cb, where given,
 * runs first with the count Slurm states, and the array is then required to
 * hold exactly that many.
 *
 * threads_per_core is a member of the job rather than of a node, and Slurm
 * prints it after the array, so it is only settled once this returns. It
 * reads back as 1 where the record leaves it unset or infinite.
 *
 * @param[in]  slurm_jobid       Slurm job ID to query.
 * @param[in]  count_cb          called once with nodes/count; may be NULL.
 * @param[in]  node_cb           called once per allocated node.
 * @param[in]  cbdata            passed to both callbacks.
 * @param[out] threads_per_core  the job's threads_per_core; may be NULL.
 */
static int prte_ras_slurm_walk_alloc_nodes(const char *slurm_jobid,
                                           prte_ras_slurm_alloc_count_fn_t count_cb,
                                           prte_ras_slurm_alloc_node_fn_t node_cb, void *cbdata,
                                           int *threads_per_core)
{
    static const char *const keys[] = {"threads_per_core", NULL};

    prte_ras_slurm_alloc_walk_t walk = {0};
    int64_t threads = 0;
    int err;

    if (NULL == node_cb) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    walk.count_cb = count_cb;
    walk.node_cb = node_cb;
    walk.cbdata = cbdata;
    walk.keep.keys = keys;
    walk.keep.kept = json_object();

    if (NULL == walk.keep.kept) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    err = prte_ras_slurm_json_run(slurm_jobid, prte_ras_slurm_json_job_nodes_member, &walk);

    if (PRTE_SUCCESS == err && !walk.seen_allocation) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
    }

    if (PRTE_SUCCESS == err && walk.have_count && walk.seen != walk.stated) {
        pmix_output(0, "ras:slurm:walk_alloc_nodes: job %s says it holds %lu nodes"
                       " but listed %lu.",
                    slurm_jobid, (unsigned long) walk.stated, (unsigned long) walk.seen);
        err = PRTE_ERR_JSON_PARSE_FAILURE;
    }

    if (PRTE_SUCCESS == err && NULL != threads_per_core) {
        err = prte_ras_slurm_get_json_numobj_value(walk.keep.kept, "threads_per_core", &threads);

        /* Unset, infinite and out of range all read back as one thread. */
        if (PRTE_SUCCESS == err) {
            *threads_per_core = (0 < threads && PRTE_SLURM_MAX_THREADS_PER_CORE >= threads)
                                    ? (int) threads
                                    : 1;
        }
    }

    json_decref(walk.keep.kept);
    return err;
}

/*
 * One node as the allocation array describes it.
 *
 * The list is built whole before any prte_node_t is, so a record that fails
 * partway adds nothing to the caller's list.
 */
typedef struct {
    char *name;
    int core_count;
    int cpu_count;
} prte_ras_slurm_alloc_node_t;

typedef struct {
    prte_ras_slurm_alloc_node_t *nodes;
    size_t count;
    size_t cap;
} prte_ras_slurm_alloc_list_t;

static void prte_ras_slurm_alloc_list_destruct(prte_ras_slurm_alloc_list_t *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        free(list->nodes[i].name);
    }

    free(list->nodes);
    list->nodes = NULL;
    list->count = 0;
    list->cap = 0;
}

static int prte_ras_slurm_alloc_list_add(prte_ras_slurm_alloc_list_t *list, const char *name,
                                         int core_count, int cpu_count)
{
    char *name_dup;

    if (list->count == list->cap) {
        size_t cap = (0 == list->cap) ? 16 : list->cap * 2;
        prte_ras_slurm_alloc_node_t *grown = realloc(list->nodes, cap * sizeof(*grown));

        if (NULL == grown) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        list->nodes = grown;
        list->cap = cap;
    }

    name_dup = strdup(name);

    if (NULL == name_dup) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    list->nodes[list->count].name = name_dup;
    list->nodes[list->count].core_count = core_count;
    list->nodes[list->count].cpu_count = cpu_count;
    list->count++;

    return PRTE_SUCCESS;
}

/*
 * Turn a collected allocation into nodes the DVM can be grown onto.
 *
 * "cpus" is the number of hardware threads the node offers. To respect what
 * the original job asked for, slots are (allocated cores * threads_per_core)
 * held to that ceiling.
 */
static int prte_ras_slurm_alloc_list_to_nodes(prte_ras_slurm_alloc_list_t *list,
                                              int threads_per_core, pmix_list_t *node_list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        prte_ras_slurm_alloc_node_t *entry = &list->nodes[i];
        prte_node_t *node;
        int slots;

        if (entry->core_count > INT_MAX / threads_per_core) {
            PRTE_ERROR_LOG(PRTE_ERR_JSON_PARSE_FAILURE);
            return PRTE_ERR_JSON_PARSE_FAILURE;
        }

        slots = entry->core_count * threads_per_core;

        if (slots > entry->cpu_count) {
            slots = entry->cpu_count;
        }

        node = PMIX_NEW(prte_node_t);

        if (NULL == node) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* These nodes are being added to a DVM that is already running, so
         * they must carry the mark the DVM extension selects on: a grow
         * launches daemons on the nodes in PRTE_NODE_STATE_ADDED and only
         * those (prte_plm_base_setup_virtual_machine). Handing them over as
         * plain UP - the state an initial discovery uses - left every node
         * Slurm granted us sitting in the pool with no daemon on it, so the
         * extend added resources the DVM could never use. The other producers
         * of a grow (ras/hosts, the no-scheduler insert path, and the
         * reused-node branch of this component) all mark ADDED for exactly
         * this reason. */
        node->state = PRTE_NODE_STATE_ADDED;
        node->name = entry->name;
        entry->name = NULL;
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = slots;
        /* derived from the allocation Slurm just granted - authoritative */
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);

        pmix_list_append(node_list, &node->super);

        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
            "%s ras:slurm:add_modified_resources: discovered node %s with %d slots",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, node->slots));
    }

    return PRTE_SUCCESS;
}

/*
 * Fetch and parse Slurm job resource JSON and add allocated nodes and slots.
 *
 * Given a Slurm job ID, this function retrieves the job resource description,
 * validates the expected JSON structure, and creates one node entry for
 * each allocated node in the job.
 *
 * Slot calculation is based on the number of allocated cores found in the
 * socket/core status data, multiplied by the effective threads-per-core value,
 * and capped by cpus.count:
 *
 * slots = min(allocated_cores * threads_per_core, cpus.count)
 *
 * The resulting nodes are inserted into the provided node list.
 *
 * @param[in] slurm_jobid Slurm job ID.
 * @param[in,out] node_list. A pmix_list_t to add nodes to.
 */
/* One element of the allocation array: its name, and how many of its cores
 * Slurm marked allocated. */
static int prte_ras_slurm_collect_alloc_node(size_t index, json_t *node_obj, void *cbdata)
{
    prte_ras_slurm_alloc_list_t *collected = (prte_ras_slurm_alloc_list_t *) cbdata;
    int err = PRTE_SUCCESS;

    PRTE_HIDE_UNUSED_PARAMS(index);

    if (!json_is_object(node_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_t *nodename = json_object_get(node_obj, "name");

    if (NULL == nodename || !json_is_string(nodename)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    const char *nodename_string = json_string_value(nodename);

    if (NULL == nodename_string || '\0' == nodename_string[0]) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_t *cpu_info = json_object_get(node_obj, "cpus");

    if (NULL == cpu_info || !json_is_object(cpu_info)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_t *cpu_count_obj = json_object_get(cpu_info, "count");

    if (NULL == cpu_count_obj || !json_is_integer(cpu_count_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    json_int_t cpu_count_num = json_integer_value(cpu_count_obj);

    if (0 >= cpu_count_num || INT_MAX < cpu_count_num) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    int cpu_max_count = (int)cpu_count_num;

    int core_count = 0;

    json_t *sockets = json_object_get(node_obj, "sockets");

    if(NULL == sockets || !json_is_array(sockets)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    size_t socket_idx;
    json_t *socket_obj;

    json_array_foreach(sockets, socket_idx, socket_obj) {

        if (!json_is_object(socket_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
        }

        json_t *cores = json_object_get(socket_obj, "cores");

        if (NULL == cores || !json_is_array(cores)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
        }

        size_t core_idx;
        json_t *core_obj;

        json_array_foreach(cores, core_idx, core_obj) {

        if(!json_is_object(core_obj)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            return err;
        }

        json_t *statuses = json_object_get(core_obj, "status");
        if (NULL == statuses || !json_is_array(statuses)) {
            err = PRTE_ERR_JSON_PARSE_FAILURE;
            PRTE_ERROR_LOG(err);
            return err;
        }

        size_t status_idx;
        json_t *status_obj;

        json_array_foreach(statuses, status_idx, status_obj) {

            if(!json_is_string(status_obj)) {
                err = PRTE_ERR_JSON_PARSE_FAILURE;
                PRTE_ERROR_LOG(err);
                return err;
            }

            if (0 == strcmp(json_string_value(status_obj), "ALLOCATED")) {
                core_count++;

                if(PRTE_SLURM_MAX_CORE_COUNT < core_count) {
                err = PRTE_ERR_JSON_PARSE_FAILURE;
                PRTE_ERROR_LOG(err);
                return err;
                }

                break;
            }
        }
        }
    }

    if(0 >= core_count) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    return prte_ras_slurm_alloc_list_add(collected, nodename_string, core_count,
                                        cpu_max_count);
}

int prte_ras_slurm_add_modified_resources(const char *slurm_jobid, pmix_list_t *node_list)
{
    prte_ras_slurm_alloc_list_t collected = {NULL, 0, 0};
    int threads_per_core = 1;
    uint32_t jobid_val;
    int err;

    if(NULL == slurm_jobid || NULL == node_list) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    err = prte_ras_slurm_convert_jobid(slurm_jobid, &jobid_val);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    err = prte_ras_slurm_walk_alloc_nodes(slurm_jobid, NULL, prte_ras_slurm_collect_alloc_node,
                                          &collected, &threads_per_core);

    if(PRTE_SUCCESS == err) {
        err = prte_ras_slurm_alloc_list_to_nodes(&collected, threads_per_core, node_list);
    }

    prte_ras_slurm_alloc_list_destruct(&collected);

    return err;
}

/**
 * Synchronize a session's node list with a reduced Slurm allocation.
 *
 * Nodes no longer present in the allocation are appended to
 * removed_nodes.
 *
 * @param[in] slurm_jobid Slurm job identifier.
 * @param[in,out] session Session to update.
 * @param[out] removed_nodes Receives detached nodes; must be empty.
 */
/* The session's nodes, and which of them the record still lists. */
typedef struct {
    pmix_pointer_array_t *matched;
    pmix_pointer_array_t *unmatched;
    size_t session_nodes;
} prte_ras_slurm_detach_t;

static int prte_ras_slurm_detach_count(size_t node_count, void *cbdata)
{
    prte_ras_slurm_detach_t *detach = (prte_ras_slurm_detach_t *) cbdata;

    /* Sanity checks the matching below relies on. Slurm states this count
     * ahead of the nodes themselves, and the walk holds the list to it. */
    if (0 == node_count || 0 == detach->session_nodes || node_count >= detach->session_nodes) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    return PRTE_SUCCESS;
}

static int prte_ras_slurm_detach_node(size_t index, json_t *node_obj, void *cbdata)
{
    prte_ras_slurm_detach_t *detach = (prte_ras_slurm_detach_t *) cbdata;
    const char *nodename_string;
    json_t *nodename;
    int session_node_idx;
    int err;
    int j;

    if (!json_is_object(node_obj)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    nodename = json_object_get(node_obj, "name");

    if (NULL == nodename || !json_is_string(nodename)) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    nodename_string = json_string_value(nodename);

    if (NULL == nodename_string || '\0' == nodename_string[0]) {
        err = PRTE_ERR_JSON_PARSE_FAILURE;
        PRTE_ERROR_LOG(err);
        return err;
    }

    session_node_idx = (int) index;

    /* find the equivalent node in the list of unmatched nodes.
     * We expect (but do not strictly require) the ordering here
     * to be favorable; i.e. node at index i should be the
     * one we are looking for */
    for (j = 0; j < detach->unmatched->size; j++) {
        prte_node_t *curr = (prte_node_t *) pmix_pointer_array_get_item(detach->unmatched,
                                                                        session_node_idx);

        if (NULL != curr && NULL != curr->name && 0 == strcmp(curr->name, nodename_string)) {
            pmix_pointer_array_add(detach->matched, curr);
            pmix_pointer_array_set_item(detach->unmatched, session_node_idx, NULL);
            return PRTE_SUCCESS;
        }

        if (++session_node_idx >= detach->unmatched->size) {
            session_node_idx = 0;
        }
    }

    err = PRTE_ERR_NOT_FOUND;
    PRTE_ERROR_LOG(err);
    return err;
}

int prte_ras_slurm_detach_nodes(const char *slurm_jobid, prte_session_t *session, pmix_pointer_array_t *removed_nodes)
{
    pmix_pointer_array_t matched_nodes, unmatched_nodes;
    prte_ras_slurm_detach_t detach;
    int err;
    int i;

    if (NULL == slurm_jobid || NULL == removed_nodes ||
        NULL == session || NULL == session->nodes ||
        0 != removed_nodes->size) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    PMIX_CONSTRUCT(&matched_nodes, pmix_pointer_array_t);
    PMIX_CONSTRUCT(&unmatched_nodes, pmix_pointer_array_t);

    detach.matched = &matched_nodes;
    detach.unmatched = &unmatched_nodes;
    detach.session_nodes = 0;

    /* Built before the record is read, so the count Slurm states can be
     * judged the moment it arrives. */
    for (i = 0; i < session->nodes->size; i++) {
        prte_node_t *node_ptr = (prte_node_t *)pmix_pointer_array_get_item(session->nodes, i);

        if (NULL != node_ptr) {
            pmix_pointer_array_add(&unmatched_nodes, node_ptr);
            detach.session_nodes++;
        }
    }

    err = prte_ras_slurm_walk_alloc_nodes(slurm_jobid, prte_ras_slurm_detach_count,
                                          prte_ras_slurm_detach_node, &detach, NULL);

    if(PRTE_SUCCESS != err) {
        goto cleanup;
    }

    /* success, clear out old entries and reconstruct the node list*/

    pmix_pointer_array_remove_all(session->nodes);

    for (i = 0; i < matched_nodes.size; i++) {
        prte_node_t *curr = (prte_node_t *)pmix_pointer_array_get_item(&matched_nodes, i);
        if(NULL != curr) {
            pmix_pointer_array_set_item(session->nodes, i, curr);
        }
    }

    pmix_pointer_array_remove_all(&matched_nodes);

    for (i = 0; i < unmatched_nodes.size; i++) {
        prte_node_t *curr = (prte_node_t *)pmix_pointer_array_get_item(&unmatched_nodes, i);
        if(NULL != curr) {
            pmix_pointer_array_add(removed_nodes, curr);
        }
    }

    pmix_pointer_array_remove_all(&unmatched_nodes);

cleanup:

    PMIX_DESTRUCT(&matched_nodes);
    PMIX_DESTRUCT(&unmatched_nodes);

    return err;
}
