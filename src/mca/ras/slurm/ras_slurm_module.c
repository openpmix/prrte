/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/argv.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/include/prrte_socket_errno.h"

#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ras/base/ras_private.h"
#include "ras_slurm.h"

#define PRRTE_SLURM_DYN_MAX_SIZE 256

/*
 * API functions
 */
static int init(void);
static int prrte_ras_slurm_allocate(prrte_job_t *jdata, prrte_list_t *nodes);
static void deallocate(prrte_job_t *jdata,
                       prrte_app_context_t *app);
static int prrte_ras_slurm_finalize(void);

/*
 * RAS slurm module
 */
prrte_ras_base_module_t prrte_ras_slurm_module = {
    init,
    prrte_ras_slurm_allocate,
    deallocate,
    prrte_ras_slurm_finalize
};

/* Local functions */
static int prrte_ras_slurm_discover(char *regexp, char* tasks_per_node,
                                   prrte_list_t *nodelist);
static int prrte_ras_slurm_parse_ranges(char *base, char *ranges, char ***nodelist);
static int prrte_ras_slurm_parse_range(char *base, char *range, char ***nodelist);

static int dyn_allocate(prrte_job_t *jdata);
static char* get_node_list(prrte_app_context_t *app);
static int parse_alloc_msg(char *msg, int *idx, int *sjob,
                           char **nodelist, char **tpn);

static void recv_data(int fd, short args, void *cbdata);
static void timeout(int fd, short args, void *cbdata);
static int read_ip_port(char *filename, char **ip, uint16_t *port);


/* define structs for tracking dynamic allocations */
typedef struct {
    prrte_object_t super;
    int sjob;
} local_apptracker_t;
PRRTE_CLASS_INSTANCE(local_apptracker_t,
                   prrte_object_t,
                   NULL, NULL);

typedef struct {
    prrte_list_item_t super;
    char *cmd;
    prrte_event_t timeout_ev;
    prrte_jobid_t jobid;
    prrte_pointer_array_t apps;
    int napps;
} local_jobtracker_t;
static void jtrk_cons(local_jobtracker_t *ptr)
{
    ptr->cmd = NULL;
    PRRTE_CONSTRUCT(&ptr->apps, prrte_pointer_array_t);
    prrte_pointer_array_init(&ptr->apps, 1, INT_MAX, 1);
    ptr->napps = 0;
}
static void jtrk_des(local_jobtracker_t *ptr)
{
    int i;
    local_apptracker_t *ap;

    if (NULL != ptr->cmd) {
        free(ptr->cmd);
    }
    for (i=0; i < ptr->apps.size; i++) {
        if (NULL != (ap = (local_apptracker_t*)prrte_pointer_array_get_item(&ptr->apps, i))) {
            PRRTE_RELEASE(ap);
        }
    }
    PRRTE_DESTRUCT(&ptr->apps);
}
PRRTE_CLASS_INSTANCE(local_jobtracker_t,
                   prrte_list_item_t,
                   jtrk_cons, jtrk_des);

/* local vars */
static int socket_fd;
static prrte_list_t jobs;
static prrte_event_t recv_ev;

