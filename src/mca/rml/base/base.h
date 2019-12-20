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

#include "prrte_config.h"

#include "src/dss/dss_types.h"
#include "src/mca/mca.h"
#include "src/class/prrte_pointer_array.h"

#include "src/runtime/prrte_globals.h"
#include "src/mca/routed/routed.h"

#include "src/mca/rml/rml.h"


BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_rml_base_framework;
/* select a component */
PRRTE_EXPORT int prrte_rml_base_select(void);

/*
 *  globals that might be needed
 */

/* a global struct containing framework-level values */
typedef struct {
    prrte_list_t posted_recvs;
    prrte_list_t unmatched_msgs;
    int max_retries;
} prrte_rml_base_t;
PRRTE_EXPORT extern prrte_rml_base_t prrte_rml_base;


/* structure to send RML messages - used internally */
typedef struct {
    prrte_list_item_t super;
    prrte_process_name_t dst;     // targeted recipient
    prrte_process_name_t origin;
    int status;                  // returned status on send
    prrte_rml_tag_t tag;          // targeted tag
    int retries;                 // #times we have tried to send it

    /* user's send callback functions and data */
    union {
        prrte_rml_callback_fn_t        iov;
        prrte_rml_buffer_callback_fn_t buffer;
    } cbfunc;
    void *cbdata;

    /* pointer to the user's iovec array */
    struct iovec *iov;
    int count;
    /* pointer to the user's buffer */
    prrte_buffer_t *buffer;
    /* msg seq number */
    uint32_t seq_num;
    /* pointer to raw data for cross-transport
     * transfers
     */
    char *data;
} prrte_rml_send_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_rml_send_t);

/* define an object for transferring send requests to the event lib */
typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    prrte_rml_send_t send;
} prrte_rml_send_request_t;
PRRTE_CLASS_DECLARATION(prrte_rml_send_request_t);

/* structure to recv RML messages - used internally */
typedef struct {
    prrte_list_item_t super;
    prrte_event_t ev;
    prrte_process_name_t sender;  // sender
    prrte_rml_tag_t tag;          // targeted tag
    uint32_t seq_num;             //sequence number
    struct iovec iov;            // the recvd data
} prrte_rml_recv_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_rml_recv_t);

typedef struct {
    prrte_list_item_t super;
    bool buffer_data;
    prrte_process_name_t peer;
    prrte_rml_tag_t tag;
    bool persistent;
    union {
        prrte_rml_callback_fn_t        iov;
        prrte_rml_buffer_callback_fn_t buffer;
    } cbfunc;
    void *cbdata;
} prrte_rml_posted_recv_t;
PRRTE_CLASS_DECLARATION(prrte_rml_posted_recv_t);

/* define an object for transferring recv requests to the list of posted recvs */
typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    bool cancel;
    prrte_rml_posted_recv_t *post;
} prrte_rml_recv_request_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_rml_recv_request_t);

/* define a structure for sending a message to myself */
typedef struct {
    prrte_object_t object;
    prrte_event_t ev;
    prrte_rml_tag_t tag;
    struct iovec* iov;
    int count;
    prrte_buffer_t *buffer;
    union {
        prrte_rml_callback_fn_t        iov;
        prrte_rml_buffer_callback_fn_t buffer;
    } cbfunc;
    void *cbdata;
} prrte_self_send_xfer_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_self_send_xfer_t);

#define PRRTE_RML_POST_MESSAGE(p, t, s, b, l)                            \
    do {                                                                \
        prrte_rml_recv_t *msg;                                           \
        prrte_output_verbose(5, prrte_rml_base_framework.framework_output, \
                            "%s Message posted at %s:%d for tag %d",    \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),         \
                            __FILE__, __LINE__, (t));                   \
        msg = PRRTE_NEW(prrte_rml_recv_t);                                 \
        msg->sender.jobid = (p)->jobid;                                 \
        msg->sender.vpid = (p)->vpid;                                   \
        msg->tag = (t);                                                 \
        msg->seq_num = (s);                                             \
        msg->iov.iov_base = (IOVBASE_TYPE*)(b);                         \
        msg->iov.iov_len = (l);                                         \
        /* setup the event */                                           \
        prrte_event_set(prrte_event_base, &msg->ev, -1,                   \
                       PRRTE_EV_WRITE,                                   \
                       prrte_rml_base_process_msg, msg);                 \
        prrte_event_set_priority(&msg->ev, PRRTE_MSG_PRI);                \
        prrte_event_active(&msg->ev, PRRTE_EV_WRITE, 1);                  \
    } while(0);

#define PRRTE_RML_ACTIVATE_MESSAGE(m)                            \
    do {                                                        \
        /* setup the event */                                   \
        prrte_event_set(prrte_event_base, &(m)->ev, -1,           \
                       PRRTE_EV_WRITE,                           \
                       prrte_rml_base_process_msg, (m));         \
        prrte_event_set_priority(&(m)->ev, PRRTE_MSG_PRI);        \
        prrte_event_active(&(m)->ev, PRRTE_EV_WRITE, 1);          \
    } while(0);

#define PRRTE_RML_SEND_COMPLETE(m)                                       \
    do {                                                                \
        prrte_output_verbose(5, prrte_rml_base_framework.framework_output, \
                            "%s-%s Send message complete at %s:%d",     \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),         \
                            PRRTE_NAME_PRINT(&((m)->dst)),               \
                            __FILE__, __LINE__);                        \
        if (NULL != (m)->iov) {                                         \
            if (NULL != (m)->cbfunc.iov) {                              \
                (m)->cbfunc.iov((m)->status,                            \
                            &((m)->dst),                                \
                            (m)->iov, (m)->count,                       \
                            (m)->tag, (m)->cbdata);                     \
            }                                                           \
         } else if (NULL != (m)->cbfunc.buffer) {                       \
            /* non-blocking buffer send */                              \
            (m)->cbfunc.buffer((m)->status, &((m)->dst),                \
                           (m)->buffer,                                 \
                           (m)->tag, (m)->cbdata);                      \
         }                                                              \
        PRRTE_RELEASE(m);                                                 \
    }while(0);

/* common implementations */
PRRTE_EXPORT void prrte_rml_base_post_recv(int sd, short args, void *cbdata);
PRRTE_EXPORT void prrte_rml_base_process_msg(int fd, short flags, void *cbdata);


END_C_DECLS

#endif /* MCA_RML_BASE_H  */
