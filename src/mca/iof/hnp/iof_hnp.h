/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * The hnp IOF component is used in HNP processes only.  It is the
 * "hub" for all IOF activity, meaning that *all* IOF traffic is
 * routed to the hnp component, and this component figures out where
 * it is supposed to go from there.  Specifically: there is *no*
 * direct proxy-to-proxy IOF communication.  If a proxy/orted wants to
 * get a stream from another proxy/orted, the stream will go
 * proxy/orted -> HNP -> proxy/orted.
 *
 * The hnp IOF component does two things: 1. forward fragments between
 * file descriptors and streams, and 2. maintain forwarding tables to
 * "route" incoming fragments to outgoing destinations (both file
 * descriptors and other published streams).
 *
 */

#ifndef PRRTE_IOF_HNP_H
#define PRRTE_IOF_HNP_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif  /* HAVE_SYS_UIO_H */
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif  /* HAVE_NET_UIO_H */

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"


BEGIN_C_DECLS

/**
 * IOF HNP Component
 */
struct prrte_iof_hnp_component_t {
    prrte_iof_base_component_t super;
    prrte_list_t procs;
    prrte_iof_read_event_t *stdinev;
    prrte_event_t stdinsig;
};
typedef struct prrte_iof_hnp_component_t prrte_iof_hnp_component_t;

PRRTE_MODULE_EXPORT extern prrte_iof_hnp_component_t prrte_iof_hnp_component;
extern prrte_iof_base_module_t prrte_iof_hnp_module;

void prrte_iof_hnp_recv(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                       void* cbdata);

void prrte_iof_hnp_read_local_handler(int fd, short event, void *cbdata);
void prrte_iof_hnp_stdin_cb(int fd, short event, void *cbdata);
bool prrte_iof_hnp_stdin_check(int fd);

int prrte_iof_hnp_send_data_to_endpoint(prrte_process_name_t *host,
                                       prrte_process_name_t *target,
                                       prrte_iof_tag_t tag,
                                       unsigned char *data, int numbytes);

END_C_DECLS

#endif
