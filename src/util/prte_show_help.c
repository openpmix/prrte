/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/proc_info.h"

#include "src/util/prte_show_help.h"

/* Emit here and now, through PMIx, which applies the duplicate
 * suppression and aggregation and then delivers.  Correct on the HNP, on
 * a tool, and in an application; see the header for why it is not on a
 * prted. */
static void deliver_locally(const char *filename, const char *topic, const char *output)
{
    pmix_show_help_norender(filename, topic, output);
}

/* Carries a rendered message from whatever thread produced it over to the
 * progress thread, which is the only one allowed to touch the RML.
 *
 * This is not hypothetical: the odls dispatches each fork to a worker
 * thread from its own pool, so render_child_msg() - the whole reason a
 * daemon renders help at all - runs off the progress thread whenever a
 * node has enough local procs to engage that pool. The caddy owns all
 * three strings. */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    char *filename;
    char *topic;
    char *output;
} prte_show_help_caddy_t;

static void shcon(prte_show_help_caddy_t *p)
{
    p->filename = NULL;
    p->topic = NULL;
    p->output = NULL;
}
static void shdes(prte_show_help_caddy_t *p)
{
    if (NULL != p->filename) {
        free(p->filename);
    }
    if (NULL != p->topic) {
        free(p->topic);
    }
    if (NULL != p->output) {
        free(p->output);
    }
}
static PMIX_CLASS_INSTANCE(prte_show_help_caddy_t, pmix_object_t, shcon, shdes);

/* Runs on the PRRTE progress thread: pack the message and send it to the
 * HNP. Any failure falls back to local delivery rather than dropping a
 * diagnostic on the floor. */
static void relay_to_hnp(int sd, short args, void *cbdata)
{
    prte_show_help_caddy_t *cd = (prte_show_help_caddy_t *) cbdata;
    pmix_data_buffer_t *buf;
    pmix_status_t prc;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_DATA_BUFFER_CREATE(buf);
    /* the filename and topic travel with the text: the HNP needs them to
     * key duplicate suppression, and they are what identify the message */
    prc = PMIx_Data_pack(NULL, buf, &cd->filename, 1, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->topic, 1, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }
    prc = PMIx_Data_pack(NULL, buf, &cd->output, 1, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        goto fallback;
    }

    /* reliable, like the logging relay: a help message most often
     * accompanies a failure, which is exactly when a daemon between us and
     * the HNP may be on its way out */
    PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_SHOW_HELP);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(buf);
        deliver_locally(cd->filename, cd->topic, cd->output);
    }
    PMIX_RELEASE(cd);
    return;

fallback:
    PMIX_DATA_BUFFER_RELEASE(buf);
    deliver_locally(cd->filename, cd->topic, cd->output);
    PMIX_RELEASE(cd);
}

int prte_show_help(const char *filename, const char *topic,
                   int want_error_header, ...)
{
    va_list arglist;
    char *output;
    prte_show_help_caddy_t *cd;

    va_start(arglist, want_error_header);
    output = pmix_show_help_vstring(filename, topic, want_error_header, arglist);
    va_end(arglist);

    /* nothing came back - the topic rendered empty, or was not found */
    if (NULL == output) {
        return PRTE_SUCCESS;
    }

    /* The HNP is where the delivery machinery actually works: its PMIx
     * server is the one holding the tool connection (and under prterun it
     * IS the tool). A tool or an application delivers its own output too.
     * Only a non-master daemon has to hand the message off. */
    if (PRTE_PROC_IS_MASTER || !PRTE_PROC_IS_DAEMON) {
        deliver_locally(filename, topic, output);
        free(output);
        return PRTE_SUCCESS;
    }

    /* We are a prted. If we do not have an HNP to send to yet - very early
     * in startup, or while coming down - emit locally rather than lose the
     * message: our stderr may well still be connected, and a message that
     * might be seen beats one that certainly will not. */
    if (prte_finalizing || PMIX_RANK_INVALID == PRTE_PROC_MY_HNP->rank ||
        PMIX_NSPACE_INVALID(PRTE_PROC_MY_HNP->nspace)) {
        deliver_locally(filename, topic, output);
        free(output);
        return PRTE_SUCCESS;
    }

    /* Hand off to the progress thread to do the sending. We may well be on
     * one of the odls spawn-pool threads - render_child_msg() is called
     * from there - and the RML is progress-thread-only state. */
    cd = PMIX_NEW(prte_show_help_caddy_t);
    cd->filename = (NULL == filename) ? NULL : strdup(filename);
    cd->topic = (NULL == topic) ? NULL : strdup(topic);
    cd->output = output; /* the caddy takes it */
    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, relay_to_hnp);
    return PRTE_SUCCESS;
}

void prte_show_help_recv(int status, pmix_proc_t *sender,
                         pmix_data_buffer_t *buffer,
                         prte_rml_tag_t tag, void *cbdata)
{
    char *filename = NULL, *topic = NULL, *output = NULL;
    int32_t cnt;
    pmix_status_t rc;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &filename, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &topic, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &output, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    pmix_output_verbose(5, prte_debug_output,
                        "%s showing help relayed by %s [%s:%s]",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender),
                        (NULL == filename) ? "NULL" : filename,
                        (NULL == topic) ? "NULL" : topic);

    /* the text was rendered on the sending daemon - all that is left is to
     * deliver it, and to let PMIx suppress it if an identical message from
     * another node has already been shown */
    if (NULL != output) {
        deliver_locally(filename, topic, output);
    }

cleanup:
    if (NULL != filename) {
        free(filename);
    }
    if (NULL != topic) {
        free(topic);
    }
    if (NULL != output) {
        free(output);
    }
}