/* init the module */
static int init(void)
{
    char *slurm_host=NULL;
    uint16_t port=0;
    struct sockaddr_in address;
    int flags;
    struct hostent *h;

    if (prrte_ras_slurm_component.dyn_alloc_enabled) {
        if (NULL == prrte_ras_slurm_component.config_file) {
            prrte_show_help("help-ras-slurm.txt", "dyn-alloc-no-config", true);
            return PRRTE_ERR_SILENT;
        }
        /* setup the socket */
        if (PRRTE_SUCCESS != read_ip_port(prrte_ras_slurm_component.config_file,
                                         &slurm_host, &port) ||
            NULL == slurm_host || 0 == port) {
            if (NULL != slurm_host) {
                free(slurm_host);
            }
            return PRRTE_ERR_SILENT;
        }
        PRRTE_OUTPUT_VERBOSE((2, prrte_ras_base_framework.framework_output,
                             "ras:slurm got [ ip = %s, port = %u ] from %s\n",
                             slurm_host, port, prrte_ras_slurm_component.config_file));

        /* obtain a socket for our use */
        if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            free(slurm_host);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* connect to the Slurm dynamic allocation port */
        bzero(&address, sizeof(address));
        address.sin_family = AF_INET;
        if (!prrte_net_isaddr(slurm_host)) {
            /* if the ControlMachine was not specified as an IP address,
             * we need to resolve it here
             */
            if (NULL == (h = gethostbyname(slurm_host))) {
                /* could not resolve it */
                prrte_show_help("help-ras-slurm.txt", "host-not-resolved",
                               true, slurm_host);
                free(slurm_host);
                return PRRTE_ERR_SILENT;
            }
            free(slurm_host);
            slurm_host = strdup(inet_ntoa(*(struct in_addr*)h->h_addr_list[0]));
        }
        address.sin_addr.s_addr = inet_addr(slurm_host);
        address.sin_port =  htons(port);
        if (connect(socket_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            prrte_show_help("help-ras-slurm.txt", "connection-failed",
                           true, slurm_host, (int)port);
            free(slurm_host);
            return PRRTE_ERR_SILENT;
        }
        free(slurm_host);

        /* set socket up to be non-blocking */
        if ((flags = fcntl(socket_fd, F_GETFL, 0)) < 0) {
            prrte_output(0, "ras:slurm:dyn: fcntl(F_GETFL) failed: %s (%d)",
                        strerror(prrte_socket_errno), prrte_socket_errno);
            return PRRTE_ERROR;
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(socket_fd, F_SETFL, flags) < 0) {
                prrte_output(0, "ras:slurm:dyn: fcntl(F_SETFL) failed: %s (%d)",
                            strerror(prrte_socket_errno), prrte_socket_errno);
                return PRRTE_ERROR;
            }
        }

        /* setup to recv data */
        prrte_event_set(prrte_event_base, &recv_ev, socket_fd,
                       PRRTE_EV_READ, recv_data, NULL);
        prrte_event_add(&recv_ev, 0);

        /* initialize the list of jobs for tracking dynamic allocations */
        PRRTE_CONSTRUCT(&jobs, prrte_list_t);
    }
    return PRRTE_SUCCESS;
}

/**
 * Discover available (pre-allocated) nodes.  Allocate the
 * requested number of nodes/process slots to the job.
 *
 */
