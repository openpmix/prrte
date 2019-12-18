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
 * communication between PRRTE processes.  The system is available for
 * most architectures, with some exceptions (the Cray XT3/XT4, for example).
 */


#ifndef PRRTE_MCA_RML_RML_H_
#define PRRTE_MCA_RML_RML_H_

#include "prrte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/mca/mca.h"
#include "src/mca/routed/routed.h"

#include "src/mca/rml/rml_types.h"

BEGIN_C_DECLS


/* ******************************************************************** */

typedef struct {
    prrte_object_t super;
    prrte_process_name_t name;
    prrte_buffer_t data;
    bool active;
} prrte_rml_recv_cb_t;
PRRTE_CLASS_DECLARATION(prrte_rml_recv_cb_t);

/* Provide a generic callback function to release buffers
 * following a non-blocking send as this happens all over
 * the code base
 */
PRRTE_EXPORT void prrte_rml_send_callback(int status, prrte_process_name_t* sender,
                                          prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                                          void* cbdata);

PRRTE_EXPORT void prrte_rml_recv_callback(int status, prrte_process_name_t* sender,
                                          prrte_buffer_t *buffer,
                                          prrte_rml_tag_t tag, void *cbdata);

/* ******************************************************************** */
/*                     RML CALLBACK FUNCTION DEFINITIONS                */

/**
 * Funtion prototype for callback from non-blocking iovec send and recv
 *
 * Funtion prototype for callback from non-blocking iovec send and recv.
 * On send, the iovec pointer will be the same pointer passed to
 * send_nb and count will equal the count given to send.
 *
 * On recv, the iovec pointer will be the address of a single iovec
 * allocated and owned by the RML, not the process receiving the
 * callback. Ownership of the data block can be transferred by setting
 * a user variable to point to the data block, and setting the
 * iovec->iov_base pointer to NULL.
 *
 * @note The parameter in/out parameters are relative to the user's callback
 * function.
 *
 * @param[in] status  Completion status
 * @param[in] peer    Opaque name of peer process
 * @param[in] msg     Pointer to the array of iovec that was sent
 *                    or to a single iovec that has been recvd
 * @param[in] count   Number of iovecs in the array
 * @param[in] tag     User defined tag for matching send/recv
 * @param[in] cbdata  User data passed to send_nb()
 */
typedef void (*prrte_rml_callback_fn_t)(int status,
                                       prrte_process_name_t* peer,
                                       struct iovec* msg,
                                       int count,
                                       prrte_rml_tag_t tag,
                                       void* cbdata);


/**
 * Funtion prototype for callback from non-blocking buffer send and receive
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
typedef void (*prrte_rml_buffer_callback_fn_t)(int status,
                                              prrte_process_name_t* peer,
                                              struct prrte_buffer_t* buffer,
                                              prrte_rml_tag_t tag,
                                              void* cbdata);

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
typedef void (*prrte_rml_exception_callback_t)(prrte_process_name_t* peer,
                                              prrte_rml_exception_t exception);


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
 * @retval PRRTE_SUCESS The process is available and will allow connections
 *                     from the local process
 * @retval PRRTE_ERROR  An unspecified error occurred during the update
 */
typedef int (*prrte_rml_module_ping_fn_t)(const char* contact_info,
                                         const struct timeval* tv);


/**
 * Send an iovec non-blocking message
 *
 * Send an array of iovecs to the specified peer.  The call
 * will return immediately, although the iovecs may not be modified
 * until the completion callback is triggered.  The iovecs *may* be
 * passed to another call to send_nb before the completion callback is
 * triggered.  The callback being triggered does not give any
 * indication of remote completion.
 *
 * @param[in] peer   Name of receiving process
 * @param[in] msg    Pointer to an array of iovecs to be sent
 * @param[in] count  Number of iovecs in array
 * @param[in] tag    User defined tag for matching send/recv
 * @param[in] cbfunc Callback function on message comlpetion
 * @param[in] cbdata User data to provide during completion callback
 *
 * @retval PRRTE_SUCCESS The message was successfully started
 * @retval PRRTE_ERR_BAD_PARAM One of the parameters was invalid
 * @retval PRRTE_ERR_ADDRESSEE_UNKNOWN Contact information for the
 *                    receiving process is not available
 * @retval PRRTE_ERROR  An unspecified error occurred
 */
