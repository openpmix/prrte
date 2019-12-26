/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      NVIDIA Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * In windows, many of the socket functions return an EWOULDBLOCK
 * instead of things like EAGAIN, EINPROGRESS, etc. It has been
 * verified that this will not conflict with other error codes that
 * are returned by these functions under UNIX/Linux environments
 */

#include "prrte_config.h"
#include "types.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <ctype.h>

#include "src/util/show_help.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/prrte_socket_errno.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/argv.h"
#include "src/class/prrte_hash_table.h"
#include "src/class/prrte_list.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/routed/routed.h"
#include "src/mca/state/state.h"
#include "src/mca/oob/oob.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/common/alps/common_alps.h"
#include "src/util/name_fns.h"
#include "src/util/parse_options.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_globals.h"

static int alps_component_open(void);
static int alps_component_close(void);
static int component_available(void);
static int component_startup(void);
static void component_shutdown(void);
static int component_send(prrte_rml_send_t *msg);
static char* component_get_addr(void);
static int component_set_addr(prrte_process_name_t *peer, char **uris);
static bool component_is_reachable(char *routed, prrte_process_name_t *peer);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prrte_oob_base_component_t prrte_oob_alps_component = {
    .oob_base = {
        PRRTE_OOB_BASE_VERSION_2_0_0,
        .mca_component_name = "alps",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),
        .mca_open_component = alps_component_open,
        .mca_close_component = alps_component_close,
    },
    .oob_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    .priority = 30, // default priority of this transport
    .available = component_available,
    .startup = component_startup,
    .shutdown = component_shutdown,
    .send_nb = component_send,
    .get_addr = component_get_addr,
    .set_addr = component_set_addr,
    .is_reachable = component_is_reachable,
};

/*
 * Initialize global variables used w/in this module.
 */
static int alps_component_open(void)
{
    return PRRTE_SUCCESS;
}

static int alps_component_close(void)
{
    return PRRTE_SUCCESS;
}

static int component_available(void)
{
    return PRRTE_ERR_NOT_SUPPORTED;
}

/* Start all modules */
static int component_startup(void)
{
    prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                        "%s ALPS STARTUP",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    return PRRTE_SUCCESS;
}

static void component_shutdown(void)
{
    prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                        "%s ALPS SHUTDOWN",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
}

static int component_send(prrte_rml_send_t *msg)
{
    prrte_output_verbose(10, prrte_oob_base_framework.framework_output,
                        "%s oob:alps:send_nb to peer %s:%d this should not be happening",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&msg->dst), msg->tag);

    return PRRTE_ERR_NOT_SUPPORTED;
}

static char* component_get_addr(void)
{
    char *cptr;

    /*
     * TODO: for aries want to plug in GNI addr here instead to
     * eventually be able to support connect/accept using aprun.
     */

    prrte_asprintf(&cptr, "gni://%s:%d", prrte_process_info.nodename, getpid());

    prrte_output_verbose(10, prrte_oob_base_framework.framework_output,
                        "%s oob:alps: component_get_addr invoked - %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),cptr);
    return cptr;
}

static int component_set_addr(prrte_process_name_t *peer,
                              char **uris)
{
    prrte_output_verbose(10, prrte_oob_base_framework.framework_output,
                        "%s oob:alps: component_set_addr invoked - this should not be happening",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    return PRRTE_ERR_NOT_SUPPORTED;
}

static bool component_is_reachable(char *routed, prrte_process_name_t *peer)
{
    prrte_output_verbose(10, prrte_oob_base_framework.framework_output,
                        "%s oob:alps: component_set_addr invoked - this should not be happening",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    return false;
}