static int prrte_ras_slurm_allocate(prrte_job_t *jdata, prrte_list_t *nodes)
{
    int ret, cpus_per_task;
    char *slurm_node_str, *regexp;
    char *tasks_per_node, *node_tasks;
    char *tmp;
    char *slurm_jobid;

    if (NULL == (slurm_jobid = getenv("SLURM_JOBID"))) {
        /* we are not in a slurm allocation - see if dyn alloc
         * is enabled
         */
        if (!prrte_ras_slurm_component.dyn_alloc_enabled) {
            /* nope - nothing we can do */
            prrte_output_verbose(2, prrte_ras_base_framework.framework_output,
                                "%s ras:slurm: no prior allocation and dynamic alloc disabled",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    } else {
        /* save this value in the global job ident string for
         * later use in any error reporting
         */
        prrte_job_ident = strdup(slurm_jobid);
    }

    slurm_node_str = getenv("SLURM_NODELIST");
    if (NULL == slurm_node_str) {
        /* see if dynamic allocation is enabled */
        if (prrte_ras_slurm_component.dyn_alloc_enabled) {
            /* attempt to get the allocation - the function
             * dyn_allocate will return as PRRTE_ERR_ALLOCATION_PENDING
             * if it succeeds in sending the allocation request
             */
            ret = dyn_allocate(jdata);
            /* return to the above layer in ras/base/ras_base_allocate.c
             * to wait for event (libevent) happening
             */
            return ret;
        }
        prrte_show_help("help-ras-slurm.txt", "slurm-env-var-not-found", 1,
                       "SLURM_NODELIST");
        return PRRTE_ERR_NOT_FOUND;
    }
    regexp = strdup(slurm_node_str);
    if(NULL == regexp) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    if (prrte_ras_slurm_component.use_all) {
        /* this is an oddball case required for debug situations where
         * a tool is started that will then call mpirun. In this case,
         * Slurm will assign only 1 tasks/per node to the tool, but
         * we want mpirun to use the entire allocation. They don't give
         * us a specific variable for this purpose, so we have to fudge
         * a bit - but this is a special edge case, and we'll live with it */
        tasks_per_node = getenv("SLURM_JOB_CPUS_PER_NODE");
        if (NULL == tasks_per_node) {
            /* couldn't find any version - abort */
            prrte_show_help("help-ras-slurm.txt", "slurm-env-var-not-found", 1,
                           "SLURM_JOB_CPUS_PER_NODE");
            free(regexp);
            return PRRTE_ERR_NOT_FOUND;
        }
        node_tasks = strdup(tasks_per_node);
        if (NULL == node_tasks) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            free(regexp);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        cpus_per_task = 1;
    } else {
        /* get the number of process slots we were assigned on each node */
        tasks_per_node = getenv("SLURM_TASKS_PER_NODE");
        if (NULL == tasks_per_node) {
            /* couldn't find any version - abort */
            prrte_show_help("help-ras-slurm.txt", "slurm-env-var-not-found", 1,
                           "SLURM_TASKS_PER_NODE");
            free(regexp);
            return PRRTE_ERR_NOT_FOUND;
        }
        node_tasks = strdup(tasks_per_node);
        if (NULL == node_tasks) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            free(regexp);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* get the number of CPUs per task that the user provided to slurm */
        tmp = getenv("SLURM_CPUS_PER_TASK");
        if(NULL != tmp) {
            cpus_per_task = atoi(tmp);
            if(0 >= cpus_per_task) {
                prrte_output(0, "ras:slurm:allocate: Got bad value from SLURM_CPUS_PER_TASK. "
                            "Variable was: %s\n", tmp);
                PRRTE_ERROR_LOG(PRRTE_ERROR);
                free(node_tasks);
                free(regexp);
                return PRRTE_ERROR;
            }
        } else {
            cpus_per_task = 1;
        }
    }

    ret = prrte_ras_slurm_discover(regexp, node_tasks, nodes);
    free(regexp);
    free(node_tasks);
    if (PRRTE_SUCCESS != ret) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                             "%s ras:slurm:allocate: discover failed!",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        return ret;
    }
    /* record the number of allocated nodes */
    prrte_num_allocated_nodes = prrte_list_get_size(nodes);

    /* All done */

    PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                         "%s ras:slurm:allocate: success",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
    return PRRTE_SUCCESS;
}

static void deallocate(prrte_job_t *jdata,
                       prrte_app_context_t *app)
{
}

static int prrte_ras_slurm_finalize(void)
{
    prrte_list_item_t *item;

    if (prrte_ras_slurm_component.dyn_alloc_enabled) {
        /* delete the recv event */
        prrte_event_del(&recv_ev);
        while (NULL != (item = prrte_list_remove_first(&jobs))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&jobs);
        /* close the socket */
        shutdown(socket_fd, 2);
        close(socket_fd);
    }
    return PRRTE_SUCCESS;
}


/**
 * Discover the available resources.
 *
 * In order to fully support slurm, we need to be able to handle
 * node regexp/task_per_node strings such as:
 * foo,bar    5,3
 * foo        5
 * foo[2-10,12,99-105],bar,foobar[3-11] 2(x10),5,100(x16)
 *
 * @param *regexp A node regular expression from SLURM (i.e. SLURM_NODELIST)
 * @param *tasks_per_node A tasks per node expression from SLURM
 *                        (i.e. SLURM_TASKS_PER_NODE)
 * @param *nodelist A list which has already been constucted to return
 *                  the found nodes in
 */
static int prrte_ras_slurm_discover(char *regexp, char *tasks_per_node,
                                   prrte_list_t* nodelist)
{
    int i, j, len, ret, count, reps, num_nodes;
    char *base, **names = NULL;
    char *begptr, *endptr, *orig;
    int *slots;
    bool found_range = false;
    bool more_to_come = false;
    char *ptr;

    orig = base = strdup(regexp);
    if (NULL == base) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                         "%s ras:slurm:allocate:discover: checking nodelist: %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         regexp));

    do {
        /* Find the base */
        len = strlen(base);
        for (i = 0; i <= len; ++i) {
            if (base[i] == '[') {
                /* we found a range. this gets dealt with below */
                base[i] = '\0';
                found_range = true;
                break;
            }
            if (base[i] == ',') {
                /* we found a singleton node, and there are more to come */
                base[i] = '\0';
                found_range = false;
                more_to_come = true;
                break;
            }
            if (base[i] == '\0') {
                /* we found a singleton node */
                found_range = false;
                more_to_come = false;
                break;
            }
        }
        if(i == 0) {
            /* we found a special character at the beginning of the string */
            prrte_show_help("help-ras-slurm.txt", "slurm-env-var-bad-value",
                           1, regexp, tasks_per_node, "SLURM_NODELIST");
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            free(orig);
            return PRRTE_ERR_BAD_PARAM;
        }

        if (found_range) {
            /* If we found a range, now find the end of the range */
            for (j = i; j < len; ++j) {
                if (base[j] == ']') {
                    base[j] = '\0';
                    break;
                }
            }
            if (j >= len) {
                /* we didn't find the end of the range */
                prrte_show_help("help-ras-slurm.txt", "slurm-env-var-bad-value",
                               1, regexp, tasks_per_node, "SLURM_NODELIST");
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                free(orig);
                return PRRTE_ERR_BAD_PARAM;
            }

            ret = prrte_ras_slurm_parse_ranges(base, base + i + 1, &names);
            if(PRRTE_SUCCESS != ret) {
                prrte_show_help("help-ras-slurm.txt", "slurm-env-var-bad-value",
                               1, regexp, tasks_per_node, "SLURM_NODELIST");
                PRRTE_ERROR_LOG(ret);
                free(orig);
                return ret;
            }
            if(base[j + 1] == ',') {
                more_to_come = true;
                base = &base[j + 2];
            } else {
                more_to_come = false;
            }
        } else {
            /* If we didn't find a range, just add the node */

            PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                                 "%s ras:slurm:allocate:discover: found node %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 base));

            if(PRRTE_SUCCESS != (ret = prrte_argv_append_nosize(&names, base))) {
                PRRTE_ERROR_LOG(ret);
                free(orig);
                return ret;
            }
            /* set base equal to the (possible) next base to look at */
            base = &base[i + 1];
        }
    } while(more_to_come);

    free(orig);

    num_nodes = prrte_argv_count(names);

    /* Find the number of slots per node */

    slots = malloc(sizeof(int) * num_nodes);
    if (NULL == slots) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    memset(slots, 0, sizeof(int) * num_nodes);

    orig = begptr = strdup(tasks_per_node);
    if (NULL == begptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        free(slots);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    j = 0;
    while (begptr) {
        count = strtol(begptr, &endptr, 10);
        if ((endptr[0] == '(') && (endptr[1] == 'x')) {
            reps = strtol((endptr+2), &endptr, 10);
            if (endptr[0] == ')') {
                endptr++;
            }
        } else {
            reps = 1;
        }

        /**
         * TBP: it seems like it would be an error to have more slot
         * descriptions than nodes. Turns out that this valid, and SLURM will
         * return such a thing. For instance, if I did:
         * srun -A -N 30 -w odin001
         * I would get SLURM_NODELIST=odin001 SLURM_TASKS_PER_NODE=4(x30)
         * That is, I am allocated 30 nodes, but since I only requested
         * one specific node, that's what is in the nodelist.
         * I'm not sure this is what users would expect, but I think it is
         * more of a SLURM issue than a prrte issue, since SLURM is OK with it,
         * I'm ok with it
         */
        for (i = 0; i < reps && j < num_nodes; i++) {
            slots[j++] = count;
        }

        if (*endptr == ',') {
            begptr = endptr + 1;
        } else if (*endptr == '\0' || j >= num_nodes) {
            break;
        } else {
            prrte_show_help("help-ras-slurm.txt", "slurm-env-var-bad-value", 1,
                           regexp, tasks_per_node, "SLURM_TASKS_PER_NODE");
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            free(slots);
            free(orig);
            return PRRTE_ERR_BAD_PARAM;
        }
    }

    free(orig);

    /* Convert the argv of node names to a list of node_t's */

    for (i = 0; NULL != names && NULL != names[i]; ++i) {
        prrte_node_t *node;

        if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(names[i]) ) {
            if (NULL != (ptr = strchr(names[i], '.'))) {
                *ptr = '\0';
            }
        }

        PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                             "%s ras:slurm:allocate:discover: adding node %s (%d slot%s)",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             names[i], slots[i], (1 == slots[i]) ? "" : "s"));

        node = PRRTE_NEW(prrte_node_t);
        if (NULL == node) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            free(slots);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        node->name = strdup(names[i]);
        node->state = PRRTE_NODE_STATE_UP;
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = slots[i];
        prrte_list_append(nodelist, &node->super);
    }
    free(slots);
    prrte_argv_free(names);

    /* All done */
    return ret;
}


