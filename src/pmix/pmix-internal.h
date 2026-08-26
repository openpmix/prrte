/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
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

#ifndef PRTE_PMIX_H
#define PRTE_PMIX_H

#include "prte_config.h"

#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/mca/mca.h"
#include "src/threads/pmix_threads.h"
#include "src/util/error.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"

#include <pmix.h>
#include <pmix_server.h>
#include <pmix_tool.h>
#include <pmix_version.h>

BEGIN_C_DECLS

PRTE_EXPORT extern int prte_pmix_verbose_output;

typedef struct {
    pmix_list_item_t super;
    pmix_app_t app;
    void *info;
    /* The mapping/ranking/binding directives this app segment carried, held
     * aside until the whole cmd line has been parsed. Only then is it known
     * whether they are per-app at all: a directive written on the first app
     * and nowhere else speaks for the whole job, however many apps there
     * are, so it belongs in the job's spec rather than any app's. Written on
     * a later app it is that app's alone. prte_parse_locals() decides and
     * distributes them. */
    char *mapby;
    char *rankby;
    char *bindto;
    /* The job-level directives this app segment carried. Unlike the three
     * above these are never per-app - there is no such thing as one app
     * being displayed, or one app not launching - but the global parse of
     * the command line stops at the first executable and so cannot see one
     * written in a later segment. prte_parse_locals() collects them and
     * hands them back to the tool's parse result. */
    char *output;
    char *display;
    char *rtos;
} prte_pmix_app_t;
PMIX_CLASS_DECLARATION(prte_pmix_app_t);

/* define another caddy for putting statically defined
 * pmix_info_t objects on a list */
typedef struct {
    pmix_list_item_t super;
    pmix_info_t info;
} prte_info_item_t;
PMIX_CLASS_DECLARATION(prte_info_item_t);

typedef struct {
    pmix_list_item_t super;
    pmix_list_t infolist;
} prte_info_array_item_t;
PMIX_CLASS_DECLARATION(prte_info_array_item_t);

typedef struct {
    pmix_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool active;
    int status;
    char *msg;
} prte_pmix_lock_t;

typedef struct {
    pmix_list_item_t super;
    pmix_value_t value;
} prte_value_t;
PMIX_CLASS_DECLARATION(prte_value_t);

#define prte_pmix_condition_wait(a, b) pthread_cond_wait(a, &(b)->m_lock_pthread)

#define PRTE_PMIX_CONSTRUCT_LOCK(l)                \
    do {                                           \
        PMIX_CONSTRUCT(&(l)->mutex, pmix_mutex_t); \
        pmix_mutex_lock(&(l)->mutex);              \
        pthread_cond_init(&(l)->cond, NULL);       \
        (l)->active = true;                        \
        (l)->status = 0;                           \
        (l)->msg = NULL;                           \
        PMIX_POST_OBJECT((l));                     \
        pmix_mutex_unlock(&(l)->mutex);            \
    } while (0)

#define PRTE_PMIX_DESTRUCT_LOCK(l)        \
    do {                                  \
        PMIX_ACQUIRE_OBJECT((l));         \
        PMIX_DESTRUCT(&(l)->mutex);       \
        pthread_cond_destroy(&(l)->cond); \
        if (NULL != (l)->msg) {           \
            free((l)->msg);               \
            (l)->msg = NULL;              \
        }                                 \
    } while (0)

/* Block the calling thread until someone wakes the lock. Never use this
 * on the thread that drives prte_event_base - see the discussion of
 * prte_pmix_shifted_wakeup() below and in the top-level AGENTS.md. */
#define PRTE_PMIX_WAIT_THREAD(lck)                                 \
    do {                                                           \
        pmix_mutex_lock(&(lck)->mutex);                            \
        while ((lck)->active) {                                    \
            prte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex); \
        }                                                          \
        PMIX_ACQUIRE_OBJECT(lck);                                  \
        pmix_mutex_unlock(&(lck)->mutex);                          \
    } while (0)

