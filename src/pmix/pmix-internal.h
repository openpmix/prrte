/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_PMIX_H
#define PRRTE_PMIX_H

#include "prrte_config.h"
#include "types.h"

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "src/mca/mca.h"
#include "src/event/event-internal.h"
#include "src/dss/dss.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/include/hash_string.h"

#include PRRTE_PMIX_HEADER
#if ! PRRTE_PMIX_HEADER_GIVEN
#include "pmix_server.h"
#include "pmix_tool.h"
#include "pmix_version.h"
#endif

BEGIN_C_DECLS

PRRTE_EXPORT extern int prrte_pmix_verbose_output;

/* define a caddy for pointing to pmix_info_t that
 * are to be included in an answer */
typedef struct {
    prrte_list_item_t super;
    pmix_proc_t source;
    pmix_info_t *info;
    pmix_persistence_t persistence;
} prrte_ds_info_t;
PRRTE_CLASS_DECLARATION(prrte_ds_info_t);

/* define another caddy for putting statically defined
 * pmix_info_t objects on a list */
typedef struct {
    prrte_list_item_t super;
    pmix_info_t info;
} prrte_info_item_t;
PRRTE_CLASS_DECLARATION(prrte_info_item_t);


typedef struct {
    prrte_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool active;
    int status;
    char *msg;
} prrte_pmix_lock_t;

#define prrte_pmix_condition_wait(a,b)   pthread_cond_wait(a, &(b)->m_lock_pthread)

#define PRRTE_PMIX_CONSTRUCT_LOCK(l)                     \
    do {                                                \
        PRRTE_CONSTRUCT(&(l)->mutex, prrte_mutex_t);       \
        pthread_cond_init(&(l)->cond, NULL);            \
        (l)->active = true;                             \
        (l)->status = 0;                                \
        (l)->msg = NULL;                                \
        PRRTE_POST_OBJECT((l));                          \
    } while(0)

#define PRRTE_PMIX_DESTRUCT_LOCK(l)          \
    do {                                    \
        PRRTE_ACQUIRE_OBJECT((l));           \
        PRRTE_DESTRUCT(&(l)->mutex);          \
        pthread_cond_destroy(&(l)->cond);   \
        if (NULL != (l)->msg) {             \
            free((l)->msg);                 \
        }                                   \
    } while(0)


#if PRRTE_ENABLE_DEBUG
#define PRRTE_PMIX_ACQUIRE_THREAD(lck)                               \
    do {                                                            \
        prrte_mutex_lock(&(lck)->mutex);                             \
        if (prrte_debug_threads) {                                   \
            prrte_output(0, "Waiting for thread %s:%d",              \
                        __FILE__, __LINE__);                        \
        }                                                           \
        while ((lck)->active) {                                     \
            prrte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex);  \
        }                                                           \
        if (prrte_debug_threads) {                                   \
            prrte_output(0, "Thread obtained %s:%d",                 \
                        __FILE__, __LINE__);                        \
        }                                                           \
        (lck)->active = true;                                       \
    } while(0)
#else
#define PRRTE_PMIX_ACQUIRE_THREAD(lck)                               \
    do {                                                            \
        prrte_mutex_lock(&(lck)->mutex);                             \
        while ((lck)->active) {                                     \
            prrte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex);  \
        }                                                           \
        (lck)->active = true;                                       \
    } while(0)
#endif


#if PRRTE_ENABLE_DEBUG
#define PRRTE_PMIX_WAIT_THREAD(lck)                                  \
    do {                                                            \
        prrte_mutex_lock(&(lck)->mutex);                             \
        if (prrte_debug_threads) {                                   \
            prrte_output(0, "Waiting for thread %s:%d",              \
                        __FILE__, __LINE__);                        \
        }                                                           \
        while ((lck)->active) {                                     \
            prrte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex);  \
        }                                                           \
        if (prrte_debug_threads) {                                   \
            prrte_output(0, "Thread obtained %s:%d",                 \
                        __FILE__, __LINE__);                        \
        }                                                           \
        PRRTE_ACQUIRE_OBJECT(&lck);                                  \
        prrte_mutex_unlock(&(lck)->mutex);                           \
    } while(0)