/*
 * Parse one or more ranges in a set
 *
 * @param base     The base text of the node name
 * @param *ranges  A pointer to a range. This can contain multiple ranges
 *                 (i.e. "1-3,10" or "5" or "9,0100-0130,250")
 * @param ***names An argv array to add the newly discovered nodes to
 */
static int prrte_ras_slurm_parse_ranges(char *base, char *ranges, char ***names)
{
    int i, len, ret;
    char *start, *orig;

    /* Look for commas, the separator between ranges */

    len = strlen(ranges);
    for (orig = start = ranges, i = 0; i < len; ++i) {
        if (',' == ranges[i]) {
            ranges[i] = '\0';
            ret = prrte_ras_slurm_parse_range(base, start, names);
            if (PRRTE_SUCCESS != ret) {
                PRRTE_ERROR_LOG(ret);
                return ret;
            }
            start = ranges + i + 1;
        }
    }

    /* Pick up the last range, if it exists */

    if (start < orig + len) {

        PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                             "%s ras:slurm:allocate:discover: parse range %s (2)",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             start));

        ret = prrte_ras_slurm_parse_range(base, start, names);
        if (PRRTE_SUCCESS != ret) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    /* All done */
    return PRRTE_SUCCESS;
}


/*
 * Parse a single range in a set and add the full names of the nodes
 * found to the names argv
 *
 * @param base     The base text of the node name
 * @param *ranges  A pointer to a single range. (i.e. "1-3" or "5")
 * @param ***names An argv array to add the newly discovered nodes to
 */
