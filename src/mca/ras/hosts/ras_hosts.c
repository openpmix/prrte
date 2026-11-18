/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

#include "src/class/pmix_list.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/state.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_node_index.h"
#include "src/util/proc_info.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/textfile.h"
#include "ras_hosts.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);
static pmix_status_t modify(prte_pmix_server_req_t *req);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_hosts_module = {
    .init = NULL,
    .allocate = allocate,
    .modify = modify,
    .finalize = finalize
};

/* Reporting a hard error is the DRIVER's job, not ours.
 *
 * prte_ras_base_allocate() reads anything outside its return protocol as a
 * real error and answers it with PRTE_ERROR_LOG plus
 * PRTE_ACTIVATE_JOB_STATE(PRTE_JOB_STATE_ALLOC_FAILED).  Two of the five
 * error paths below used to activate that state as well, so a bad hostfile
 * ran the DVM's whole failure teardown twice - two job_errors for the daemon
 * job, and with it two terminate_orteds()/DAEMONS_TERMINATED activations -
 * while the other three paths, and every other ras component, returned the
 * code and left the reporting alone.  Return the error; the driver has it. */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    int rc, i, j;
    char *hosts, **hostlist = NULL;
    bool check;
    prte_app_context_t *app;
    pmix_info_t *req = NULL;
    size_t nreq = 0;

    /* We first see if we were given a rank/seqfile - if so, use it
     * as the hosts will be taken from the mapping */
    hosts = NULL;
    check = prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING);
    if (check && NULL != hosts) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:hosts:allocate parsing rank/seqfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

        /* a rank/seqfile was provided - parse it, for the job's owner */
        rc = prte_requester_info_job(jdata, &req, &nreq);
        if (PRTE_SUCCESS == rc) {
            rc = prte_util_add_hostfile_nodes(nodes, hosts, req, nreq);
            PMIX_INFO_FREE(req, nreq);
        }
        if (PRTE_SUCCESS != rc) {
            free(hosts);
            return rc;
        }
        free(hosts);
    }

    /* if something was found in the rankfile, then we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        /* Record that the rankfile mapping policy has been selected */
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
            if (NULL == jdata->map) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
        }
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_GIVEN);
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYUSER);
        /* rankfile is considered equivalent to an RM allocation */
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        }
        return PRTE_SUCCESS;
    }

    /* if a dash-host has been provided, aggregate across all the
     * app_contexts. Any hosts the user wants to add via comm_spawn
     * can be done so using the add_host option */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL ==  app) {
            continue;
        }
        hosts = NULL;
        check = prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts, PMIX_STRING);
        if (check && NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding dash_hosts",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            rc = prte_requester_info_job(jdata, &req, &nreq);
            if (PRTE_SUCCESS == rc) {
                rc = prte_util_add_dash_host_nodes(nodes, hosts, req, nreq);
                PMIX_INFO_FREE(req, nreq);
            }
            if (PRTE_SUCCESS != rc) {
                free(hosts);
                return rc;
            }
            free(hosts);
        }
    }

    /* if something was found in the dash-host(s), then we are done */
    if (!pmix_list_is_empty(nodes)) {
        return PRTE_SUCCESS;
    }

    /* Our next option is to look for a hostfile and assign our global
     * pool from there.
     *
     * Individual hostfile names, if given, are included
     * in the app_contexts for this job. We therefore need to
     * retrieve the app_contexts for the job, and then cycle
     * through them to see if anything is there. The parser will
     * add the nodes found in each hostfile to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * in hostfiles from across all app_contexts
     *
     * Note that any relative node syntax found in the hostfiles will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding hostfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            hostlist = PMIx_Argv_split(hosts, ',');
            if (NULL == hostlist) {
                /* PMIx_Argv_split answers "nothing" with NULL, not with an
                 * empty array, so a value that is empty or all separators
                 * yields no tokens at all and indexing the result faults the
                 * DVM master before it has a node pool to fail over.  That is
                 * a live command line: the persistent branch of prte.c joins
                 * the option's values raw, so "prte --hostfile ''" and
                 * "prte --hostfile ," both arrive here as a string with no
                 * filename in it.  (The prterun branch absolutizes each value
                 * against the tool's cwd first, which is why only prte gets
                 * here with one.)  The user asked for specific hosts and named
                 * none, so say so rather than carrying on with an allocation
                 * they did not ask for. */
                prte_show_help_job(jdata, "help-hostfile.txt", "no-hostfile", true, hosts);
                free(hosts);
                return PRTE_ERR_SILENT;
            }
            free(hosts);
            /* read for the job's owner, who named them */
            rc = prte_requester_info_job(jdata, &req, &nreq);
            for (j = 0; PRTE_SUCCESS == rc && NULL != hostlist[j]; j++) {
                rc = prte_util_add_hostfile_nodes(nodes, hostlist[j], req, nreq);
            }
            if (NULL != req) {
                PMIX_INFO_FREE(req, nreq);
            }
            PMIx_Argv_free(hostlist);
            if (PRTE_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* if something was found in the hostfile(s), then we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        return PRTE_SUCCESS;
    }

    /* if nothing was found so far, then look for a default hostfile */
    if (NULL != prte_default_hostfile) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing default hostfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_default_hostfile));

        /* a default hostfile was provided - parse it. It is the DVM's own
         * configuration, so read as ourselves */
        if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(nodes, prte_default_hostfile,
                                                               NULL, 0))) {
            return rc;
        }
    }

    /* if something was found in the default hostfile, then we are done */
    if (!pmix_list_is_empty(nodes)) {
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:hosts:allocate nothing found in hosts",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}

/* Apply a "slots=+N" / "slots=-N" adjustment to a node that is already in the
 * global pool.
 *
 * Two things the open-coded version got wrong. It clamped at zero but not at
 * slots_max, so an adjustment could push a node above the ceiling the
 * allocation gave it - unlike every other slot adjustment in the framework
 * (prte_ras_base_node_insert clamps both ends for PRTE_NODE_ADD_SLOTS). And it
 * never told prte_ras_base.total_slots_alloc, so the framework's own running
 * total drifted from the pool it was describing.  Note what that total is
 * NOT: a job's PMIX_UNIV_SIZE / PMIX_MAX_PROCS is jdata->total_slots_alloc,
 * which prte_plm_base_daemons_reported recomputes from the job's own session
 * nodes and which therefore does not come from here. */
static void adjust_slots(prte_node_t *nptr, int slots)
{
    int32_t before = nptr->slots;
    /* sum in a type that cannot overflow on the inputs: both operands are
     * int32_t and the clamp below cannot rescue a sum that has already
     * overflowed - signed overflow is undefined, not a value to be clamped.
     * int64_t rather than long, which is 32 bits in an ILP32 build. */
    int64_t adjusted = (int64_t) before + (int64_t) slots;

    if (0 > adjusted) {
        adjusted = 0;
    } else if (0 < nptr->slots_max && adjusted > (int64_t) nptr->slots_max) {
        adjusted = nptr->slots_max;
    }
    nptr->slots = (int32_t) adjusted;
    prte_ras_base.total_slots_alloc += (nptr->slots - before);
}

/* Read a hostfile slot count.
 *
 * The bare strtol() this replaces answered 0 for anything that is not a
 * number, and 0 is a meaningful count here: an unrecognized "slots=" value
 * gave the node zero slots AND set PRTE_NODE_FLAG_SLOTS_GIVEN, so it joined
 * the DVM able to run nothing and was never re-sized from its topology.  It
 * also truncated a long into an int, which matters because the sign is what
 * the caller acts on: a value too large to fit could arrive with the sign the
 * user did not write, and "a new node may not be given negative slots" is a
 * refusal that then does not fire.
 *
 * Trailing text after the digits is tolerated only as whitespace, which is
 * what the bare strtol() effectively did - this parser reads one keyword per
 * line and ignores whatever follows the value. */
static bool parse_slots(const char *value, int *result)
{
    char *endptr = NULL;
    long l;

    if ('+' != value[0] && '-' != value[0] &&
        !isdigit((unsigned char) value[0])) {
        return false;
    }
    errno = 0;
    l = strtol(value, &endptr, 10);
    if (0 != errno || NULL == endptr || endptr == value) {
        return false;
    }
    if ('\0' != *endptr && !isspace((unsigned char) *endptr)) {
        return false;
    }
    if (INT32_MAX < l || INT32_MIN > l) {
        return false;
    }
    *result = (int) l;
    return true;
}

/* Read the hosts a request adds from hostfile, for the requester rq
 * describes: as their user and group, and what is wrong with the file is
 * told to them - the tool behind the job a spawn's --add-hostfile came
 * with, or that made the allocation request. */
static pmix_status_t read_hostfile(const pmix_info_t *rq, size_t nrq, char *hostfile,
                                   pmix_list_t *nodes, prte_node_index_t *pidx)
{
    uid_t uid;
    gid_t gid;
    prte_textfile_t tf;
    char *line, *cptr, *ptr, *ndname;
    bool addslots, failed = false;
    int slots, rc;
    prte_node_t *nptr, *node;

    /* We don't use the hostfile parsing code in src/util because it
     * has a stricter idea of the range of allowed
     * syntax here. The file is still opened the way that code opens
     * one: this runs on the master's progress thread, and a bare fopen()
     * of a FIFO with no writer would block that thread for good, so the
     * open is non-blocking and only a regular file is read - and it is
     * made as whoever named the file */
    prte_requester_ids(rq, nrq, &uid, &gid);
    rc = prte_textfile_open(&tf, hostfile, uid, gid);
    if (PRTE_ERR_BAD_PARAM == rc) {
        prte_show_help_info(rq, nrq, "help-ras-base.txt",
                       "ras-base:addhost-not-a-file", true, hostfile);
        return PMIX_ERR_SILENT;
    }
    if (PRTE_SUCCESS != rc) {
        prte_show_help_info(rq, nrq, "help-ras-base.txt", "ras-base:addhost-not-found", true,
                            hostfile);
        return PMIX_ERR_SILENT;
    }
    /* a line at a time, within the limits every file we read is held to
     * (prte_textfile_getline) */
    while (NULL != (line = prte_textfile_getline(&tf))) {
        // ignore comments and blank lines
        if (0 == strlen(line)) {
            free(line);
            continue;
        }
        // remove leading whitespace. NOTE: isspace() takes an int whose
        // value must be representable as unsigned char (or EOF); passing a
        // plain char is undefined for any byte with the high bit set, so
        // every ctype call here casts.
        cptr = line;
        while (isspace((unsigned char) *cptr)) {
            ++cptr;
        }
        if ('#' == *cptr) {
            free(line);
            continue;
        }
        addslots = false;
        // because there can be arbitrary whitespace around keywords,
        // we manually parse the line to get the directives
        ptr = cptr;
        while ('\0' != *ptr && !isspace((unsigned char) *ptr)) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            // end of the line - just the node name was given
            slots = -1;
            goto process;
        }
        *ptr = '\0'; // terminate the name
        // find the '=' sign
        ++ptr;
        while ('\0' != *ptr && '=' != *ptr) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            // didn't specify slots - use the default value
            slots = -1;
            goto process;
        }
        // find the value
        ++ptr;
        while ('\0' != *ptr && isspace((unsigned char) *ptr)) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            // bad syntax
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            prte_textfile_close(&tf);
            free(line);
            return PMIX_ERR_SILENT;
        }
        // if it is a '+' or '-', then we are adjusting
        // the #slots
        if ('+' == *ptr || '-' == *ptr) {
            addslots = true;
        }
        if (!parse_slots(ptr, &slots)) {
            prte_show_help_info(rq, nrq, "help-ras-base.txt", "ras-base:bad-slots", true,
                           cptr, hostfile, ptr);
            prte_textfile_close(&tf);
            free(line);
            return PMIX_ERR_BAD_PARAM;
        }