typedef int (*prrte_rml_module_send_nb_fn_t)(prrte_process_name_t* peer,
                                            struct iovec* msg,
                                            int count,
                                            prrte_rml_tag_t tag,
                                            prrte_rml_callback_fn_t cbfunc,
                                            void* cbdata);


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
 * @retval PRRTE_SUCCESS The message was successfully started
 * @retval PRRTE_ERR_BAD_PARAM One of the parameters was invalid
 * @retval PRRTE_ERR_ADDRESSEE_UNKNOWN Contact information for the
 *                    receiving process is not available
 * @retval PRRTE_ERROR  An unspecified error occurred
 */
typedef int (*prrte_rml_module_send_buffer_nb_fn_t)(prrte_process_name_t* peer,
                                                   struct prrte_buffer_t* buffer,
                                                   prrte_rml_tag_t tag,
                                                   prrte_rml_buffer_callback_fn_t cbfunc,
                                                   void* cbdata);

/**
 * Purge the RML/OOB of contact info and pending messages
 * to/from a specified process. Used when a process aborts
 * and is to be restarted
 */
typedef void (*prrte_rml_module_purge_fn_t)(prrte_process_name_t *peer);


/**
 * Receive an iovec non-blocking message
 *
 * @param[in]  peer    Peer process or PRRTE_NAME_WILDCARD for wildcard receive
 * @param[in]  tag     User defined tag for matching send/recv
 * @param[in] persistent Boolean flag indicating whether or not this is a one-time recv
 * @param[in] cbfunc   Callback function on message comlpetion
 * @param[in] cbdata   User data to provide during completion callback
 */
typedef void (*prrte_rml_module_recv_nb_fn_t)(prrte_process_name_t* peer,
                                          prrte_rml_tag_t tag,
                                          bool persistent,
                                          prrte_rml_callback_fn_t cbfunc,
                                          void* cbdata);


/**
 * Receive a buffer non-blocking message
 *
 * @param[in]  peer    Peer process or PRRTE_NAME_WILDCARD for wildcard receive
 * @param[in]  tag     User defined tag for matching send/recv
 * @param[in] persistent Boolean flag indicating whether or not this is a one-time recv
 * @param[in] cbfunc   Callback function on message comlpetion
 * @param[in] cbdata   User data to provide during completion callback
 */
typedef void (*prrte_rml_module_recv_buffer_nb_fn_t)(prrte_process_name_t* peer,
                                                 prrte_rml_tag_t tag,
                                                 bool persistent,
                                                 prrte_rml_buffer_callback_fn_t cbfunc,
                                                 void* cbdata);

/**
 * Cancel a posted non-blocking receive
 *
 * Attempt to cancel a posted non-blocking receive.
 *
 * @param[in] peer    Peer process or PRRTE_NAME_WILDCARD, exactly as passed
 *                    to the non-blocking receive call
 * @param[in] tag     Posted receive tag
 */
typedef void (*prrte_rml_module_recv_cancel_fn_t)(prrte_process_name_t* peer,
                                              prrte_rml_tag_t tag);


/**
 * RML internal module interface - these will be implemented by all RML components
 */
typedef struct prrte_rml_base_module_t {
    /* pointer to the parent component for this module */
    struct prrte_rml_component_t                 *component;
    /* the routed module to be used */
    char                                        *routed;
    /** Ping process for connectivity check */
    prrte_rml_module_ping_fn_t                    ping;

    /** Send non-blocking iovec message */
    prrte_rml_module_send_nb_fn_t                 send_nb;

    /** Send non-blocking buffer message */
    prrte_rml_module_send_buffer_nb_fn_t          send_buffer_nb;

    prrte_rml_module_recv_nb_fn_t                 recv_nb;
    prrte_rml_module_recv_buffer_nb_fn_t          recv_buffer_nb;
    prrte_rml_module_recv_cancel_fn_t             recv_cancel;

    /** Purge information */
    prrte_rml_module_purge_fn_t                   purge;
} prrte_rml_base_module_t;


/** Interface for RML communication */
PRRTE_EXPORT extern prrte_rml_base_module_t prrte_rml;

/* ******************************************************************** */
/*                     RML COMPONENT DEFINITION                         */

/**
 * RML component interface
 *
 * Component interface for the RML framework.  A public instance of
 * this structure, called mca_rml_[component name]_component, must
 * exist in any RML component.
 */
typedef struct prrte_rml_component_t {
    /* Base component description */
    prrte_mca_base_component_t                        base;
    /* Base component data block */
    prrte_mca_base_component_data_t                   data;
    /* Component priority */
    int                                         priority;
} prrte_rml_component_t;



/* ******************************************************************** */


/** Macro for use in components that are of type rml */
#define PRRTE_RML_BASE_VERSION_3_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("rml", 3, 0, 0)


/* ******************************************************************** */


END_C_DECLS

#endif