#define PRTE_PMIX_WAKEUP_THREAD(lck)       \
    do {                                   \
        pmix_mutex_lock(&(lck)->mutex);    \
        (lck)->active = false;             \
        PMIX_POST_OBJECT(lck);             \
        pthread_cond_signal(&(lck)->cond); \
        pmix_mutex_unlock(&(lck)->mutex);  \
    } while (0)

/* Caddy for shifting a lock wakeup from the PMIx progress thread
 * onto the PRRTE progress thread. A prte_pmix_lock_t is a PRRTE
 * object, so a callback executing on the PMIx progress thread must
 * not wake it directly - a waiter that drives prte_event_base while
 * polling the lock's active flag would never observe a wakeup
 * signaled while the event loop is parked in the backend. Posting
 * the wakeup as an event both wakes the loop and serializes the
 * flag update. */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    prte_pmix_lock_t *lock;
    pmix_status_t status;
    char *msg;
} prte_pmix_wakeup_caddy_t;
PMIX_CLASS_DECLARATION(prte_pmix_wakeup_caddy_t);

/* Post a wakeup of the given lock to prte_event_base. The handler
 * running on the PRRTE progress thread stores status in
 * lock->status, transfers ownership of msg (which must be NULL or
 * heap-allocated) to lock->msg, and then wakes the waiter. Call
 * this - never PRTE_PMIX_WAKEUP_THREAD - from any callback that
 * PMIx invokes in a process hosting a PRRTE event base. */
PRTE_EXPORT void prte_pmix_shifted_wakeup(prte_pmix_lock_t *lock,
                                          pmix_status_t status,
                                          char *msg);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process when we don't want the PMIx module
 * to request it from the server if not found:
 *
 * r - lvalue receiving the PMIx status of the modex op
 *     (pmix_status_t) - NOT a PRTE error code. Feed it to
 *     prte_pmix_convert_status() before comparing it against
 *     anything but PMIX_SUCCESS.
 * s - string key (char*)
 * p - pointer to the pmix_proc_t of the proc that posted
 *     the data (pmix_proc_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRTE_MODEX_RECV_VALUE_OPTIONAL(r, s, p, d, t)                                  \
    do {                                                                               \
        pmix_value_t *_kv = NULL;                                                      \
        pmix_info_t _info;                                                             \
        size_t _sz;                                                                    \
        PMIX_OUTPUT_VERBOSE((1, prte_pmix_verbose_output,                              \
                             "%s[%s:%d] MODEX RECV VALUE OPTIONAL FOR PROC %s KEY %s", \
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__,   \
                             PRTE_NAME_PRINT((p)), (s)));                              \
        PMIX_INFO_LOAD(&_info, PMIX_OPTIONAL, NULL, PMIX_BOOL);                        \
        (r) = PMIx_Get((p), (s), &(_info), 1, &(_kv));                                 \
        PMIX_INFO_DESTRUCT(&_info);                                                    \
        if (PMIX_SUCCESS != (r)) {                                                     \
            /* leave the library's status alone - it says why */                       \
        } else if (NULL == _kv) {                                                      \
            (r) = PMIX_ERR_NOT_FOUND;                                                  \
        } else if (_kv->type != (t)) {                                                 \
            (r) = PMIX_ERR_TYPE_MISMATCH;                                              \
        } else {                                                                       \
            PMIX_VALUE_UNLOAD((r), _kv, (void **) (d), &_sz);                          \
        }                                                                              \
        if (NULL != _kv) {                                                             \
            PMIX_VALUE_RELEASE(_kv);                                                   \
        }                                                                              \
    } while (0)

/* PRTE attribute */
typedef uint16_t prte_attribute_key_t;
typedef struct {
    pmix_list_item_t super;   /* required for this to be on lists */
    prte_attribute_key_t key; /* key identifier */
    bool local;               // whether or not to pack/send this value
    pmix_value_t data;
} prte_attribute_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_attribute_t);

/* Translators between PRRTE's and PMIx's code spaces. Every one of
 * these is total: an input the switch does not name still yields a
 * legal code in the target space, never a raw pass-through of the
 * input. See src/pmix/AGENTS.md for why that matters - the two spaces
 * overlap numerically. */