process:
        /* "user@host" used to name an account to reach the host as, and
         * the launcher logged in to it as that account. The DVM reaches
         * every node as its own user and runs no daemon as another, so such
         * an entry is refused, as it is in a --host list - not reached as
         * our own user instead, which is not what the file asked for. The
         * name goes on the launch agent's command line, so it must be one
         * a host can have. */
        ndname = cptr;
        if (NULL != strchr(ndname, '@')) {
            prte_show_help_info(rq, nrq, "help-ras-base.txt",
                           "ras-base:addhost-user-host", true, cptr, hostfile);
            prte_textfile_close(&tf);
            free(line);
            return PMIX_ERR_BAD_PARAM;
        }
        if (!prte_util_host_name_ok(ndname)) {
            prte_show_help_info(rq, nrq, "help-ras-base.txt",
                           "ras-base:addhost-bad-name", true, cptr, hostfile);
            prte_textfile_close(&tf);
            free(line);
            return PMIX_ERR_SILENT;
        }

        /* See if we already have this node.
         *
         * prte_node_match() is the one place that knows how a name reaches a
         * pool entry: it resolves a name that refers to this host to our
         * canonical nodename, searches the per-node alias lists, and tolerates
         * an entry that carries no name at all.  This used to spell that walk
         * out for itself and had already drifted from it - it compared
         * nptr->name with no NULL check, which is the same drift the framework
         * guide records for --display-cpus. */
        nptr = prte_node_index_match_name(pidx, ndname);
        if (NULL != nptr) {
            if (addslots) {
                adjust_slots(nptr, slots);
            }
        } else {
            // this is a new node - add it
            node = PMIX_NEW(prte_node_t);
            node->name = strdup(ndname);
            node->state = PRTE_NODE_STATE_ADDED;
            if (0 <= slots) {
                /* they gave us the number of slots, so set it - including an
                 * explicit "slots=0", which means this node contributes none
                 * and must NOT be silently re-sized from its core count.
                 * Only the -1 marker (no slots clause at all) leaves the
                 * count to be computed when the daemon reports its topology. */
                node->slots = slots;
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
            } else if (0 > slots && -1 != slots) {
                // cannot have a new node with negative slots - the -1
                // is a marker for a node without slots being specified
                prte_show_help_info(rq, nrq, "help-ras-base.txt", "negative-slots", true,
                               ndname, slots);
                PMIX_RELEASE(node);
                free(line);
                prte_textfile_close(&tf);
                return PMIX_ERR_BAD_PARAM;
            }
            pmix_list_append(nodes, &node->super);
        }
        free(line);
    }
    failed = tf.failed;
    prte_textfile_close(&tf);
    if (failed) {
        if (!prte_textfile_report_limit(&tf, rq, nrq, hostfile)) {
            prte_show_help_info(rq, nrq, "help-prte-util.txt", "file-read-failed", true, hostfile);
        }
        return PMIX_ERR_SILENT;
    }
    return PMIX_SUCCESS;
}