static int prrte_ras_slurm_parse_range(char *base, char *range, char ***names)
{
    char *str, temp1[BUFSIZ];
    size_t i, j, start, end;
    size_t base_len, len, num_len;
    size_t num_str_len;
    bool found;
    int ret;

    len = strlen(range);
    base_len = strlen(base);
    /* Silence compiler warnings; start and end are always assigned
       properly, below */
    start = end = 0;

    /* Look for the beginning of the first number */

    for (found = false, i = 0; i < len; ++i) {
        if (isdigit((int) range[i])) {
            if (!found) {
                start = atoi(range + i);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }

    /* Look for the end of the first number */

    for (found = false, num_str_len = 0; i < len; ++i, ++num_str_len) {
        if (!isdigit((int) range[i])) {
            break;
        }
    }

    /* Was there no range, just a single number? */

    if (i >= len) {
        end = start;
        found = true;
    }

    /* Nope, there was a range.  Look for the beginning of the second
       number */

    else {
        for (; i < len; ++i) {
            if (isdigit((int) range[i])) {
                end = atoi(range + i);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }

    /* Make strings for all values in the range */

    len = base_len + num_str_len + 32;
    str = malloc(len);
    if (NULL == str) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    strcpy(str, base);
    for (i = start; i <= end; ++i) {
        str[base_len] = '\0';
        snprintf(temp1, BUFSIZ - 1, "%lu", (long) i);

        /* Do we need zero pading? */

        if ((num_len = strlen(temp1)) < num_str_len) {
            for (j = base_len; j < base_len + (num_str_len - num_len); ++j) {
                str[j] = '0';
            }
            str[j] = '\0';
        }
        strcat(str, temp1);
        ret = prrte_argv_append_nosize(names, str);
        if(PRRTE_SUCCESS != ret) {
            PRRTE_ERROR_LOG(ret);
            free(str);
            return ret;
        }
    }
    free(str);

    /* All done */
    return PRRTE_SUCCESS;
}

static void timeout(int fd, short args, void *cbdata)
{
    local_jobtracker_t *jtrk = (local_jobtracker_t*)cbdata;
    prrte_job_t *jdata;

    prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-timeout", true);
    prrte_output_verbose(2, prrte_ras_base_framework.framework_output,
                        "%s Timed out on dynamic allocation",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    /* indicate that we failed to receive an allocation */
    jdata = prrte_get_job_data_object(jtrk->jobid);
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOC_FAILED);
}

static void recv_data(int fd, short args, void *cbdata)
{
    bool found;
    int i, rc;
    prrte_node_t *nd, *nd2;
    prrte_list_t nds, ndtmp;
    prrte_list_item_t *item, *itm;
    char recv_msg[8192];
    int nbytes, idx, sjob;
    char **alloc, *nodelist, *tpn;
    local_jobtracker_t *ptr, *jtrk;
    local_apptracker_t *aptrk;
    prrte_app_context_t *app;
    prrte_jobid_t jobid;
    prrte_job_t *jdata;
    char **dash_host = NULL;

    prrte_output_verbose(2, prrte_ras_base_framework.framework_output,
                        "%s ras:slurm: dynamic allocation - data recvd",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* read the data from the socket and put it in the
     * nodes field of op
     */
    memset(recv_msg, 0, sizeof(recv_msg));
    nbytes = read(fd, recv_msg, sizeof(recv_msg) - 1);

    prrte_output_verbose(2, prrte_ras_base_framework.framework_output,
                        "%s ras:slurm: dynamic allocation msg: %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), recv_msg);

    /* check if we got something */
    if (0 == nbytes || 0 == strlen(recv_msg) || strstr(recv_msg, "failure") != NULL) {
        /* show an error here - basically, a "nothing was available"
         * message
         */
        prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-failed", true,
                       (0 == strlen(recv_msg)) ? "NO MSG" : recv_msg);
        PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_ALLOC_FAILED);
        return;
    }

    /* break the message into its component parts, separated by colons */
    alloc = prrte_argv_split(recv_msg, ':');

    /* the first section contains the PRRTE jobid for this allocation */
    tpn = strchr(alloc[0], '=');
    prrte_util_convert_string_to_jobid(&jobid, tpn+1);
    /* get the corresponding job object */
    jdata = prrte_get_job_data_object(jobid);
    jtrk = NULL;
    /* find the associated tracking object */
    for (item = prrte_list_get_first(&jobs);
         item != prrte_list_get_end(&jobs);
         item = prrte_list_get_next(item)) {
        ptr = (local_jobtracker_t*)item;
        if (ptr->jobid == jobid) {
            jtrk = ptr;
            break;
        }
    }
    if (NULL == jtrk) {
        prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-failed", true, "NO JOB TRACKER");
        PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_ALLOC_FAILED);
        prrte_argv_free(alloc);
        return;
    }

    /* stop the timeout event */
    prrte_event_del(&jtrk->timeout_ev);

    /* cycle across all the remaining parts - each is the allocation for
     * an app in this job
     */
    PRRTE_CONSTRUCT(&nds, prrte_list_t);
    PRRTE_CONSTRUCT(&ndtmp, prrte_list_t);
    idx = -1;
    sjob = -1;
    nodelist = NULL;
    tpn = NULL;
    for (i=1; NULL != alloc[i]; i++) {
        if (PRRTE_SUCCESS != parse_alloc_msg(alloc[i], &idx, &sjob, &nodelist, &tpn)) {
            prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-failed", true, jtrk->cmd);
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOC_FAILED);
            prrte_argv_free(alloc);
            if (NULL != nodelist) {
                free(nodelist);
            }
            if (NULL != tpn) {
                free(tpn);
            }
            return;
        }
        if (idx < 0) {
            prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-failed", true, jtrk->cmd);
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOC_FAILED);
            prrte_argv_free(alloc);
            free(nodelist);
            free(tpn);
            return;
        }
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, idx))) {
            prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-failed", true, jtrk->cmd);
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOC_FAILED);
            prrte_argv_free(alloc);
            free(nodelist);
            free(tpn);
            return;
        }
        /* release the current dash_host as that contained the *desired* allocation */
        prrte_remove_attribute(&app->attributes, PRRTE_APP_DASH_HOST);
        /* track the Slurm jobid */
        if (NULL == (aptrk = (local_apptracker_t*)prrte_pointer_array_get_item(&jtrk->apps, idx))) {
            aptrk = PRRTE_NEW(local_apptracker_t);
            prrte_pointer_array_set_item(&jtrk->apps, idx, aptrk);
        }
        aptrk->sjob = sjob;
        /* since the nodelist/tpn may contain regular expressions, parse them */
        if (PRRTE_SUCCESS != (rc = prrte_ras_slurm_discover(nodelist, tpn, &ndtmp))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOC_FAILED);
            prrte_argv_free(alloc);
            free(nodelist);
            free(tpn);
            return;
        }
        /* transfer the discovered nodes to our node list, and construct
         * the new dash_host entry to match what was allocated
         */
        while (NULL != (item = prrte_list_remove_first(&ndtmp))) {
            nd = (prrte_node_t*)item;
            prrte_argv_append_nosize(&dash_host, nd->name);
            /* check for duplicates */
            found = false;
            for (itm = prrte_list_get_first(&nds);
                 itm != prrte_list_get_end(&nds);
                 itm = prrte_list_get_next(itm)) {
                nd2 = (prrte_node_t*)itm;
                if (0 == strcmp(nd->name, nd2->name)) {
                    found = true;
                    nd2->slots += nd->slots;
                    PRRTE_RELEASE(item);
                    break;
                }
            }
            if (!found) {
                /* append the new node to our list */
                prrte_list_append(&nds, item);
            }
        }
        /* cleanup */
        free(nodelist);
        free(tpn);
    }
    /* cleanup */
    prrte_argv_free(alloc);
    PRRTE_DESTRUCT(&ndtmp);
    if (NULL != dash_host) {
        tpn = prrte_argv_join(dash_host, ',');
        for (idx=0; idx < jdata->apps->size; idx++) {
            if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, idx))) {
                prrte_show_help("help-ras-slurm.txt", "slurm-dyn-alloc-failed", true, jtrk->cmd);
                PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOC_FAILED);
                prrte_argv_free(dash_host);
                free(tpn);
                return;
            }
            prrte_set_attribute(&app->attributes, PRRTE_APP_DASH_HOST, PRRTE_ATTR_LOCAL, (void*)tpn, PRRTE_STRING);
        }
        prrte_argv_free(dash_host);
        free(tpn);
    }

    if (prrte_list_is_empty(&nds)) {
        /* if we get here, then we were able to contact slurm,
         * which means we are in an actively managed cluster.
         * However, slurm indicated that nothing is currently
         * available that meets our requirements. This is a fatal
         * situation - we do NOT have the option of running on
         * user-specified hosts as the cluster is managed.
         */
        PRRTE_DESTRUCT(&nds);
        prrte_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    }

    /* store the found nodes */
    if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nds, jdata))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&nds);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        return;
    }
    PRRTE_DESTRUCT(&nds);

    /* default to no-oversubscribe-allowed for managed systems */
    if (!(PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
    }
    /* flag that the allocation is managed */
    prrte_managed_allocation = true;
    /* move the job along */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOCATION_COMPLETE);
    /* all done */
    return;
}