PRTE_EXPORT pmix_proc_state_t prte_pmix_convert_state(int state);
PRTE_EXPORT int prte_pmix_convert_pstate(pmix_proc_state_t);
PRTE_EXPORT pmix_status_t prte_pmix_convert_rc(int rc);
PRTE_EXPORT int prte_pmix_convert_status(pmix_status_t status);
PRTE_EXPORT pmix_status_t prte_pmix_convert_job_state_to_error(int state);
PRTE_EXPORT pmix_status_t prte_pmix_convert_proc_state_to_error(int state);

/* Reach the interface version a framework's own header states.
 *
 * The PRRTE counterpart of PMIX_MCA_FW_VER() in PMIx's mca.h, and the
 * reason PRRTE needs one of its own: that macro pastes a PMIX_ prefix,
 * and these are PRRTE's frameworks, so the numbers belong in PRRTE's
 * namespace. A framework states its version once, as three macros in its
 * <framework>.h - for example, in plm.h:
 *
 *   #define PRTE_MCA_plm_MAJOR_VERSION   2
 *   #define PRTE_MCA_plm_MINOR_VERSION   0
 *   #define PRTE_MCA_plm_RELEASE_VERSION 0
 *
 * Both sides of the load-time version check reach those same three
 * integers by pasting the framework's name: the component stamp below,
 * and the framework's declaration. Nothing restates them, so the version
 * a component carries and the version it is checked against cannot drift
 * apart, and bumping an interface is one edit in one header.
 *
 * The name carries the framework's directory name verbatim, lower case
 * and all: the preprocessor pastes tokens, it does not upper-case them. */
#define PRTE_MCA_FW_VER_(name, level) PRTE_MCA_##name##_##level##_VERSION
#define PRTE_MCA_FW_VER(name, level)  PRTE_MCA_FW_VER_(name, level)

/* Open a component struct.
 *
 * Every PRRTE component begins its base struct with this, naming its
 * framework as a bare token - the framework's directory name, so that the
 * same token both stringifies into the struct and pastes into the version
 * macros above:
 *
 *   prte_plm_base_component_t prte_mca_plm_ssh_component = {
 *       .base_version = {
 *           PRTE_MCA_BASE_VERSION(plm),
 *           .pmix_mca_component_name = "ssh",
 *           ...
 *
 * PMIx's PMIX_MCA_BASE_VERSION() cannot serve here: it stamps the project
 * as "pmix" with PMIx's own version numbers. This one says "prte" and
 * PRRTE's, which is the whole reason PRRTE keeps a macro of its own at
 * this level. */
#define PRTE_MCA_BASE_VERSION(type)                                                \
    PMIX_MCA_BASE_VERSION_2_1_0("prte", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,    \
                                PRTE_RELEASE_VERSION, #type,                       \
                                PRTE_MCA_FW_VER(type, MAJOR),                      \
                                PRTE_MCA_FW_VER(type, MINOR),                      \
                                PRTE_MCA_FW_VER(type, RELEASE))

/* Declare a PRRTE framework, stating its interface version.
 *
 * The PRRTE counterpart of PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE,
 * built on the same underlying declaration so that the framework reports
 * the version its header states and pmix_mca_base_components_open() can
 * refuse a component built against a different one. It takes no version
 * arguments and no project argument: every framework here is "prte", and
 * the numbers come from the header.
 *
 * A framework that uses this without defining the three macros above does
 * not compile, which is the point. Using PMIX_MCA_BASE_FRAMEWORK_DECLARE
 * instead still works and is what PRRTE did until August 2026 - it simply
 * reports version 0.0, which the loader reads as "no version stated" and
 * skips the check for, so the framework's components are never screened. */
#define PRTE_MCA_BASE_FRAMEWORK_DECLARE(name, description, registerfn, openfn,     \
                                        closefn, static_components, flags)         \
    PMIX_MCA_BASE_FRAMEWORK_DECLARE_FULL(prte, name,                               \
                                         PRTE_MCA_FW_VER(name, MAJOR),             \
                                         PRTE_MCA_FW_VER(name, MINOR),             \
                                         description, registerfn, openfn, closefn, \
                                         static_components, flags)


END_C_DECLS

#endif
