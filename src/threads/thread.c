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
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/threads/threads.h"
#include "src/threads/tsd.h"
#include "src/include/constants.h"

bool prrte_debug_threads = false;

static void prrte_thread_construct(prrte_thread_t *t);

static pthread_t prrte_main_thread;

struct prrte_tsd_key_value {
    prrte_tsd_key_t key;
    prrte_tsd_destructor_t destructor;
};

static struct prrte_tsd_key_value *prrte_tsd_key_values = NULL;
static int prrte_tsd_key_values_count = 0;

PRRTE_EXPORT PRRTE_CLASS_INSTANCE(prrte_thread_t,
                                prrte_object_t,
                                prrte_thread_construct, NULL);


/*
 * Constructor
 */
static void prrte_thread_construct(prrte_thread_t *t)
{
    t->t_run = 0;
    t->t_handle = (pthread_t) -1;
}

int prrte_thread_start(prrte_thread_t *t)
{
    int rc;

    if (PRRTE_ENABLE_DEBUG) {
        if (NULL == t->t_run || t->t_handle != (pthread_t) -1) {
            return PRRTE_ERR_BAD_PARAM;
        }
    }

    rc = pthread_create(&t->t_handle, NULL, (void*(*)(void*)) t->t_run, t);

    return (rc == 0) ? PRRTE_SUCCESS : PRRTE_ERROR;
}


int prrte_thread_join(prrte_thread_t *t, void **thr_return)
{
    int rc = pthread_join(t->t_handle, thr_return);
    t->t_handle = (pthread_t) -1;
    return (rc == 0) ? PRRTE_SUCCESS : PRRTE_ERROR;
}


bool prrte_thread_self_compare(prrte_thread_t *t)
{
    return t->t_handle == pthread_self();
}


prrte_thread_t *prrte_thread_get_self(void)
{
    prrte_thread_t *t = PRRTE_NEW(prrte_thread_t);
    t->t_handle = pthread_self();
    return t;
}

void prrte_thread_kill(prrte_thread_t *t, int sig)
{
    pthread_kill(t->t_handle, sig);
}

int prrte_tsd_key_create(prrte_tsd_key_t *key,
                    prrte_tsd_destructor_t destructor)
{
    int rc;
    rc = pthread_key_create(key, destructor);
    if ((0 == rc) && (pthread_self() == prrte_main_thread)) {
        prrte_tsd_key_values = (struct prrte_tsd_key_value *)realloc(prrte_tsd_key_values, (prrte_tsd_key_values_count+1) * sizeof(struct prrte_tsd_key_value));
        prrte_tsd_key_values[prrte_tsd_key_values_count].key = *key;
        prrte_tsd_key_values[prrte_tsd_key_values_count].destructor = destructor;
        prrte_tsd_key_values_count ++;
    }
    return rc;
}

int prrte_tsd_keys_destruct()
{
    int i;
    void * ptr;
    for (i=0; i<prrte_tsd_key_values_count; i++) {
        if(PRRTE_SUCCESS == prrte_tsd_getspecific(prrte_tsd_key_values[i].key, &ptr)) {
            if (NULL != prrte_tsd_key_values[i].destructor) {
                prrte_tsd_key_values[i].destructor(ptr);
                prrte_tsd_setspecific(prrte_tsd_key_values[i].key, NULL);
            }
        }
    }
    if (0 < prrte_tsd_key_values_count) {
        free(prrte_tsd_key_values);
        prrte_tsd_key_values_count = 0;
    }
    return PRRTE_SUCCESS;
}

void prrte_thread_set_main() {
    prrte_main_thread = pthread_self();
}