/* we cannot use the RML to communicate with SLURM as it doesn't
 * understand our internal protocol, so we have to do a bare-bones
 * exchange based on sockets
 */
static int dyn_allocate(prrte_job_t *jdata)
{
    char *cmd_str, **cmd=NULL, *tmp, *jstring;
    char *node_list;
    prrte_app_context_t *app;
    int i;
    struct timeval tv;
    local_jobtracker_t *jtrk;
    int64_t i64, *i64ptr;

    if (NULL == prrte_ras_slurm_component.config_file) {
        prrte_output(0, "Cannot perform dynamic allocation as no Slurm configuration file provided");
        return PRRTE_ERR_NOT_FOUND;
    }

    /* track this request */
    jtrk = PRRTE_NEW(local_jobtracker_t);
    jtrk->jobid = jdata->jobid;
    prrte_list_append(&jobs, &jtrk->super);

    /* construct the command - note that the jdata structure contains
     * a field for the minimum number of nodes required for the job.
     * The node list can be constructed from the union of all the nodes
     * contained in the dash_host field of the app_contexts. So you'll
     * need to do a little work to build the command. We don't currently
     * have a field in the jdata structure for "mandatory" vs "optional"
     * allocations, so we'll have to add that someday. Likewise, you may
     * want to provide a param to adjust the timeout value
     */
    /* construct the cmd string */
    prrte_argv_append_nosize(&cmd, "allocate");
    /* add the jobid */
    prrte_util_convert_jobid_to_string(&jstring, jdata->jobid);
    prrte_asprintf(&tmp, "jobid=%s", jstring);
    prrte_argv_append_nosize(&cmd, tmp);
    free(tmp);
    free(jstring);
    /* if we want the allocation for all apps in one shot,
     * then tell slurm
     *
     * RHC: we don't currently have the ability to handle
     * rolling allocations in the rest of the code base
     */
#if 0
    if (!prrte_ras_slurm_component.rolling_alloc) {
        prrte_argv_append_nosize(&cmd, "return=all");
    }
#else
    prrte_argv_append_nosize(&cmd, "return=all");
#endif

    /* pass the timeout */
    prrte_asprintf(&tmp, "timeout=%d", prrte_ras_slurm_component.timeout);
    prrte_argv_append_nosize(&cmd, tmp);
    free(tmp);

    /* for each app, add its allocation request info */
    i64ptr = &i64;
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        /* add the app id, preceded by a colon separator */
        prrte_asprintf(&tmp, ": app=%d", (int)app->idx);
        prrte_argv_append_nosize(&cmd, tmp);
        free(tmp);
        /* add the number of process "slots" we need */
        prrte_asprintf(&tmp, "np=%d", app->num_procs);
        prrte_argv_append_nosize(&cmd, tmp);
        free(tmp);
        /* if we were given a minimum number of nodes, pass it along */
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_MIN_NODES, (void**)&i64ptr, PRRTE_INT64)) {
            prrte_asprintf(&tmp, "N=%ld", (long int)i64);
            prrte_argv_append_nosize(&cmd, tmp);
            free(tmp);
        }
        /* add the list of nodes, if one was given, ensuring
         * that each node only appears once
         */
        node_list =  get_node_list(app);
        if (NULL != node_list) {
            prrte_asprintf(&tmp, "node_list=%s", node_list);
            prrte_argv_append_nosize(&cmd, tmp);
            free(node_list);
            free(tmp);
        }
        /* add the mandatory/optional flag */
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_MANDATORY, NULL, PRRTE_BOOL)) {
            prrte_argv_append_nosize(&cmd, "flag=mandatory");
        } else {
            prrte_argv_append_nosize(&cmd, "flag=optional");
        }
    }

    /* assemble it into the final cmd to be sent */
    cmd_str = prrte_argv_join(cmd, ' ');
    prrte_argv_free(cmd);

    /* start a timer - if the response to our request doesn't appear
     * in the defined time, then we will error out as Slurm isn't
     * responding to us
     */
    prrte_event_evtimer_set(prrte_event_base, &jtrk->timeout_ev, timeout, jtrk);
    tv.tv_sec = prrte_ras_slurm_component.timeout * 2;
    tv.tv_usec = 0;
    prrte_event_evtimer_add(&jtrk->timeout_ev, &tv);

    prrte_output_verbose(2, prrte_ras_base_framework.framework_output,
                        "%s slurm:dynalloc cmd_str = %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        cmd_str);

    if (send(socket_fd, cmd_str, strlen(cmd_str)+1, 0) < 0) {
        PRRTE_ERROR_LOG(PRRTE_ERR_COMM_FAILURE);
    }
    free(cmd_str);

    /* we cannot wait here for a response as we
     * are already in an event. So return a value
     * that indicates we are waiting for an
     * allocation so the base functions know
     * that they shouldn't progress the job
     */
    return PRRTE_ERR_ALLOCATION_PENDING;
}

