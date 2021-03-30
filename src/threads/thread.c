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
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/include/constants.h"
#include "src/threads/threads.h"
#include "src/threads/tsd.h"

bool prte_debug_threads = false;

static void prte_thread_construct(prte_thread_t *t);

static pthread_t prte_main_thread;

struct prte_tsd_key_value {
    prte_tsd_key_t key;
    prte_tsd_destructor_t destructor;
};

static struct prte_tsd_key_value *prte_tsd_key_values = NULL;
static int prte_tsd_key_values_count = 0;

PRTE_EXPORT PRTE_CLASS_INSTANCE(prte_thread_t, prte_object_t, prte_thread_construct, NULL);

/*
 * Constructor
 */
static void prte_thread_construct(prte_thread_t *t)
{
    t->t_run = 0;
    t->t_handle = (pthread_t) -1;
}

int prte_thread_start(prte_thread_t *t)
{
    int rc;

    if (PRTE_ENABLE_DEBUG) {
        if (NULL == t->t_run || t->t_handle != (pthread_t) -1) {
            return PRTE_ERR_BAD_PARAM;
        }
    }

    rc = pthread_create(&t->t_handle, NULL, (void *(*) (void *) ) t->t_run, t);

    return (rc == 0) ? PRTE_SUCCESS : PRTE_ERROR;
}

int prte_thread_join(prte_thread_t *t, void **thr_return)
{
    int rc = pthread_join(t->t_handle, thr_return);
    t->t_handle = (pthread_t) -1;
    return (rc == 0) ? PRTE_SUCCESS : PRTE_ERROR;
}

bool prte_thread_self_compare(prte_thread_t *t)
{
    return t->t_handle == pthread_self();
}

prte_thread_t *prte_thread_get_self(void)
{
    prte_thread_t *t = PRTE_NEW(prte_thread_t);
    t->t_handle = pthread_self();
    return t;
}

void prte_thread_kill(prte_thread_t *t, int sig)
{
    pthread_kill(t->t_handle, sig);
}

int prte_tsd_key_create(prte_tsd_key_t *key, prte_tsd_destructor_t destructor)
{
    int rc;
    rc = pthread_key_create(key, destructor);
    if ((0 == rc) && (pthread_self() == prte_main_thread)) {
        prte_tsd_key_values = (struct prte_tsd_key_value *)
            realloc(prte_tsd_key_values,
                    (prte_tsd_key_values_count + 1) * sizeof(struct prte_tsd_key_value));
        prte_tsd_key_values[prte_tsd_key_values_count].key = *key;
        prte_tsd_key_values[prte_tsd_key_values_count].destructor = destructor;
        prte_tsd_key_values_count++;
    }
    return rc;
}

int prte_tsd_keys_destruct()
{
    int i;
    void *ptr;
    for (i = 0; i < prte_tsd_key_values_count; i++) {
        if (PRTE_SUCCESS == prte_tsd_getspecific(prte_tsd_key_values[i].key, &ptr)) {
            if (NULL != prte_tsd_key_values[i].destructor) {
                prte_tsd_key_values[i].destructor(ptr);
                prte_tsd_setspecific(prte_tsd_key_values[i].key, NULL);
            }
        }
    }
    if (0 < prte_tsd_key_values_count) {
        free(prte_tsd_key_values);
        prte_tsd_key_values_count = 0;
    }
    return PRTE_SUCCESS;
}

void prte_thread_set_main()
{
    prte_main_thread = pthread_self();
}
