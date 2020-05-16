/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_PMIX_H
#define PRTE_PMIX_H

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "src/mca/mca.h"
#include "src/event/event-internal.h"
#include "src/dss/dss.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/printf.h"
#include "src/util/proc_info.h"
#include "src/include/hash_string.h"

#include PRTE_PMIX_HEADER
#if ! PRTE_PMIX_HEADER_GIVEN
#include <pmix_server.h>
#include <pmix_tool.h>
#include <pmix_version.h>
#endif

BEGIN_C_DECLS

PRTE_EXPORT extern int prte_pmix_verbose_output;

/* define a caddy for pointing to pmix_info_t that
 * are to be included in an answer */
typedef struct {
    prte_list_item_t super;
    pmix_proc_t source;
    pmix_info_t *info;
    pmix_persistence_t persistence;
} prte_ds_info_t;
PRTE_CLASS_DECLARATION(prte_ds_info_t);

/* define another caddy for putting statically defined
 * pmix_info_t objects on a list */
typedef struct {
    prte_list_item_t super;
    pmix_info_t info;
} prte_info_item_t;
PRTE_CLASS_DECLARATION(prte_info_item_t);

typedef struct {
    prte_list_item_t super;
    prte_list_t infolist;
} prte_info_array_item_t;
PRTE_CLASS_DECLARATION(prte_info_array_item_t);

typedef struct {
    prte_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool active;
    int status;
    char *msg;
} prte_pmix_lock_t;

#define prte_pmix_condition_wait(a,b)   pthread_cond_wait(a, &(b)->m_lock_pthread)

#define PRTE_PMIX_CONSTRUCT_LOCK(l)                    \
    do {                                                \
        PRTE_CONSTRUCT(&(l)->mutex, prte_mutex_t);    \
        pthread_cond_init(&(l)->cond, NULL);            \
        (l)->active = true;                             \
        (l)->status = 0;                                \
        (l)->msg = NULL;                                \
        PRTE_POST_OBJECT((l));                         \
    } while(0)

#define PRTE_PMIX_DESTRUCT_LOCK(l)         \
    do {                                    \
        PRTE_ACQUIRE_OBJECT((l));          \
        PRTE_DESTRUCT(&(l)->mutex);        \
        pthread_cond_destroy(&(l)->cond);   \
        if (NULL != (l)->msg) {             \
            free((l)->msg);                 \
        }                                   \
    } while(0)


#if PRTE_ENABLE_DEBUG
#define PRTE_PMIX_ACQUIRE_THREAD(lck)                              \
    do {                                                            \
        prte_mutex_lock(&(lck)->mutex);                            \
        if (prte_debug_threads) {                                  \
            prte_output(0, "Waiting for thread %s:%d",             \
                        __FILE__, __LINE__);                        \
        }                                                           \
        while ((lck)->active) {                                     \
            prte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex); \
        }                                                           \
        if (prte_debug_threads) {                                  \
            prte_output(0, "Thread obtained %s:%d",                \
                        __FILE__, __LINE__);                        \
        }                                                           \
        (lck)->active = true;                                       \
    } while(0)
#else
#define PRTE_PMIX_ACQUIRE_THREAD(lck)                              \
    do {                                                            \
        prte_mutex_lock(&(lck)->mutex);                            \
        while ((lck)->active) {                                     \
            prte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex); \
        }                                                           \
        (lck)->active = true;                                       \
    } while(0)
#endif


#if PRTE_ENABLE_DEBUG
#define PRTE_PMIX_WAIT_THREAD(lck)                                 \
    do {                                                            \
        prte_mutex_lock(&(lck)->mutex);                            \
        if (prte_debug_threads) {                                  \
            prte_output(0, "Waiting for thread %s:%d",             \
                        __FILE__, __LINE__);                        \
        }                                                           \
        while ((lck)->active) {                                     \
            prte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex); \
        }                                                           \
        if (prte_debug_threads) {                                  \
            prte_output(0, "Thread obtained %s:%d",                \
                        __FILE__, __LINE__);                        \
        }                                                           \
        PRTE_ACQUIRE_OBJECT(&lck);                                 \
        prte_mutex_unlock(&(lck)->mutex);                          \
    } while(0)