static int parse_alloc_msg(char *msg, int *idx, int *sjob,
                           char **nodelist, char **tpn)
{
    char *tmp;
    char *p_str;
    char *pos;
    int found=0;

    if (msg == NULL || strlen(msg) == 0) {
        return PRRTE_ERR_BAD_PARAM;
    }

    tmp = strdup(msg);
    p_str = strtok(tmp, " ");
    while (p_str) {
        if (NULL != strstr(p_str, "slurm_jobid")) {
            pos = strchr(p_str, '=');
            *sjob = strtol(pos+1, NULL, 10);
            found++;
        } else if (NULL != strstr(p_str, "allocated_node_list")) {
            pos = strchr(p_str, '=');
            *nodelist = strdup(pos+1);
            found++;
        } else if (NULL != strstr(p_str, "tasks_per_node")) {
            pos = strchr(p_str, '=');
            *tpn = strdup(pos+1);
            found++;
        } else if (NULL != strstr(p_str, "app")) {
            pos = strchr(p_str, '=');
            *idx = strtol(pos+1, NULL, 10);
            found++;
        }
        p_str = strtok(NULL, " ");
    }
    free(tmp);

    if (4 != found) {
        return PRRTE_ERR_NOT_FOUND;
    }
    return PRRTE_SUCCESS;
}

