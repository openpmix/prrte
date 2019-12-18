/* -*- C -*-
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"

#include <stdio.h>
#include <unistd.h>

#include "src/mca/pmix/pmix.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"

static pid_t pid;
static char hostname[PRRTE_MAXHOSTNAMELEN];

static void notification_fn(int status,
                            const opal_process_name_t *source,
                            opal_list_t *info, opal_list_t *results,
                            opal_pmix_notification_complete_fn_t cbfunc,
                            void *cbdata)
 {
    int peer_rank;

    fprintf(stderr, "prrte_notify: Name %s Host: %s Pid %ld status %d source %s\n",
            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
            hostname, (long)pid, status, PRRTE_NAME_PRINT(source));

    /** let the notifier know we are done */
    if (cbfunc) {
       cbfunc(PRRTE_ERR_HANDLERS_COMPLETE, NULL, NULL, NULL, cbdata);
    }

}

static void errhandler_reg_callbk(int status,
                                  size_t evhdlr_ref,
                                  void *cbdata)
{
    return;
}

int main(int argc, char* argv[])
{
    int rc;
    opal_value_t *kv;
    opal_list_t info;

    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "prrte_abort: couldn't init prrte - error code %d\n", rc);
        return rc;
    }
    pid = getpid();
    gethostname(hostname, sizeof(hostname));

    printf("prrte_notify: Name %s Host: %s Pid %ld\n",
           PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
           hostname, (long)pid);
    fflush(stdout);

    /* register the event handler */
    PRRTE_CONSTRUCT(&info, opal_list_t);
    kv = PRRTE_NEW(opal_value_t);
    kv->key = strdup(PRRTE_PMIX_EVENT_ORDER_PREPEND);
    kv->type = PRRTE_BOOL;
    kv->data.flag = true;
    opal_list_append(&info, &kv->super);

    opal_pmix.register_evhandler(NULL, &info,
                                notification_fn,
                                NULL, NULL);

    while (1) {
        usleep(100);
    }

    return 0;
}