#else
#define PRTE_PMIX_WAIT_THREAD(lck)                                 \
    do {                                                            \
        prte_mutex_lock(&(lck)->mutex);                            \
        while ((lck)->active) {                                     \
            prte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex); \
        }                                                           \
        PRTE_ACQUIRE_OBJECT(lck);                                  \
        prte_mutex_unlock(&(lck)->mutex);                          \
    } while(0)
#endif


#if PRTE_ENABLE_DEBUG
#define PRTE_PMIX_RELEASE_THREAD(lck)                  \
    do {                                                \
        if (prte_debug_threads) {                      \
            prte_output(0, "Releasing thread %s:%d",   \
                        __FILE__, __LINE__);            \
        }                                               \
        (lck)->active = false;                          \
        pthread_cond_broadcast(&(lck)->cond);           \
        prte_mutex_unlock(&(lck)->mutex);              \
    } while(0)
#else
#define PRTE_PMIX_RELEASE_THREAD(lck)                      \
    do {                                                    \
        assert(0 != prte_mutex_trylock(&(lck)->mutex));    \
        (lck)->active = false;                              \
        pthread_cond_broadcast(&(lck)->cond);               \
        prte_mutex_unlock(&(lck)->mutex);                  \
    } while(0)
#endif


#define PRTE_PMIX_WAKEUP_THREAD(lck)           \
    do {                                        \
        prte_mutex_lock(&(lck)->mutex);        \
        (lck)->active = false;                  \
        PRTE_POST_OBJECT(lck);                 \
        pthread_cond_broadcast(&(lck)->cond);   \
        prte_mutex_unlock(&(lck)->mutex);      \
    } while(0)

/*
 * Count the hash for the the external RM
 */
#define PRTE_HASH_JOBID( str, hash ){               \
    PRTE_HASH_STR( str, hash );                     \
    hash &= ~(0x8000);                               \
}

/**
 * Provide a simplified macro for sending data via modex
 * to other processes. The macro requires four arguments:
 *
 * r - the integer return status from the modex op
 * sc - the PMIX scope of the data
 * s - the key to tag the data being posted
 * d - pointer to the data object being posted
 * t - the type of the data
 */
#define PRTE_MODEX_SEND_VALUE(r, sc, s, d, t)  \
    do {                                        \
        pmix_value_t _kv;                       \
        PMIX_VALUE_LOAD(&_kv, (d), (t));        \
        (r) = PMIx_Put((sc), (s), &(_kv));      \
                PRTE_ERROR_LOG((r));           \
    } while(0);

/**
 * Provide a simplified macro for sending data via modex
 * to other processes. The macro requires four arguments:
 *
 * r - the integer return status from the modex op
 * sc - the PMIX scope of the data
 * s - the key to tag the data being posted
 * d - the data object being posted
 * sz - the number of bytes in the data object
 */
#define PRTE_MODEX_SEND_STRING(r, sc, s, d, sz)    \
    do {                                            \
        pmix_value_t _kv;                           \
        _kv.type = PMIX_BYTE_OBJECT;                \
        _kv.data.bo.bytes = (uint8_t*)(d);          \
        _kv.data.bo.size = (sz);                    \
        (r) = PMIx_Put(sc, (s), &(_kv));            \
    } while(0);

/**
 * Provide a simplified macro for sending data via modex
 * to other processes. The macro requires four arguments:
 *
 * r - the integer return status from the modex op
 * sc - the PMIX scope of the data
 * s - the MCA component that is posting the data
 * d - the data object being posted
 * sz - the number of bytes in the data object
 */
