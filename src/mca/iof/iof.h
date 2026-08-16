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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * I/O Forwarding Service
 *
 * The IOF connects an application process's stdin/stdout/stderr back to
 * the user. An application proc runs on some node, forked by that node's
 * prted, and its terminal is not the user's terminal - so the daemon that
 * forked it captures its stdout/stderr from a pipe and forwards them, and
 * receives stdin and writes it down a pipe.
 *
 * Everything funnels through the DVM master (the HNP): a daemon never
 * sends output to another daemon. The master collects all of it and hands
 * it to the PMIx server library, which performs the actual emission -
 * terminal writes, --output tagging, per-rank files, and copies to tools
 * that asked for them are the PMIx server's job, not this framework's.
 * Stdin travels the other way: it enters at the master, which resolves
 * which daemon hosts the target (or xcasts a wildcard) and sends it down.
 *
 * Note: by default, stdin is forwarded only to rank 0. Stdin for every
 * other proc is tied to /dev/null.
 *
 * Exactly one component is selected per process, chosen by the process's
 * role: "hnp" in the DVM master, "prted" in every other daemon. A tool is
 * neither, gets no module, and reaches the IOF through the PMIx server.
 *
 * The two central API calls are named less helpfully than one would like,
 * so state them plainly:
 *
 *   push: tie a local READ fd - the read end of a proc's stdout or stderr
 *         pipe - to a read event, so whatever the proc writes is captured
 *         and forwarded. This is the OUTPUT direction.
 *
 *   pull: tie a local WRITE fd - the write end of a proc's stdin pipe - to
 *         a sink, so data addressed to that proc's stdin is written down
 *         it. This is the STDIN direction, and stdin is the only stream
 *         it accepts.
 *
 * Streams are identified by a proc name (which may carry a wildcard rank)
 * and a prte_iof_tag_t. The tags are BIT FLAGS, not an enumeration - see
 * iof_types.h, which is authoritative - so a single call can name several
 * streams at once and every test is a mask.
 *
 * See src/mca/iof/AGENTS.md for the end-to-end picture, the ownership
 * rules, and what the flow-control tags do and do not accomplish.
 */

#ifndef PRTE_IOF_H
#define PRTE_IOF_H

#include "prte_config.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/pmix/pmix-internal.h"

#include "src/runtime/prte_globals.h"

#include "iof_types.h"

BEGIN_C_DECLS

/* Initialize the selected module */
typedef int (*prte_iof_base_init_fn_t)(void);

/**
 * Capture OUTPUT. Tie the read end of a local proc's stdout or stderr
 * pipe to a read event, so whatever appears there is forwarded - to the
 * PMIx server in the master, and to the master over the RML in a daemon.
 *
 * Both of a proc's streams must be pushed before either is activated, or
 * an immediate EOF on one can declare the proc IOF-complete before the
 * other is wired; the modules handle that internally.
 *
 * @param peer     Name of the proc whose output this is
 * @param src_tag  PRTE_IOF_STDOUT or PRTE_IOF_STDERR - which stream "fd" is
 * @param fd       Local file descriptor to read from
 */
typedef int (*prte_iof_base_push_fn_t)(const pmix_proc_t *peer, prte_iof_tag_t src_tag, int fd);

/**
 * Register a STDIN sink. Tie the write end of a local proc's stdin pipe
 * to a sink, so stdin addressed to that proc is written down it.
 *
 * Only PRTE_IOF_STDIN is accepted; anything else returns
 * PRTE_ERR_NOT_SUPPORTED.
 *
 * @param peer          Name of the proc whose stdin this is
 * @param source_tag    PRTE_IOF_STDIN
 * @param fd            Local file descriptor to write to
 */
typedef int (*prte_iof_base_pull_fn_t)(const pmix_proc_t *peer, prte_iof_tag_t source_tag, int fd);

/**
 * Close the streams named by the bits in "source_tag" for the given proc,
 * tearing down their read events and/or sink. Once all three are gone the
 * proc is dropped from the module's list.
 */
typedef int (*prte_iof_base_close_fn_t)(const pmix_proc_t *peer, prte_iof_tag_t source_tag);

/**
 * Inject stdin. Deliver a chunk to the named target, whose rank may be
 * PMIX_RANK_WILDCARD. In the master this means routing it - to the daemon
 * hosting the target over the RML, to every daemon for a wildcard, or
 * straight into a local proc's sink. In a daemon it means relaying to the
 * master, which is the only process that can do that routing.
 *
 * A zero-byte push is NOT a no-op: it is the sentinel that flushes what
 * precedes it and then closes the target's stdin.
 */
typedef int (*prte_iof_base_push_stdin_fn_t)(const pmix_proc_t *dst_name, uint8_t *data, size_t sz);

/* Flag that a job is complete: purge any endpoint bundles belonging to
 * this job's nspace that outlived their procs */
typedef void (*prte_iof_base_complete_fn_t)(const prte_job_t *jdata);

/* finalize the selected module */
typedef int (*prte_iof_base_finalize_fn_t)(void);

/**
 *  IOF module.
 */
struct prte_iof_base_module_2_0_0_t {
    prte_iof_base_init_fn_t init;
    prte_iof_base_push_fn_t push;
    prte_iof_base_pull_fn_t pull;
    prte_iof_base_close_fn_t close;
    prte_iof_base_complete_fn_t complete;
    prte_iof_base_finalize_fn_t finalize;
    prte_iof_base_push_stdin_fn_t push_stdin;
};

typedef struct prte_iof_base_module_2_0_0_t prte_iof_base_module_2_0_0_t;
typedef prte_iof_base_module_2_0_0_t prte_iof_base_module_t;
PRTE_EXPORT extern prte_iof_base_module_t prte_iof;

typedef pmix_mca_base_component_t prte_iof_base_component_t;

END_C_DECLS

/* The iof framework interface version. It is stated here and nowhere
 * else: components stamp it into their struct with
 * PRTE_MCA_BASE_VERSION(iof), and the framework's declaration reaches
 * the same three by pasting its name, so the two cannot drift apart.
 * Bump it on any change to the module interface that a component built
 * against the previous one would not survive. */
#define PRTE_MCA_iof_MAJOR_VERSION   2
#define PRTE_MCA_iof_MINOR_VERSION   0
#define PRTE_MCA_iof_RELEASE_VERSION 0

#endif /* PRTE_IOF_H */
