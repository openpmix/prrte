/*
 * Copyright (c) 2010-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include "src/include/constants.h"

#include "src/event/event-internal.h"

/*
 * Globals
 */
prrte_event_base_t *prrte_sync_event_base=NULL;
static bool initialized = false;

int prrte_event_base_open(void)
{
    if (initialized) {
        return PRRTE_SUCCESS;
    }

    /* Declare our intent to use threads */
    prrte_event_use_threads();

    /* get our event base */
    if (NULL == (prrte_sync_event_base = event_base_new())) {
        return PRRTE_ERROR;
    }

    /* set the number of priorities */
    if (0 < PRRTE_EVENT_NUM_PRI) {
        prrte_event_base_priority_init(prrte_sync_event_base, PRRTE_EVENT_NUM_PRI);
    }

    initialized = true;
    return PRRTE_SUCCESS;
}

int prrte_event_base_close(void)
{
    if (!initialized) {
        return PRRTE_SUCCESS;
    }
    prrte_event_base_free(prrte_sync_event_base);

    initialized = false;
    return PRRTE_SUCCESS;

}

prrte_event_t* prrte_event_alloc(void)
{
    prrte_event_t *ev;

    ev = (prrte_event_t*)malloc(sizeof(prrte_event_t));
    return ev;
}

int prrte_event_assign(struct event *ev, prrte_event_base_t *evbase,
                      int fd, short arg, event_callback_fn cbfn, void *cbd)
{
#if PRRTE_HAVE_LIBEV
    event_set(ev, fd, arg, cbfn, cbd);
    event_base_set(evbase, ev);
#else
    event_assign(ev, evbase, fd, arg, cbfn, cbd);
#endif
    return 0;
}
