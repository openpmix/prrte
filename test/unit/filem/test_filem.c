/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the filem framework.
 *
 * The substantive work of filem -- staging bytes across a live DVM
 * (raw_preposition_files / recv_files / write_handler) and symlinking
 * them into per-proc session directories (raw_link_local_files) -- is
 * asynchronous, progress-thread, multi-node I/O and cannot run without a
 * live DVM; it is covered by the integration harnesses.
 *
 * What *can* be exercised in isolation are the two pieces the base owns
 * with no I/O:
 *
 *   1. The three reference-counted request classes
 *      (prte_filem_base_process_set_t / _file_set_t / _request_t): their
 *      constructors must establish the documented defaults and their
 *      destructors must drain their lists and free their strings without
 *      crashing.
 *
 *   2. The default "none" module that filem_base_frame.c installs into
 *      the global prte_filem when no component is selected.  Every one of
 *      its entry points must be a safe no-op that returns PRTE_SUCCESS,
 *      preposition must still fire its completion callback (or the job
 *      would wedge forever at VM_READY), and -- the point of a recent
 *      fix -- the fault_handler slot must be non-NULL and callable, since
 *      routed_radix.c invokes prte_filem.fault_handler() unconditionally
 *      on every daemon-fault recovery even when filem is "none".
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* completion-callback bookkeeping for the preposition no-op test */
static bool cb_fired = false;
static int cb_status = -1;
static void completion_cb(int status, void *cbdata)
{
    cb_fired = true;
    cb_status = status;
    *((bool *) cbdata) = true;
}

/*
 * The request classes must construct to their documented defaults and
 * tear down cleanly.  A file_set owns its two path strings; the
 * destructor must free them (run under a leak checker to confirm).  A
 * request owns two lists; appending to them and releasing the request
 * must drain and release every member.
 */
static int test_classes(void)
{
    int failures = 0;
    prte_filem_base_process_set_t *pset;
    prte_filem_base_file_set_t *fset;
    prte_filem_base_request_t *req;

    /* process_set: source and sink both start INVALID */
    pset = PMIX_NEW(prte_filem_base_process_set_t);
    CHECK("process_set source rank invalid", PMIX_RANK_INVALID == pset->source.rank);
    CHECK("process_set sink rank invalid", PMIX_RANK_INVALID == pset->sink.rank);
    PMIX_RELEASE(pset);

    /* file_set: NULL targets, NONE hints, UNKNOWN type; destructor frees
     * the strings we hand it */
    fset = PMIX_NEW(prte_filem_base_file_set_t);
    CHECK("file_set local_target NULL", NULL == fset->local_target);
    CHECK("file_set remote_target NULL", NULL == fset->remote_target);
    CHECK("file_set local hint NONE", PRTE_FILEM_HINT_NONE == fset->local_hint);
    CHECK("file_set remote hint NONE", PRTE_FILEM_HINT_NONE == fset->remote_hint);
    CHECK("file_set type UNKNOWN", PRTE_FILEM_TYPE_UNKNOWN == fset->target_flag);
    fset->local_target = strdup("/some/local/path");
    fset->remote_target = strdup("some/remote/path");
    PMIX_RELEASE(fset);

    /* request: empty lists, UNKNOWN movement type; appending members and
     * releasing must drain both lists */
    req = PMIX_NEW(prte_filem_base_request_t);
    CHECK("request process_sets empty", 0 == pmix_list_get_size(&req->process_sets));
    CHECK("request file_sets empty", 0 == pmix_list_get_size(&req->file_sets));
    CHECK("request movement UNKNOWN", PRTE_FILEM_MOVE_TYPE_UNKNOWN == req->movement_type);

    pset = PMIX_NEW(prte_filem_base_process_set_t);
    pmix_list_append(&req->process_sets, &pset->super);
    fset = PMIX_NEW(prte_filem_base_file_set_t);
    fset->local_target = strdup("f");
    pmix_list_append(&req->file_sets, &fset->super);
    CHECK("request holds one process_set", 1 == pmix_list_get_size(&req->process_sets));
    CHECK("request holds one file_set", 1 == pmix_list_get_size(&req->file_sets));
    /* releasing the request must drain and release both members */
    PMIX_RELEASE(req);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_classes\n");
    }
    return failures;
}

/*
 * With no component selected, the global prte_filem is the "none" module
 * from filem_base_frame.c.  Every slot must be a safe no-op.  Most
 * importantly the fault_handler must be present and callable -- a NULL
 * there is a crash on daemon-fault recovery -- and preposition must fire
 * its completion callback so the state machine can advance.
 */
static int test_none_module(void)
{
    int failures = 0;
    bool done = false;

    /* every function pointer must be wired */
    CHECK("none init set", NULL != prte_filem.filem_init);
    CHECK("none finalize set", NULL != prte_filem.filem_finalize);
    CHECK("none fault_handler set", NULL != prte_filem.fault_handler);
    CHECK("none put set", NULL != prte_filem.put);
    CHECK("none get set", NULL != prte_filem.get);
    CHECK("none rm set", NULL != prte_filem.rm);
    CHECK("none wait set", NULL != prte_filem.wait);
    CHECK("none wait_all set", NULL != prte_filem.wait_all);
    CHECK("none preposition set", NULL != prte_filem.preposition_files);
    CHECK("none link_local set", NULL != prte_filem.link_local_files);

    /* the no-op transfer slots all return success */
    CHECK("none put succeeds", PRTE_SUCCESS == prte_filem.put(NULL));
    CHECK("none put_nb succeeds", PRTE_SUCCESS == prte_filem.put_nb(NULL));
    CHECK("none get succeeds", PRTE_SUCCESS == prte_filem.get(NULL));
    CHECK("none get_nb succeeds", PRTE_SUCCESS == prte_filem.get_nb(NULL));
    CHECK("none rm succeeds", PRTE_SUCCESS == prte_filem.rm(NULL));
    CHECK("none rm_nb succeeds", PRTE_SUCCESS == prte_filem.rm_nb(NULL));
    CHECK("none wait succeeds", PRTE_SUCCESS == prte_filem.wait(NULL));
    CHECK("none wait_all succeeds", PRTE_SUCCESS == prte_filem.wait_all(NULL));
    CHECK("none link_local succeeds", PRTE_SUCCESS == prte_filem.link_local_files(NULL, NULL));

    /* the fault handler must be safe to invoke (it is a no-op for none) */
    prte_filem.fault_handler(NULL);
    CHECK("none fault_handler returns", true);

    /* preposition must fire the completion callback with SUCCESS even
     * though it stages nothing -- otherwise the DVM hangs at VM_READY */
    cb_fired = false;
    cb_status = -1;
    CHECK("none preposition succeeds",
          PRTE_SUCCESS == prte_filem.preposition_files(NULL, completion_cb, &done));
    CHECK("none preposition fired callback", cb_fired);
    CHECK("none preposition callback status SUCCESS", PRTE_SUCCESS == cb_status);
    CHECK("none preposition passed cbdata", done);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_none_module\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* open the framework so its verbosity channel is valid and the
     * request classes are registered.  We deliberately do NOT run
     * prte_filem_base_select(), so the global prte_filem remains the
     * default "none" module that frame.c installs.
     */
    rc = pmix_mca_base_framework_open(&prte_filem_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "filem framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_classes();
    failures += test_none_module();

    (void) pmix_mca_base_framework_close(&prte_filem_base_framework);

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all filem unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d filem unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
