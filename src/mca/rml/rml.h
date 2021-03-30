/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
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
 * Runtime Messaging Layer (RML) Communication Interface
 *
 * The Runtime Messaging Layer (RML) provices basic point-to-point
 * communication between PRTE processes.  The system is available for
 * most architectures, with some exceptions (the Cray XT3/XT4, for example).
 */

#ifndef PRTE_MCA_RML_RML_H_
#define PRTE_MCA_RML_RML_H_

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/mca.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/* ******************************************************************** */

typedef struct {
    prte_object_t super;
    pmix_proc_t name;
    pmix_data_buffer_t data;
    bool active;
} prte_rml_recv_cb_t;
PRTE_CLASS_DECLARATION(prte_rml_recv_cb_t);

/* Provide a generic callback function to release buffers
 * following a non-blocking send as this happens all over
 * the code base
 */
PRTE_EXPORT void prte_rml_send_callback(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                        prte_rml_tag_t tag, void *cbdata);

PRTE_EXPORT void prte_rml_recv_callback(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                        prte_rml_tag_t tag, void *cbdata);

/* ******************************************************************** */
/*                     RML CALLBACK FUNCTION DEFINITIONS                */

/**
 * Function prototype for callback from non-blocking buffer send and receive
 *
 * Function prototype for callback from non-blocking buffer send and
 * receive. On send, the buffer will be the same pointer passed to
 * send_buffer_nb. On receive, the buffer will be allocated and owned
 * by the RML, not the process receiving the callback.
 *
 * @note The parameter in/out parameters are relative to the user's callback
 * function.
 *
 * @param[in] status  Completion status
 * @param[in] peer    Name of peer process
 * @param[in] buffer  Message buffer
 * @param[in] tag     User defined tag for matching send/recv
 * @param[in] cbdata  User data passed to send_buffer_nb() or recv_buffer_nb()
 */
typedef void (*prte_rml_buffer_callback_fn_t)(int status, pmix_proc_t *peer,
                                              pmix_data_buffer_t *buffer, prte_rml_tag_t tag,
                                              void *cbdata);

/**
 * Function prototype for exception callback
 *
 * Function prototype for callback triggered when a communication error is detected.
 *
 * @note The parameter in/out parameters are relative to the user's callback
 * function.
 *
 * @param[in] peer      Name of peer process
 * @param[in] exception Description of the error causing the exception
 */
typedef void (*prte_rml_exception_callback_t)(pmix_proc_t *peer, prte_rml_exception_t exception);

/* ******************************************************************** */
/*                 RML INTERNAL MODULE API DEFINITION                   */

/**
 * "Ping" another process to determine availability
 *
 * Ping another process to determine if it is available.  This
 * function only verifies that the process is alive and will allow a
 * connection to the local process.  It does *not* qualify as
 * establishing communication with the remote process, as required by
 * the note for set_contact_info().
 *
 * @param[in] contact_info The contact info string for the remote process
 * @param[in] tv           Timeout after which the ping should be failed
 *
 * @retval PRTE_SUCESS The process is available and will allow connections
 *                     from the local process
 * @retval PRTE_ERROR  An unspecified error occurred during the update
 */
typedef int (*prte_rml_module_ping_fn_t)(const char *contact_info, const struct timeval *tv);

/**
 * Send a buffer non-blocking message
 *
 * Send a buffer to the specified peer.  The call
 * will return immediately, although the buffer may not be modified
 * until the completion callback is triggered.  The buffer *may* be
 * passed to another call to send_nb before the completion callback is
 * triggered.  The callback being triggered does not give any
 * indication of remote completion.
 *
 * @param[in] peer   Name of receiving process
 * @param[in] buffer Pointer to buffer to be sent
 * @param[in] tag    User defined tag for matching send/recv
 * @param[in] cbfunc Callback function on message comlpetion
 * @param[in] cbdata User data to provide during completion callback
 *
 * @retval PRTE_SUCCESS The message was successfully started
 * @retval PRTE_ERR_BAD_PARAM One of the parameters was invalid
 * @retval PRTE_ERR_ADDRESSEE_UNKNOWN Contact information for the
 *                    receiving process is not available
 * @retval PRTE_ERROR  An unspecified error occurred
 */
typedef int (*prte_rml_module_send_buffer_nb_fn_t)(pmix_proc_t *peer, pmix_data_buffer_t *buffer,
                                                   prte_rml_tag_t tag,
                                                   prte_rml_buffer_callback_fn_t cbfunc,
                                                   void *cbdata);

/**
 * Purge the RML/OOB of contact info and pending messages
 * to/from a specified process. Used when a process aborts
 * and is to be restarted
 */
typedef void (*prte_rml_module_purge_fn_t)(pmix_proc_t *peer);

/**
 * Receive a buffer non-blocking message
 *
 * @param[in]  peer    Peer process or PRTE_NAME_WILDCARD for wildcard receive
 * @param[in]  tag     User defined tag for matching send/recv
 * @param[in] persistent Boolean flag indicating whether or not this is a one-time recv
 * @param[in] cbfunc   Callback function on message comlpetion
 * @param[in] cbdata   User data to provide during completion callback
 */
typedef void (*prte_rml_module_recv_buffer_nb_fn_t)(pmix_proc_t *peer, prte_rml_tag_t tag,
                                                    bool persistent,
                                                    prte_rml_buffer_callback_fn_t cbfunc,
                                                    void *cbdata);

/**
 * Cancel a posted non-blocking receive
 *
 * Attempt to cancel a posted non-blocking receive.
 *
 * @param[in] peer    Peer process or PRTE_NAME_WILDCARD, exactly as passed
 *                    to the non-blocking receive call
 * @param[in] tag     Posted receive tag
 */
typedef void (*prte_rml_module_recv_cancel_fn_t)(pmix_proc_t *peer, prte_rml_tag_t tag);

/**
 * RML internal module interface - these will be implemented by all RML components
 */
typedef struct prte_rml_base_module_t {
    /* pointer to the parent component for this module */
    struct prte_rml_component_t *component;
    /* the routed module to be used */
    char *routed;
    /** Ping process for connectivity check */
    prte_rml_module_ping_fn_t ping;

    /** Send non-blocking buffer message */
    prte_rml_module_send_buffer_nb_fn_t send_buffer_nb;

    prte_rml_module_recv_buffer_nb_fn_t recv_buffer_nb;
    prte_rml_module_recv_cancel_fn_t recv_cancel;

    /** Purge information */
    prte_rml_module_purge_fn_t purge;
} prte_rml_base_module_t;

/** Interface for RML communication */
PRTE_EXPORT extern prte_rml_base_module_t prte_rml;

/* ******************************************************************** */
/*                     RML COMPONENT DEFINITION                         */

/**
 * RML component interface
 *
 * Component interface for the RML framework.  A public instance of
 * this structure, called mca_rml_[component name]_component, must
 * exist in any RML component.
 */
typedef struct prte_rml_component_t {
    /* Base component description */
    prte_mca_base_component_t base;
    /* Base component data block */
    prte_mca_base_component_data_t data;
    /* Component priority */
    int priority;
} prte_rml_component_t;

/* ******************************************************************** */

/** Macro for use in components that are of type rml */
#define PRTE_RML_BASE_VERSION_3_0_0 PRTE_MCA_BASE_VERSION_2_1_0("rml", 3, 0, 0)

/* ******************************************************************** */

END_C_DECLS

#endif