#define PRTE_MODEX_SEND(r, sc, s, d, sz)                       \
    do {                                                        \
        char *_key;                                             \
        _key = prte_mca_base_component_to_string((s));         \
        PRTE_MODEX_SEND_STRING((r), (sc), _key, (d), (sz));    \
        free(_key);                                             \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process when we don't want the PMIx module
 * to request it from the server if not found:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the prte_process_name_t of the proc that posted
 *     the data (prte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRTE_MODEX_RECV_VALUE_OPTIONAL(r, s, p, d, t)                                  \
    do {                                                                                \
        pmix_proc_t _proc;                                                              \
        pmix_value_t *_kv = NULL;                                                       \
        pmix_info_t _info;                                                              \
        size_t _sz;                                                                     \
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_verbose_output,                             \
                            "%s[%s:%d] MODEX RECV VALUE OPTIONAL FOR PROC %s KEY %s",   \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),                       \
                            __FILE__, __LINE__,                                         \
                            PRTE_NAME_PRINT((p)), (s)));                               \
        PMIX_LOAD_PROCID(&(_proc), prte_process_info.myproc.nspace, (p)->vpid);        \
        PMIX_INFO_LOAD(&_info, PMIX_OPTIONAL, NULL, PMIX_BOOL);                         \
        (r) = PMIx_Get(&(_proc), (s), &(_info), 1, &(_kv));                             \
        if (NULL == _kv) {                                                              \
            (r) = PMIX_ERR_NOT_FOUND;                                                   \
        } else if (_kv->type != (t)) {                                                  \
            (r) = PMIX_ERR_TYPE_MISMATCH;                                               \
        } else if (PMIX_SUCCESS == (r)) {                                               \
            PMIX_VALUE_UNLOAD((r), _kv, (void**)(d), &_sz);                             \
        }                                                                               \
        if (NULL != _kv) {                                                              \
            PMIX_VALUE_RELEASE(_kv);                                                    \
        }                                                                               \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process when we want the PMIx module
 * to request it from the server if not found, but do not
 * want the server to go find it if the server doesn't
 * already have it:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the prte_process_name_t of the proc that posted
 *     the data (prte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRTE_MODEX_RECV_VALUE_IMMEDIATE(r, s, p, d, t)                                 \
    do {                                                                                \
        pmix_proc_t _proc;                                                              \
        pmix_value_t *_kv = NULL;                                                       \
        pmix_info_t _info;                                                              \
        size_t _sz;                                                                     \
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_verbose_output,                             \
                            "%s[%s:%d] MODEX RECV VALUE OPTIONAL FOR PROC %s KEY %s",   \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),                       \
                            __FILE__, __LINE__,                                         \
                            PRTE_NAME_PRINT((p)), (s)));                               \
        PMIX_LOAD_PROCID(&(_proc), prte_process_info.myproc.nspace, (p)->vpid);        \
        PMIX_INFO_LOAD(&_info, PMIX_IMMEDIATE, NULL, PMIX_BOOL);                        \
        (r) = PMIx_Get(&(_proc), (s), &(_info), 1, &(_kv));                             \
        if (NULL == _kv) {                                                              \
            (r) = PMIX_ERR_NOT_FOUND;                                                   \
        } else if (_kv->type != (t)) {                                                  \
            (r) = PMIX_ERR_TYPE_MISMATCH;                                               \
        } else if (PMIX_SUCCESS == (r)) {                                               \
            PMIX_VALUE_UNLOAD((r), _kv, (void**)(d), &_sz);                             \
        }                                                                               \
        if (NULL != _kv) {                                                              \
            PMIX_VALUE_RELEASE(_kv);                                                    \
        }                                                                               \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the prte_process_name_t of the proc that posted
 *     the data (prte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRTE_MODEX_RECV_VALUE(r, s, p, d, t)                                       \
    do {                                                                            \
        pmix_proc_t _proc;                                                          \
        pmix_value_t *_kv = NULL;                                                   \
        size_t _sz;                                                                 \
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_verbose_output,                         \
                            "%s[%s:%d] MODEX RECV VALUE FOR PROC %s KEY %s",        \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),                   \
                            __FILE__, __LINE__,                                     \
                            PRTE_NAME_PRINT((p)), (s)));                           \
        PMIX_LOAD_PROCID(&(_proc), prte_process_info.myproc.nspace, (p)->vpid);    \
        (r) = PMIx_Get(&(_proc), (s), NULL, 0, &(_kv));                             \
        if (NULL == _kv) {                                                          \
            (r) = PMIX_ERR_NOT_FOUND;                                               \
        } else if (_kv->type != (t)) {                                              \
            (r) = PMIX_ERR_TYPE_MISMATCH;                                           \
        } else if (PMIX_SUCCESS == (r)) {                                           \
            PMIX_VALUE_UNLOAD((r), _kv, (void**)(d), &_sz);                         \
        }                                                                           \
        if (NULL != _kv) {                                                          \
            PMIX_VALUE_RELEASE(_kv);                                                \
        }                                                                           \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the prte_process_name_t of the proc that posted
 *     the data (prte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned (char**)
 * sz - pointer to a location wherein the number of bytes
 *     in the data object can be returned (size_t)
 */
#define PRTE_MODEX_RECV_STRING(r, s, p, d, sz)                                     \
    do {                                                                            \
        pmix_proc_t _proc;                                                          \
        pmix_value_t *_kv = NULL;                                                   \
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_verbose_output,                         \
                            "%s[%s:%d] MODEX RECV STRING FOR PROC %s KEY %s",       \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),                   \
                            __FILE__, __LINE__,                                     \
                            PRTE_NAME_PRINT((p)), (s)));                           \
        *(d) = NULL;                                                                \
        *(sz) = 0;                                                                  \
        PMIX_LOAD_PROCID(&(_proc), prte_process_info.myproc.nspace, (p)->vpid);    \
        (r) = PMIx_Get(&(_proc), (s), NULL, 0, &(_kv));                             \
        if (NULL == _kv) {                                                          \
            (r) = PMIX_ERR_NOT_FOUND;                                               \
        } else if (PMIX_SUCCESS == (r)) {                                           \
            *(d) = _kv->data.bo.bytes;                                              \
            *(sz) = _kv->data.bo.size;                                              \
            _kv->data.bo.bytes = NULL; /* protect the data */                       \
        }                                                                           \
        if (NULL != _kv) {                                                          \
            PMIX_VALUE_RELEASE(_kv);                                                \
        }                                                                           \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - the MCA component that posted the data (prte_mca_base_component_t*)
 * p - pointer to the prte_process_name_t of the proc that posted
 *     the data (prte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned (char**)
 * sz - pointer to a location wherein the number of bytes
 *     in the data object can be returned (size_t)
 */
#define PRTE_MODEX_RECV(r, s, p, d, sz)                                \
    do {                                                                \
        char *_key;                                                     \
        _key = prte_mca_base_component_to_string((s));                 \
        PRTE_OUTPUT_VERBOSE((1, prte_pmix_verbose_output,             \
                            "%s[%s:%d] MODEX RECV FOR PROC %s KEY %s",  \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),       \
                            __FILE__, __LINE__,                         \
                            PRTE_NAME_PRINT((p)), _key));              \
        if (NULL == _key) {                                             \
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);                 \
            (r) = PRTE_ERR_OUT_OF_RESOURCE;                            \
        } else {                                                        \
            PRTE_MODEX_RECV_STRING((r), _key, (p), (d), (sz));         \
            free(_key);                                                 \
        }                                                               \
    } while(0);