/* Each line is looked up in the node pool, so look it up in an index of
 * the pool (prte_node_index_t) rather than walking the pool once per line,
 * which made a long file quadratic. Nothing here changes the pool: new
 * nodes go on the caller's list, for prte_ras_base_node_insert(). */
static pmix_status_t process_hostfile(const pmix_info_t *rq, size_t nrq, char *hostfile,
                                      pmix_list_t *nodes)
{
    prte_node_index_t pidx;
    pmix_status_t rc;

    prte_node_index_construct(&pidx);
    if (PRTE_SUCCESS != prte_node_index_add_pool(&pidx)) {
        prte_node_index_destruct(&pidx);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    rc = read_hostfile(rq, nrq, hostfile, nodes, &pidx);
    prte_node_index_destruct(&pidx);
    return rc;
}

/* Whether a PMIX_ALLOC_NEW creates a reservation, and so needs an id: an
 * add-host grows the general pool, and a request to share the nodes puts
 * them there (see ras_base_prepare_grow), and neither has an allocation to
 * name. */
static bool makes_reservation(const prte_pmix_server_req_t *req)
{
    size_t n;

    for (n = 0; n < req->ninfo; n++) {
        if (PMIx_Check_key(req->info[n].key, PMIX_ADD_HOST) ||
            PMIx_Check_key(req->info[n].key, PMIX_ADD_HOSTFILE)) {
            return false;
        }
#if defined(PMIX_ALLOC_SHARE)
        if (PMIx_Check_key(req->info[n].key, PMIX_ALLOC_SHARE) &&
            PMIX_INFO_TRUE(&req->info[n])) {
            return false;
        }
#endif
    }
    return true;
}

/* With no scheduler, this component grants the allocation, so it assigns
 * the allocation's PMIX_ALLOC_ID - as a scheduler's answer would carry one.
 * The ids are numbers counted from 1, skipping any already naming a
 * reservation. The id is added to the request's info, where
 * prte_ras_base_complete_request() records it with the reservation, along
 * with the requester's PMIX_ALLOC_REQ_ID, and returns both. */
static uint32_t hosts_alloc_counter = 0;

static pmix_status_t assign_alloc_id(prte_pmix_server_req_t *req)
{
    char id[16];

    do {
        if (0 == ++hosts_alloc_counter) {
            hosts_alloc_counter = 1;
        }
        snprintf(id, sizeof(id), "%u", hosts_alloc_counter);
    } while (prte_session_alloc_id_in_use(id));

    return prte_ras_base_add_alloc_id(req, id);
}

static pmix_status_t modify_for(prte_pmix_server_req_t *req, const pmix_info_t *rq, size_t nrq)
{
    int rc;
    pmix_list_t nodes;
    size_t n, k;
    char **hostfiles;
    bool handled = false;

    PMIX_CONSTRUCT(&nodes, pmix_list_t);

    // look for applicable directives
    for (n=0; n < req->ninfo; n++) {
        if (PMIx_Check_key(req->info[n].key, PMIX_ADD_HOSTFILE)) {
            /* the value has to be a string we can split - a request that
             * arrived over the wire may carry anything */
            if (PMIX_STRING != req->info[n].value.type ||
                NULL == req->info[n].value.data.string) {
                PMIX_LIST_DESTRUCT(&nodes);
                req->pstatus = PMIX_ERR_BAD_PARAM;
                return req->pstatus;
            }
            // comma-delimited list of hostfiles to add or delete
            hostfiles = PMIx_Argv_split(req->info[n].value.data.string, ',');
            if (NULL == hostfiles) {
                continue;
            }
            for (k=0; NULL != hostfiles[k]; k++) {
                rc = process_hostfile(rq, nrq, hostfiles[k], &nodes);
                if (PMIX_SUCCESS != rc) {
                    PMIX_LIST_DESTRUCT(&nodes);
                    PMIx_Argv_free(hostfiles);
                    req->pstatus = rc;
                    return rc;
                }
            }
            PMIx_Argv_free(hostfiles);
            handled = true;
        }
        if (PMIx_Check_key(req->info[n].key, PMIX_ADD_HOST)) {
            pmix_list_t dhnodes;
            prte_node_t *nd;

            if (PMIX_STRING != req->info[n].value.type ||
                NULL == req->info[n].value.data.string) {
                PMIX_LIST_DESTRUCT(&nodes);
                req->pstatus = PMIX_ERR_BAD_PARAM;
                return req->pstatus;
            }
            // comma-delimited list of hosts to add or delete
            PMIX_CONSTRUCT(&dhnodes, pmix_list_t);
            rc = prte_util_add_dash_host_nodes(&dhnodes, req->info[n].value.data.string, rq, nrq);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&dhnodes);
                PMIX_LIST_DESTRUCT(&nodes);
                req->pstatus = prte_pmix_convert_rc(rc);
                return req->pstatus;
            }
            /* mark these as newly added so the DVM extension will
             * include them despite any static -host filter given
             * when the DVM was started - the hostfile parser above
             * already does this for its new nodes */
            while (NULL != (nd = (prte_node_t *) pmix_list_remove_first(&dhnodes))) {
                nd->state = PRTE_NODE_STATE_ADDED;
                pmix_list_append(&nodes, &nd->super);
            }
            PMIX_DESTRUCT(&dhnodes);
            handled = true;
        }
    }

    if (0 < pmix_list_get_size(&nodes)) {
        /* mark that an updated nidmap must be communicated to existing daemons */
        prte_nidmap_communicated = false;
        rc = prte_ras_base_node_insert(&nodes, req->jdata);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            /* node_insert drains what it consumed; destruct so whatever it
             * did not reach is not leaked along with the list itself */
            PMIX_LIST_DESTRUCT(&nodes);
            req->pstatus = prte_pmix_convert_rc(rc);
            return req->pstatus;
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);

    /* When no external scheduler is present, this component is the DVM's local
     * resource authority for elastic operations.  Claim the size-change
     * directives so the base prte_ras_base_complete_request() logic runs with
     * the ORIGINAL request info intact:
     *   - PMIX_ALLOC_NEW / PMIX_ALLOC_EXTEND carrying PMIX_ALLOC_NODE_LIST add
     *     the named nodes and extend the DVM;
     *   - PMIX_ALLOC_RELEASE removes the named nodes (PMIX_ALLOC_NODE_LIST) or
     *     tears down a whole reservation (PMIX_ALLOC_ID).
     * Keeping the original request info intact preserves the node list and the
     * allocation ids, so a release can target a specific reservation.
     *
     * It also makes our answer say whom the allocation is for, as every
     * module's must: there is no scheduler here to ask, so the allocation is
     * granted for exactly the PMIX_ALLOC_UID/GID the request carries, which
     * prte_ras_base_alloc_owner checked or filled in before we were called.
     *
     * A PMIX_ALLOC_NEW that creates a reservation is given its PMIX_ALLOC_ID
     * here (assign_alloc_id). */
    switch (req->allocdir) {
    case PMIX_ALLOC_NEW:
        if (makes_reservation(req)) {
            rc = assign_alloc_id(req);
            if (PMIX_SUCCESS != rc) {
                req->pstatus = rc;
                return rc;
            }
        }
        handled = true;
        break;
    case PMIX_ALLOC_EXTEND:
    case PMIX_ALLOC_RELEASE:
        handled = true;
        break;
    default:
        break;
    }

    /* If we satisfied something, let the base layer finish it; otherwise defer
     * to the next module (this component is the lowest-priority RAS). */
    if (handled) {
        return PMIX_OPERATION_SUCCEEDED;
    }
    return PMIX_ERR_TAKE_NEXT_OPTION;
}

/* A file or host list a request names is read as whoever named it - the
 * owner of the job a spawn's add-hostfile came with, or the requester of
 * an allocation - and what is wrong with it is told to them. */
static pmix_status_t modify(prte_pmix_server_req_t *req)
{
    pmix_info_t *rq;
    size_t nrq;
    pmix_status_t rc;
    uid_t uid;
    gid_t gid;
    int prc;

    if (NULL != req->jdata) {
        prc = prte_requester_info_job(req->jdata, &rq, &nrq);
    } else {
        prte_pmix_server_request_user(req->info, req->ninfo, &uid, &gid);
        prc = prte_requester_info_proc(&req->tproc, uid, gid, &rq, &nrq);
    }
    if (PRTE_SUCCESS != prc) {
        req->pstatus = prte_pmix_convert_rc(prc);
        return req->pstatus;
    }
    rc = modify_for(req, rq, nrq);
    PMIX_INFO_FREE(rq, nrq);
    return rc;
}
