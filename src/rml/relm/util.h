/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file:
 *
 * Functions for various standard relm tasks
 */

#ifndef PRTE_RELM_UTIL_H
#define PRTE_RELM_UTIL_H

#include <stdint.h>

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"
#include "src/util/error.h"

#include "src/rml/relm/types.h"
#include "src/rml/relm/state_machine.h"
#include "src/rml/relm/base/base.h"

BEGIN_C_DECLS

// Locally post the underlying message. Data is emptied.
void prte_relm_post(prte_relm_msg_t* msg);

// Check if any previous message has already been posted
bool prte_relm_prev_is_posted(prte_relm_msg_t* msg);

// Get a state's name for printing
char* prte_relm_state_name(prte_relm_state_t state);

#define PRTE_RELM_MSG_OUTPUT_PREFIX "%s relm: %s message %u %d->%d: "
#define PRTE_RELM_MSG_OUTPUT_ARGS(msg)                     \
    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),                    \
    ((msg) ? prte_relm_state_name((msg)->state) : "NULL"), \
    ((msg) ? (msg)->uid : PRTE_RELM_UID_INVALID),          \
    (int) ((msg) ? (msg)->src : PMIX_RANK_INVALID),        \
    (int) ((msg) ? (msg)->dst : PMIX_RANK_INVALID)
#define PRTE_RELM_MSG_OUTPUT(v, msg, text) PMIX_OUTPUT_VERBOSE((    \
        v, prte_relm_base.output, PRTE_RELM_MSG_OUTPUT_PREFIX text, \
        PRTE_RELM_MSG_OUTPUT_ARGS(msg)                              \
    ))
#define PRTE_RELM_MSG_OUTPUT_VERBOSE(v, msg, text, ...) PMIX_OUTPUT_VERBOSE(( \
        v, prte_relm_base.output, PRTE_RELM_MSG_OUTPUT_PREFIX text,           \
        PRTE_RELM_MSG_OUTPUT_ARGS(msg), __VA_ARGS__                           \
    ))
#define PRTE_RELM_MSG_OUTPUT_TRACE(v, msg) \
    PRTE_RELM_MSG_OUTPUT_VERBOSE(v, msg, "%s:%d", __func__, __LINE__);
#define PRTE_RELM_MSG_ERROR_LOG(msg, e)                                   \
    do {                                                              \
        PRTE_ERROR_LOG(e);                                            \
        if(PRTE_ERR_SILENT != (e)){                                   \
            PRTE_RELM_MSG_OUTPUT_VERBOSE(                             \
                0, msg, " %s:%s:%d %s", __FILE__, __func__, __LINE__, \
                prte_strerror(e)                                      \
            );                                                        \
        }                                                             \
    } while(0)

#define PRTE_RELM_OUTPUT(v, text) PMIX_OUTPUT_VERBOSE(( \
        v, prte_relm_base.output, "%s relm: " text,     \
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)              \
    ))
#define PRTE_RELM_OUTPUT_VERBOSE(v, text, ...) PMIX_OUTPUT_VERBOSE(( \
        v, prte_relm_base.output, "%s relm: " text,                  \
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __VA_ARGS__              \
    ))

// Packing and Unpacking functions.
//
// Pack functions return status.
//   On error: prints and updates proc/job state
//
// Unpack functions return the unpacked type.
//   On error: prints, updates proc/job state, and returns an invalid version
//   of the unpacked type.
int prte_relm_pack_signature(pmix_data_buffer_t* buf, prte_relm_msg_t* msg);
prte_relm_signature_t prte_relm_unpack_signature(pmix_data_buffer_t* buf);

int prte_relm_pack_state(pmix_data_buffer_t* buf, prte_relm_state_t state);
prte_relm_state_t prte_relm_unpack_state(pmix_data_buffer_t* buf);

int prte_relm_pack_data(pmix_data_buffer_t* buf, pmix_byte_object_t data);
pmix_byte_object_t prte_relm_unpack_data(pmix_data_buffer_t* buf);

int prte_relm_pack_uid(pmix_data_buffer_t* buf, prte_relm_uid_t uid);
prte_relm_uid_t prte_relm_unpack_uid(pmix_data_buffer_t* buf);

END_C_DECLS

#endif