#define PRTE_PMIX_SHOW_HELP    "prte.show.help"

/* some helper functions */
PRTE_EXPORT pmix_proc_state_t prte_pmix_convert_state(int state);
PRTE_EXPORT int prte_pmix_convert_pstate(pmix_proc_state_t);
PRTE_EXPORT pmix_status_t prte_pmix_convert_rc(int rc);
PRTE_EXPORT int prte_pmix_convert_status(pmix_status_t status);
PRTE_EXPORT pmix_status_t prte_pmix_convert_job_state_to_error(int state);
PRTE_EXPORT pmix_status_t prte_pmix_convert_proc_state_to_error(int state);
PRTE_EXPORT int prte_convert_jobid_to_nspace(pmix_nspace_t nspace, prte_jobid_t jobid);
PRTE_EXPORT int prte_convert_nspace_to_jobid(prte_jobid_t *jobid, pmix_nspace_t nspace);
PRTE_EXPORT void prte_convert_daemon_nspace(prte_jobid_t *jobid, pmix_nspace_t nspace);

#define PRTE_PMIX_CREATE_NSPACE(n, j)      \
    do {                                    \
        char *_tmp;                         \
        prte_asprintf(&_tmp, "%u", (j));   \
        PMIX_LOAD_NSPACE((n), _tmp);        \
        free(_tmp);                         \
    } while(0)