static char* get_node_list(prrte_app_context_t *app)
{
    int j;
    char **total_host = NULL;
    char *nodes;
    char **dash_host, *dh;

    if (!prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&dh, PRRTE_STRING)) {
        return NULL;
    }
    dash_host = prrte_argv_split(dh, ',');
    free(dh);
    for (j=0; NULL != dash_host[j]; j++) {
        prrte_argv_append_unique_nosize(&total_host, dash_host[j], false);
    }
    prrte_argv_free(dash_host);
    if (NULL == total_host) {
        return NULL;
    }

    nodes = prrte_argv_join(total_host, ',');
    prrte_argv_free(total_host);
    return nodes;
}

static int read_ip_port(char *filename, char **ip, uint16_t *port)
{
    FILE *fp;
    char line[PRRTE_SLURM_DYN_MAX_SIZE];
    char *pos;
    bool found_port = false;
    bool found_ip = false;

    if (NULL == (fp = fopen(filename, "r"))) {
        prrte_show_help("help-ras-slurm.txt", "config-file-not-found", true, filename);
        return PRRTE_ERR_SILENT;
    }

    memset(line, 0, PRRTE_SLURM_DYN_MAX_SIZE);
    while (NULL != fgets(line, PRRTE_SLURM_DYN_MAX_SIZE, fp) &&
                 (!found_ip || !found_port)) {
        if (0 == strlen(line)) {
            continue;
        }
        line[strlen(line)-1] = '\0';
        if (0 == strncmp(line, "JobSubmitDynAllocPort", strlen("JobSubmitDynAllocPort"))) {
            pos = strstr(line, "=") + 1;
            *port = strtol(pos, NULL, 10);
            found_port = true;
        } else if (0 == strncmp(line, "ControlMachine", strlen("ControlMachine"))) {
            pos = strstr(line, "=") + 1;
            *ip = strdup(pos);
            found_ip = true;
        }
        memset(line, 0, PRRTE_SLURM_DYN_MAX_SIZE);
    }

    fclose(fp);
    if (!found_ip) {
        prrte_output(0, "The IP address or name of the Slurm control machine was not provided");
        return PRRTE_ERR_NOT_FOUND;
    }
    if (!found_port) {
        prrte_output(0, "The IP port of the Slurm dynamic allocation service was not provided");
        return PRRTE_ERR_NOT_FOUND;
    }

    return PRRTE_SUCCESS;
}