#else
#define PRRTE_PMIX_WAIT_THREAD(lck)                                  \
    do {                                                            \
        prrte_mutex_lock(&(lck)->mutex);                             \
        while ((lck)->active) {                                     \
            prrte_pmix_condition_wait(&(lck)->cond, &(lck)->mutex);  \
        }                                                           \
        PRRTE_ACQUIRE_OBJECT(lck);                                   \
        prrte_mutex_unlock(&(lck)->mutex);                           \
    } while(0)
#endif


#if PRRTE_ENABLE_DEBUG
#define PRRTE_PMIX_RELEASE_THREAD(lck)                   \
    do {                                                \
        if (prrte_debug_threads) {                       \
            prrte_output(0, "Releasing thread %s:%d",    \
                        __FILE__, __LINE__);            \
        }                                               \
        (lck)->active = false;                          \
        pthread_cond_broadcast(&(lck)->cond);           \
        prrte_mutex_unlock(&(lck)->mutex);               \
    } while(0)
#else
#define PRRTE_PMIX_RELEASE_THREAD(lck)                   \
    do {                                                \
        assert(0 != prrte_mutex_trylock(&(lck)->mutex)); \
        (lck)->active = false;                          \
        pthread_cond_broadcast(&(lck)->cond);           \
        prrte_mutex_unlock(&(lck)->mutex);               \
    } while(0)
#endif


#define PRRTE_PMIX_WAKEUP_THREAD(lck)                    \
    do {                                                \
        prrte_mutex_lock(&(lck)->mutex);                 \
        (lck)->active = false;                          \
        PRRTE_POST_OBJECT(lck);                          \
        pthread_cond_broadcast(&(lck)->cond);           \
        prrte_mutex_unlock(&(lck)->mutex);               \
    } while(0)

/*
 * Count the fash for the the external RM
 */
