/*
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRtE_FILEM_RAW_EXPORT_H
#define PRRtE_FILEM_RAW_EXPORT_H

#include "prte_config.h"

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_object.h"
#include "src/event/event-internal.h"
#include "src/mca/mca.h"

#include "src/mca/filem/filem.h"

BEGIN_C_DECLS

PRTE_MODULE_EXPORT extern prte_filem_base_component_t prte_mca_filem_raw_component;
PRTE_EXPORT extern prte_filem_base_module_t prte_filem_raw_module;

extern bool prte_filem_raw_flatten_trees;

/* How many chunks of one file may be in flight at once - see the flow
 * control commentary on prte_filem_raw_xfer_t below.
 */
extern int prte_filem_raw_chunk_window;

#define PRTE_FILEM_RAW_CHUNK_MAX 16384

/* buffer size used when copying a staged file into a working directory,
 * and when comparing one against what is already there
 */
#define PRTE_FILEM_RAW_COPY_MAX 8192

/* local classes */
typedef struct {
    pmix_list_item_t super;
    pmix_list_t xfers;
    int32_t status;
    prte_filem_completion_cbfunc_t cbfunc;
    void *cbdata;
} prte_filem_raw_outbound_t;
PMIX_CLASS_DECLARATION(prte_filem_raw_outbound_t);

typedef struct {
    pmix_list_item_t super;
    prte_event_t ev;
    int fd;
    prte_filem_raw_outbound_t *outbound;
    bool pending;
    char *src;
    char *file;
    int32_t type;
    uint32_t mode;
    int32_t nchunk;
    int status;
    /* Flow control for the chunk pump.
     *
     * send_chunk hands a chunk to the non-blocking xcast and immediately
     * re-arms its own read event.  Nothing in that loop waits for the bytes
     * to arrive anywhere: the only ack this component receives is one per
     * *file*, sent by each daemon at EOF, so it cannot gate a read.  Without
     * a cap the file is therefore read as fast as the progress thread can
     * turn, and every chunk stays resident until it is delivered - xcast
     * holds a broadcast's payload on every daemon along the path until that
     * daemon's subtree confirms receipt, and retires those holdings in
     * strict broadcast order, so one slow subtree pins every chunk behind
     * it.  Measured on a ten-node DVM, a 256 MB preload took the HNP from
     * 9 MB to 744 MB.
     *
     * "inflight" is how many of this file's chunks have been broadcast and
     * not yet confirmed delivered to the whole DVM.  "paused" says the read
     * stopped because that reached prte_filem_raw_chunk_window, and a
     * delivery is what will restart it.
     */
    int32_t inflight;
    bool paused;
    /* The transfer has been retired.  A delivery that arrives afterwards
     * still has to give back the reference it holds, but must not re-arm a
     * read on a file that is finished.
     */
    bool retired;
    /* Delivery accounting, all four fields maintained together.
     *
     * "nexpected" is how many daemons still owe an ack.  It is seeded from
     * the DVM's daemon count when the transfer starts and is decremented by
     * the fault handler as daemons depart, rather than being re-read from
     * prte_process_info.num_daemons on each ack: the handler has to settle
     * the transfer at a moment when the daemon that just died may or may
     * not have been subtracted from that global yet.
     *
     * "nrecvd" counts the daemons that have the file AND are still in the
     * DVM, so a departure takes its ack back out again.  That is what makes
     * "did this file reach the whole DVM?" answerable later, in
     * raw_preposition_files' already-positioned check.
     *
     * "acked" records WHICH daemons those are.  A count alone cannot tell a
     * repeated ack from one daemon apart from a first ack from another, and
     * the fault handler must know whether the daemon that died had already
     * answered.
     */
    pmix_rank_t nexpected;
    pmix_rank_t nrecvd;
    pmix_bitmap_t acked;
    /* the number of daemon vpids the DVM had assigned when this transfer
     * began.  Daemon vpids are never reused and a grow appends new ones, so
     * any rank at or above this line joined after the broadcast started,
     * never saw its chunks, and therefore owes it nothing - a distinction
     * the fault handler needs before it credits a departure.
     */
    pmix_rank_t horizon;
} prte_filem_raw_xfer_t;
PMIX_CLASS_DECLARATION(prte_filem_raw_xfer_t);

typedef struct {
    pmix_list_item_t super;
    prte_event_t ev;
    bool pending;
    int fd;
    char *file;
    char *fullpath;
    int32_t type;
    uint32_t mode;
    char **link_pts;
    pmix_list_t outputs;
} prte_filem_raw_incoming_t;
PMIX_CLASS_DECLARATION(prte_filem_raw_incoming_t);

typedef struct {
    pmix_list_item_t super;
    int numbytes;
    unsigned char data[PRTE_FILEM_RAW_CHUNK_MAX];
} prte_filem_raw_output_t;
PMIX_CLASS_DECLARATION(prte_filem_raw_output_t);

END_C_DECLS

#endif /* PRRtE_FILEM_RAW_EXPORT_H */
