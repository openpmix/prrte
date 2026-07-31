/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/include/constants.h"

#include "src/event/event-internal.h"

/*
 * Globals
 */
prte_event_base_t *prte_sync_event_base = NULL;
static bool initialized = false;

int prte_event_base_open(void)
{
    if (initialized) {
        return PRTE_SUCCESS;
    }

    /* Declare our intent to use threads */
    prte_event_use_threads();

    /* get our event base */
    if (NULL == (prte_sync_event_base = event_base_new())) {
        return PRTE_ERROR;
    }
    /* PRTE tools "block" in their own loop over the event
     * base, so no progress thread is required */
    prte_event_base = prte_sync_event_base;

    initialized = true;
    return PRTE_SUCCESS;
}

int prte_event_base_close(void)
{
    if (!initialized) {
        return PRTE_SUCCESS;
    }
    prte_event_base_free(prte_sync_event_base);

    /* prte_event_base is an alias for the base we just freed, not a second
     * base - clear both.  Leaving them set handed every subsequent caller a
     * pointer to freed memory, and since PRTE's event macros take the base
     * as an argument rather than looking it up, that use would have been an
     * ordinary-looking prte_event_set() somewhere on a shutdown path. */
    prte_sync_event_base = NULL;
    prte_event_base = NULL;

    initialized = false;
    return PRTE_SUCCESS;
}

prte_event_t *prte_event_alloc(void)
{
    prte_event_t *ev;

    /* Zero-initialize the event.  Callers allocate the event here but
     * only assign it to an event base later (e.g. the odls launch-local
     * and prte_timer_t caddies event_assign it when they are activated),
     * and every such object's destructor frees the event with
     * prte_event_free() -- libevent's event_free(), which calls
     * event_del() and dereferences the event's internal fields.  On an
     * uninitialized (raw malloc'd) event those fields are garbage and the
     * free crashes; a zeroed event has a NULL base, which event_del()
     * handles gracefully, and a later event_assign() overwrites the
     * zeros. */
    ev = (prte_event_t *) calloc(1, sizeof(prte_event_t));
    return ev;
}

int prte_event_assign(struct event *ev, prte_event_base_t *evbase, int fd, short arg,
                      event_callback_fn cbfn, void *cbd)
{
    int rc;

    rc = event_assign(ev, evbase, fd, arg, cbfn, cbd);

    /* event_assign() reports failure as -1; hand back a PRTE code so a
     * caller that does check gets something it can compare against
     * PRTE_SUCCESS rather than a raw library return */
    return (0 == rc) ? PRTE_SUCCESS : PRTE_ERROR;
}

PMIX_CLASS_INSTANCE(prte_event_list_item_t, pmix_list_item_t, NULL, NULL);
