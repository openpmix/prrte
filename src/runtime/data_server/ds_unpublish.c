/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2016 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2025      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/data_server/ds.h"

pmix_status_t prte_ds_unpublish(pmix_proc_t *sender,
                                pmix_data_buffer_t *buffer,
                                pmix_data_buffer_t *answer)
{
    int32_t count;
    prte_data_object_t *data;
    pmix_status_t rc;
    int k;
    size_t n, ninfo;
    uint32_t i;
    char *str;
    prte_data_req_t rq;
    prte_info_item_t *ds1, *ds2;
    pmix_info_t *info;

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server got unpublish from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    PMIX_CONSTRUCT(&rq, prte_data_req_t);
    memcpy(&rq.proxy, sender, sizeof(pmix_proc_t));

    /* unpack the requestor */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &rq.requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&rq);
        return rc;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: unpublish data from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_NAME_PRINT(&rq.requestor));

    /* unpack the number of keys */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&rq);
        return rc;
    }
    if (0 == ninfo) {
        /* they forgot to send us the keys?? */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIX_DESTRUCT(&rq);
        return PMIX_ERR_BAD_PARAM;
    }

    /* unpack the keys */
    for (n = 0; n < ninfo; n++) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &str, &count, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&rq);
            return rc;
        }
        PMIx_Argv_append_nosize(&rq.keys, str);
        free(str);
    }

    /* unpack the number of directives, if any */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&rq);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        count = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DESTRUCT(&rq);
            return rc;
        }
        /* Scan the directives for things we care about, which for a
         * removal is who is asking - the identity the ownership test below
         * is answered about.  PMIX_RANGE is not among them: an owner may
         * take back what it published on any range, and the range named on
         * the unpublish does not narrow that. */
        for (n = 0; n < ninfo; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
                rq.uid = info[n].value.data.uint32;
            } else if (PMIx_Check_key(info[n].key, PMIX_GRPID)) {
                rq.gid = info[n].value.data.uint32;
            }
        }
        /* a relay unpublishing on behalf of a process in its own DVM.
         * After the scan, and before the ownership test below, which is
         * what says only the owner may remove an item. */
        prte_ds_check_requestor(&rq.requestor, &rq.uid, &rq.gid, info, ninfo);
        /* ignore anything else for now */
        PMIX_INFO_FREE(info, ninfo);
    }

    /* cycle across the provided keys */
    for (i = 0; NULL != rq.keys[i]; i++) {
        /* cycle across the stored data, looking for a match */
        for (k = 0; k < prte_data_store.store.size; k++) {
            data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
            if (NULL == data) {
                continue;
            }
            /* Published data is owned by the USER that published it, and
             * only its owner may remove it.
             *
             * The test used to be the publishing PROCESS - namespace and
             * rank both.  That is stricter than it looks: a process takes
             * no data with it when it exits, so an item published by a job
             * that has ended, or that died before it could unpublish, was
             * removable by nobody at all.  Its own user's next job could
             * read it, could not replace it (that is a duplicate) and could
             * not remove it, so the name was wedged for the life of the
             * DVM.  Keying on the uid is what lets a user clean up after
             * itself, and it moves no boundary that matters: nothing here
             * crosses between users. */
            if (!prte_data_server_owns(rq.uid, rq.gid, data)) {
                continue;
            }
            /* Ownership is the whole rule for removal, and the test above
             * has just established it: an owner may unpublish what it
             * published on ANY range.  Range and access permissions govern
             * who may READ an item, and neither belongs here - applying
             * the read rule refused an owner its own data whenever the
             * range was one the owner does not itself fall within (a
             * PMIX_RANGE_RM item admits only the host's namespace, a
             * PMIX_RANGE_CUSTOM one only the accessors it named), while
             * still answering SUCCESS, and the item then sat in the store
             * until its job ended. */
            /* see if we have this key */
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                if (PMIx_Check_key(ds1->info.key, rq.keys[i])) {
                    /* found it -  remove that item */
                    pmix_list_remove_item(&data->info, &ds1->super);
                    PMIX_RELEASE(ds1);
                }
            }
            /* if all the data has been removed, then remove the object */
            if (0 == pmix_list_get_size(&data->info)) {
                pmix_pointer_array_set_item(&prte_data_store.store, data->index, NULL);
                PMIX_RELEASE(data);
            }
        }
    }
    PMIX_DESTRUCT(&rq);

    if (PMIX_SUCCESS == rc) {
        pmix_status_t st = PMIX_SUCCESS;

        // send back an answer
        rc = PMIx_Data_pack(NULL, answer, &st, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PRTE_RML_RELIABLE_SEND(rc, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(answer);
        }
    }

    return rc;
}
