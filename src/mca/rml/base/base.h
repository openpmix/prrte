/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2014 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * RML Framework maintenence interface
 *
 * Interface for starting / stopping / controlling the RML framework,307
 * as well as support for modifying RML datatypes.
 *
 * @note The only RML datatype exposed to the user is the RML tag.
 * This will always be an integral value, so the only datatype support
 * really required is the internal DSS functions for packing /
 * unpacking / comparing tags.  The user should never need to deal
 * with these.
 */

#ifndef MCA_RML_BASE_H
#define MCA_RML_BASE_H

#include "prte_config.h"

#include "src/class/prte_pointer_array.h"
#include "src/mca/mca.h"

#include "src/mca/routed/routed.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/rml/rml.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRTE_EXPORT extern prte_mca_base_framework_t prte_rml_base_framework;
/* select a component */
PRTE_EXPORT int prte_rml_base_select(void);

/*
 *  globals that might be needed
 */

/* a global struct containing framework-level values */
typedef struct {
    prte_list_t posted_recvs;
    prte_list_t unmatched_msgs;
    int max_retries;
} prte_rml_base_t;
PRTE_EXPORT extern prte_rml_base_t prte_rml_base;

/* structure to send RML messages - used internally */
typedef struct {
    prte_list_item_t super;
    pmix_proc_t dst; // targeted recipient
    pmix_proc_t origin;
    int status;         // returned status on send
    prte_rml_tag_t tag; // targeted tag
    int retries;        // #times we have tried to send it

    /* user's send callback functions and data */
    prte_rml_buffer_callback_fn_t cbfunc;
    void *cbdata;

    /* data buffer */
    pmix_data_buffer_t dbuf;
    /* msg seq number */
    uint32_t seq_num;
} prte_rml_send_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_rml_send_t);

/* define an object for transferring send requests to the event lib */
typedef struct {
    prte_object_t super;
    prte_event_t ev;
    prte_rml_send_t send;
} prte_rml_send_request_t;
PRTE_CLASS_DECLARATION(prte_rml_send_request_t);

/* structure to recv RML messages - used internally */
typedef struct {
    prte_list_item_t super;
    prte_event_t ev;
    pmix_proc_t sender;      // sender
    prte_rml_tag_t tag;      // targeted tag
    uint32_t seq_num;        // sequence number
    pmix_data_buffer_t dbuf; // the recvd data
} prte_rml_recv_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_rml_recv_t);

typedef struct {
    prte_list_item_t super;
    bool buffer_data;
    pmix_proc_t peer;
    prte_rml_tag_t tag;
    bool persistent;
    prte_rml_buffer_callback_fn_t cbfunc;
    void *cbdata;
} prte_rml_posted_recv_t;
PRTE_CLASS_DECLARATION(prte_rml_posted_recv_t);

/* define an object for transferring recv requests to the list of posted recvs */
typedef struct {
    prte_object_t super;
    prte_event_t ev;
    bool cancel;
    prte_rml_posted_recv_t *post;
} prte_rml_recv_request_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_rml_recv_request_t);

/* define a structure for sending a message to myself */
typedef struct {
    prte_object_t object;
    prte_event_t ev;
    prte_rml_tag_t tag;
    pmix_data_buffer_t dbuf;
    prte_rml_buffer_callback_fn_t cbfunc;
    void *cbdata;
} prte_self_send_xfer_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_self_send_xfer_t);

#define PRTE_RML_POST_MESSAGE(p, t, s, b, l)                                                    \
    do {                                                                                        \
        prte_rml_recv_t *msg;                                                                   \
        pmix_status_t _rc;                                                                      \
        pmix_byte_object_t _bo;                                                                 \
        prte_output_verbose(5, prte_rml_base_framework.framework_output,                        \
                            "%s Message posted at %s:%d for tag %d",                            \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__, (t));       \
        msg = PRTE_NEW(prte_rml_recv_t);                                                        \
        PMIX_XFER_PROCID(&msg->sender, (p));                                                    \
        msg->tag = (t);                                                                         \
        msg->seq_num = (s);                                                                     \
        _bo.bytes = (char *) (b);                                                               \
        _bo.size = (l);                                                                         \
        _rc = PMIx_Data_load(&msg->dbuf, &_bo);                                                 \
        if (PMIX_SUCCESS != _rc) {                                                              \
            PMIX_ERROR_LOG(_rc);                                                                \
        }                                                                                       \
        /* setup the event */                                                                   \
        prte_event_set(prte_event_base, &msg->ev, -1, PRTE_EV_WRITE, prte_rml_base_process_msg, \
                       msg);                                                                    \
        prte_event_set_priority(&msg->ev, PRTE_MSG_PRI);                                        \
        prte_event_active(&msg->ev, PRTE_EV_WRITE, 1);                                          \
    } while (0);

#define PRTE_RML_ACTIVATE_MESSAGE(m)                                                            \
    do {                                                                                        \
        /* setup the event */                                                                   \
        prte_event_set(prte_event_base, &(m)->ev, -1, PRTE_EV_WRITE, prte_rml_base_process_msg, \
                       (m));                                                                    \
        prte_event_set_priority(&(m)->ev, PRTE_MSG_PRI);                                        \
        prte_event_active(&(m)->ev, PRTE_EV_WRITE, 1);                                          \
    } while (0);

#define PRTE_RML_SEND_COMPLETE(m)                                                             \
    do {                                                                                      \
        prte_output_verbose(5, prte_rml_base_framework.framework_output,                      \
                            "%s-%s Send message complete at %s:%d",                           \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&((m)->dst)), \
                            __FILE__, __LINE__);                                              \
        if (NULL != (m)->cbfunc) {                                                            \
            /* non-blocking buffer send */                                                    \
            (m)->cbfunc((m)->status, &((m)->dst), &(m)->dbuf, (m)->tag, (m)->cbdata);         \
        }                                                                                     \
        PRTE_RELEASE(m);                                                                      \
    } while (0);

/* common implementations */
PRTE_EXPORT void prte_rml_base_post_recv(int sd, short args, void *cbdata);
PRTE_EXPORT void prte_rml_base_process_msg(int fd, short flags, void *cbdata);

END_C_DECLS

#endif /* MCA_RML_BASE_H  */