#define PRRTE_HASH_JOBID( str, hash ){               \
    PRRTE_HASH_STR( str, hash );                     \
    hash &= ~(0x8000);                              \
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
#define PRRTE_MODEX_SEND_VALUE(r, sc, s, d, t)   \
    do {                                        \
        pmix_value_t _kv;                       \
        PMIX_VALUE_LOAD(&_kv, (d), (t));        \
        (r) = PMIx_Put((sc), (s), &(_kv));      \
                PRRTE_ERROR_LOG((r));            \
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
#define PRRTE_MODEX_SEND_STRING(r, sc, s, d, sz)     \
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
#define PRRTE_MODEX_SEND(r, sc, s, d, sz)                        \
    do {                                                        \
        char *_key;                                             \
        _key = prrte_mca_base_component_to_string((s));               \
        PRRTE_MODEX_SEND_STRING((r), (sc), _key, (d), (sz));     \
        free(_key);                                             \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process when we don't want the PMIx module
 * to request it from the server if not found:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the prrte_process_name_t of the proc that posted
 *     the data (prrte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRRTE_MODEX_RECV_VALUE_OPTIONAL(r, s, p, d, t)                                   \
    do {                                                                                \
        pmix_proc_t _proc;                                                              \
        pmix_value_t *_kv = NULL;                                                       \
        pmix_info_t _info;                                                              \
        size_t _sz;                                                                     \
        PRRTE_OUTPUT_VERBOSE((1, prrte_pmix_verbose_output,                               \
                            "%s[%s:%d] MODEX RECV VALUE OPTIONAL FOR PROC %s KEY %s",   \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),                         \
                            __FILE__, __LINE__,                                         \
                            PRRTE_NAME_PRINT((p)), (s)));                               \
        PRRTE_PMIX_CONVERT_NAME(&_proc, (p));                                            \
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
 * p - pointer to the prrte_process_name_t of the proc that posted
 *     the data (prrte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRRTE_MODEX_RECV_VALUE_IMMEDIATE(r, s, p, d, t)                                   \
    do {                                                                                 \
        pmix_proc_t _proc;                                                              \
        pmix_value_t *_kv = NULL;                                                       \
        pmix_info_t _info;                                                              \
        size_t _sz;                                                                     \
        PRRTE_OUTPUT_VERBOSE((1, prrte_pmix_verbose_output,                               \
                            "%s[%s:%d] MODEX RECV VALUE OPTIONAL FOR PROC %s KEY %s",   \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),                         \
                            __FILE__, __LINE__,                                         \
                            PRRTE_NAME_PRINT((p)), (s)));                               \
        PRRTE_PMIX_CONVERT_NAME(&_proc, (p));                                            \
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
 * p - pointer to the prrte_process_name_t of the proc that posted
 *     the data (prrte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     is to be returned
 * t - the expected data type
 */
#define PRRTE_MODEX_RECV_VALUE(r, s, p, d, t)                                    \
    do {                                                                        \
        pmix_proc_t _proc;                                                      \
        pmix_value_t *_kv = NULL;                                               \
        size_t _sz;                                                             \
        PRRTE_OUTPUT_VERBOSE((1, prrte_pmix_verbose_output,                       \
                            "%s[%s:%d] MODEX RECV VALUE FOR PROC %s KEY %s",    \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),                 \
                            __FILE__, __LINE__,                                 \
                            PRRTE_NAME_PRINT((p)), (s)));                       \
        PRRTE_PMIX_CONVERT_NAME(&_proc, (p));                                   \
        (r) = PMIx_Get(&(_proc), (s), NULL, 0, &(_kv));                         \
        if (NULL == _kv) {                                                      \
            (r) = PMIX_ERR_NOT_FOUND;                                           \
        } else if (_kv->type != (t)) {                                          \
            (r) = PMIX_ERR_TYPE_MISMATCH;                                       \
        } else if (PMIX_SUCCESS == (r)) {                                       \
            PMIX_VALUE_UNLOAD((r), _kv, (void**)(d), &_sz);                     \
        }                                                                       \
        if (NULL != _kv) {                                                      \
            PMIX_VALUE_RELEASE(_kv);                                            \
        }                                                                       \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the prrte_process_name_t of the proc that posted
 *     the data (prrte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned (char**)
 * sz - pointer to a location wherein the number of bytes
 *     in the data object can be returned (size_t)
 */
#define PRRTE_MODEX_RECV_STRING(r, s, p, d, sz)                                  \
    do {                                                                        \
        pmix_proc_t _proc;                                                      \
        pmix_value_t *_kv = NULL;                                               \
        PRRTE_OUTPUT_VERBOSE((1, prrte_pmix_verbose_output,                       \
                            "%s[%s:%d] MODEX RECV STRING FOR PROC %s KEY %s",   \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),                 \
                            __FILE__, __LINE__,                                 \
                            PRRTE_NAME_PRINT((p)), (s)));                       \
        *(d) = NULL;                                                            \
        *(sz) = 0;                                                              \
        PRRTE_PMIX_CONVERT_NAME(&_proc, (p));                                    \
        (r) = PMIx_Get(&(_proc), (s), NULL, 0, &(_kv));                         \
        if (NULL == _kv) {                                                      \
            (r) = PMIX_ERR_NOT_FOUND;                                           \
        } else if (PMIX_SUCCESS == (r)) {                                       \
            *(d) = _kv->data.bo.bytes;                                          \
            *(sz) = _kv->data.bo.size;                                          \
            _kv->data.bo.bytes = NULL; /* protect the data */                   \
        }                                                                       \
        if (NULL != _kv) {                                                      \
            PMIX_VALUE_RELEASE(_kv);                                            \
        }                                                                       \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - the MCA component that posted the data (prrte_mca_base_component_t*)
 * p - pointer to the prrte_process_name_t of the proc that posted
 *     the data (prrte_process_name_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned (char**)
 * sz - pointer to a location wherein the number of bytes
 *     in the data object can be returned (size_t)
 */
#define PRRTE_MODEX_RECV(r, s, p, d, sz)                                 \
    do {                                                                \
        char *_key;                                                     \
        _key = prrte_mca_base_component_to_string((s));                       \
        PRRTE_OUTPUT_VERBOSE((1, prrte_pmix_verbose_output,               \
                            "%s[%s:%d] MODEX RECV FOR PROC %s KEY %s",  \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),         \
                            __FILE__, __LINE__,                         \
                            PRRTE_NAME_PRINT((p)), _key));              \
        if (NULL == _key) {                                             \
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);                   \
            (r) = PRRTE_ERR_OUT_OF_RESOURCE;                             \
        } else {                                                        \
            PRRTE_MODEX_RECV_STRING((r), _key, (p), (d), (sz));          \
            free(_key);                                                 \
        }                                                               \
    } while(0);

#define PRRTE_PMIX_SHOW_HELP    "prrte.show.help"

/* some helper functions */
PRRTE_EXPORT pmix_proc_state_t prrte_pmix_convert_state(int state);
PRRTE_EXPORT int prrte_pmix_convert_pstate(pmix_proc_state_t);
PRRTE_EXPORT pmix_status_t prrte_pmix_convert_rc(int rc);
PRRTE_EXPORT int prrte_pmix_convert_status(pmix_status_t status);
PRRTE_EXPORT pmix_status_t prrte_pmix_convert_job_state_to_error(int state);

#define PRRTE_PMIX_CONVERT_JOBID(n, j) \
    (void)prrte_snprintf_jobid((n), PMIX_MAX_NSLEN, (j))

#define PRRTE_PMIX_CONVERT_VPID(r, v)        \
    do {                                    \
        if (PRRTE_VPID_WILDCARD == (v)) {    \
            (r) = PMIX_RANK_WILDCARD;       \
        } else {                            \
            (r) = (v);                      \
        }                                   \
    } while(0)
#define PRRTE_PMIX_CONVERT_NAME(p, n)                        \
    do {                                                    \
        PRRTE_PMIX_CONVERT_JOBID((p)->nspace, (n)->jobid);   \
        PRRTE_PMIX_CONVERT_VPID((p)->rank, (n)->vpid);       \
    } while(0)


#define PRRTE_PMIX_CONVERT_NSPACE(r, j, n)       \
    (r) = prrte_util_convert_string_to_jobid((j), (n))

#define PRRTE_PMIX_CONVERT_RANK(v, r)        \
    do {                                    \
        if (PMIX_RANK_WILDCARD == (r)) {    \
            (v) = PRRTE_VPID_WILDCARD;       \
        } else {                            \
            (v) = (r);                      \
        }                                   \
    } while(0)

#define PRRTE_PMIX_CONVERT_PROCT(r, n, p)                            \
    do {                                                            \
        PRRTE_PMIX_CONVERT_NSPACE((r), &(n)->jobid, (p)->nspace);    \
        if (PRRTE_SUCCESS == (r)) {                                  \
            PRRTE_PMIX_CONVERT_RANK((n)->vpid, (p)->rank);           \
        }                                                           \
    } while(0)

PRRTE_EXPORT void prrte_pmix_value_load(pmix_value_t *v,
                                        prrte_value_t *kv);

PRRTE_EXPORT int prrte_pmix_value_unload(prrte_value_t *kv,
                                         const pmix_value_t *v);

PRRTE_EXPORT int prrte_pmix_register_cleanup(char *path,
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
    prrte_output(0, "[%s:%d] PMIx Error: %s", __FILE__, __LINE__, PMIx_Error_string((r)))
#endif
END_C_DECLS

#endif