/* convert prte_jobid to pmix nspace */
#define PRTE_PMIX_CONVERT_JOBID(r, n, j) \
    (r) = prte_convert_jobid_to_nspace((n), (j))

/* convert prte_vpid to pmix rank */
#define PRTE_PMIX_CONVERT_VPID(r, v)               \
    do {                                            \
        if (PRTE_VPID_WILDCARD == (v)) {           \
            (r) = PMIX_RANK_WILDCARD;               \
        } else if (PRTE_VPID_INVALID == (v)) {     \
            (r) = PMIX_RANK_INVALID;                \
        } else {                                    \
            (r) = (v);                              \
        }                                           \
    } while(0)

/* convert prte_process_name_t to pmix_proc_t */
#define PRTE_PMIX_CONVERT_NAME(r, p, n)                        \
    do {                                                        \
        PRTE_PMIX_CONVERT_JOBID(r, (p)->nspace, (n)->jobid);   \
        if (PRTE_SUCCESS == (r)) {                             \
            PRTE_PMIX_CONVERT_VPID((p)->rank, (n)->vpid);      \
        }                                                       \
    } while(0)


/* register the daemon namespace and convert to jobid */
#define PRTE_PMIX_REGISTER_DAEMON_NSPACE(j, n)     \
    prte_convert_daemon_nspace((j), (n))

/* convert pmix_nspace_t to prte_jobid_t */
#define PRTE_PMIX_CONVERT_NSPACE(r, j, n)       \
    (r) = prte_convert_nspace_to_jobid((j), (n))

/* convert pmix rank to prte_vpid_t */
#define PRTE_PMIX_CONVERT_RANK(v, r)           \
    do {                                        \
        if (PMIX_RANK_WILDCARD == (r)) {        \
            (v) = PRTE_VPID_WILDCARD;          \
        } else if (PMIX_RANK_INVALID == (r)) {  \
            (v) = PRTE_VPID_INVALID;           \
        } else {                                \
            (v) = (r);                          \
        }                                       \
    } while(0)

/* convert pmix_proc_t to prte_process_name_t */
#define PRTE_PMIX_CONVERT_PROCT(r, n, p)                           \
    do {                                                            \
        PRTE_PMIX_CONVERT_NSPACE((r), &(n)->jobid, (p)->nspace);   \
        if (PRTE_SUCCESS == (r)) {                                 \
            PRTE_PMIX_CONVERT_RANK((n)->vpid, (p)->rank);          \
        }                                                           \
    } while(0)

PRTE_EXPORT void prte_pmix_value_load(pmix_value_t *v,
                                        prte_value_t *kv);

PRTE_EXPORT int prte_pmix_value_unload(prte_value_t *kv,
                                         const pmix_value_t *v);

PRTE_EXPORT int prte_pmix_register_cleanup(char *path,
                                             bool directory,
                                             bool ignore,
                                             bool jobscope);

/* protect against early versions of PMIx */
#ifndef PMIX_LOAD_KEY
#define PMIX_LOAD_KEY(a, b) \
    do {                                            \
        memset((a), 0, PMIX_MAX_KEYLEN+1);          \
        pmix_strncpy((a), (b), PMIX_MAX_KEYLEN);    \
    }while(0)
#endif

#ifndef PMIX_CHECK_KEY
#define PMIX_CHECK_KEY(a, b) \
    (0 == strncmp((a)->key, (b), PMIX_MAX_KEYLEN))
#endif

#ifndef PMIX_ERROR_LOG
#define PMIX_ERROR_LOG(r)          \
    prte_output(0, "[%s:%d] PMIx Error: %s", __FILE__, __LINE__, PMIx_Error_string((r)))
#endif
END_C_DECLS

#endif
