/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_UTIL_SHOW_HELP_H
#define PRTE_UTIL_SHOW_HELP_H

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/rml/rml_types.h"

BEGIN_C_DECLS

/*
 * Render a help message and get it in front of the user, from anywhere in
 * a DVM.
 *
 * This is a drop-in replacement for pmix_show_help() with the same
 * signature and the same rendering, and it exists because
 * pmix_show_help() does not work on a prted.
 *
 * Why: pmix_show_help() hands the rendered text to PMIx's plog framework,
 * and plog/stdfd writes its own stderr only when the caller is a PMIx
 * *client or tool*.  A prted is a PMIx *server*, so it takes the other
 * branch, which passes the text to PMIx_server_IOF_deliver() tagged with
 * the daemon's own identity - and nothing has an IOF sink for a daemon's
 * own output, so it is dropped.  The message is rendered perfectly and
 * then thrown away, on every node but the head one.  (It appears to work
 * on the head node only because there the daemon's PMIx server is the one
 * holding the tool connection.)
 *
 * So: on the HNP, on a tool, or in an application, deliver locally exactly
 * as pmix_show_help() would.  On any other daemon, ship the rendered text
 * to the HNP over the RML and let it do the delivering.  Aggregation and
 * duplicate suppression still happen once, on the HNP, keyed by the same
 * filename/topic pair - which is better than per-node suppression anyway.
 *
 * Use this, not pmix_show_help(), for any message that a daemon can
 * produce.  It is safe (and identical) everywhere else.
 */
PRTE_EXPORT int prte_show_help(const char *filename, const char *topic,
                               int want_error_header, ...);

/* HNP-side receiver for the relayed text. Registered by the PMIx server
 * setup alongside the other HNP-only recvs. */
PRTE_EXPORT void prte_show_help_recv(int status, pmix_proc_t *sender,
                                     pmix_data_buffer_t *buffer,
                                     prte_rml_tag_t tag, void *cbdata);

END_C_DECLS

#endif /* PRTE_UTIL_SHOW_HELP_H */
