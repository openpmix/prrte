/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/threads/tsd.h"
#include "src/util/if.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "types.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

#define PRTE_RMAPS_PRINT_MAX_SIZE 50
#define PRTE_RMAPS_PRINT_NUM_BUFS 16

static bool fns_init = false;
static prte_tsd_key_t print_tsd_key;
static char *prte_rmaps_print_null = "NULL";
typedef struct {
    char *buffers[PRTE_RMAPS_PRINT_NUM_BUFS];
    int cntr;
} prte_rmaps_print_buffers_t;

static void buffer_cleanup(void *value)
{
    int i;
    prte_rmaps_print_buffers_t *ptr;

    if (NULL != value) {
        ptr = (prte_rmaps_print_buffers_t *) value;
        for (i = 0; i < PRTE_RMAPS_PRINT_NUM_BUFS; i++) {
            free(ptr->buffers[i]);
        }
    }
}

static prte_rmaps_print_buffers_t *get_print_buffer(void)
{
    prte_rmaps_print_buffers_t *ptr;
    int ret, i;

    if (!fns_init) {
        /* setup the print_args function */
        if (PRTE_SUCCESS != (ret = prte_tsd_key_create(&print_tsd_key, buffer_cleanup))) {
            PRTE_ERROR_LOG(ret);
            return NULL;
        }
        fns_init = true;
    }

    ret = prte_tsd_getspecific(print_tsd_key, (void **) &ptr);
    if (PRTE_SUCCESS != ret)
        return NULL;

    if (NULL == ptr) {
        ptr = (prte_rmaps_print_buffers_t *) malloc(sizeof(prte_rmaps_print_buffers_t));
        for (i = 0; i < PRTE_RMAPS_PRINT_NUM_BUFS; i++) {
            ptr->buffers[i] = (char *) malloc((PRTE_RMAPS_PRINT_MAX_SIZE + 1) * sizeof(char));
        }
        ptr->cntr = 0;
        ret = prte_tsd_setspecific(print_tsd_key, (void *) ptr);
    }

    return (prte_rmaps_print_buffers_t *) ptr;
}

char *prte_rmaps_base_print_mapping(prte_mapping_policy_t mapping)
{
    char *ret, *map, *mymap, *tmp;
    prte_rmaps_print_buffers_t *ptr;

    if (PRTE_MAPPING_CONFLICTED & PRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        return "CONFLICTED";
    }

    ptr = get_print_buffer();
    if (NULL == ptr) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return prte_rmaps_print_null;
    }
    /* cycle around the ring */
    if (PRTE_RMAPS_PRINT_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    switch (PRTE_GET_MAPPING_POLICY(mapping)) {
    case PRTE_MAPPING_BYNODE:
        map = "BYNODE";
        break;
    case PRTE_MAPPING_BYPACKAGE:
        map = "BYPACKAGE";
        break;
    case PRTE_MAPPING_BYL3CACHE:
        map = "BYL3CACHE";
        break;
    case PRTE_MAPPING_BYL2CACHE:
        map = "BYL2CACHE";
        break;
    case PRTE_MAPPING_BYL1CACHE:
        map = "BYL1CACHE";
        break;
    case PRTE_MAPPING_BYCORE:
        map = "BYCORE";
        break;
    case PRTE_MAPPING_BYHWTHREAD:
        map = "BYHWTHREAD";
        break;
    case PRTE_MAPPING_BYSLOT:
        map = "BYSLOT";
        break;
    case PRTE_MAPPING_SEQ:
        map = "SEQUENTIAL";
        break;
    case PRTE_MAPPING_BYUSER:
        map = "BYUSER";
        break;
    case PRTE_MAPPING_BYDIST:
        map = "MINDIST";
        break;
    default:
        if (PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(mapping)) {
            map = "PPR";
        } else {
            map = "UNKNOWN";
        }
    }
    if (0 != strcmp(map, "PPR") && (PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(mapping))) {
        prte_asprintf(&mymap, "%s[PPR]:", map);
    } else {
        prte_asprintf(&mymap, "%s:", map);
    }
    if (PRTE_MAPPING_NO_USE_LOCAL & PRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prte_asprintf(&tmp, "%sNO_USE_LOCAL,", mymap);
        free(mymap);
        mymap = tmp;
    }
    if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prte_asprintf(&tmp, "%sNOOVERSUBSCRIBE,", mymap);
        free(mymap);
        mymap = tmp;
    } else if (PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prte_asprintf(&tmp, "%sOVERSUBSCRIBE,", mymap);
        free(mymap);
        mymap = tmp;
    }
    if (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prte_asprintf(&tmp, "%sSPAN,", mymap);
        free(mymap);
        mymap = tmp;
    }

    /* remove the trailing mark */
    mymap[strlen(mymap) - 1] = '\0';

    snprintf(ptr->buffers[ptr->cntr], PRTE_RMAPS_PRINT_MAX_SIZE, "%s", mymap);
    free(mymap);
    ret = ptr->buffers[ptr->cntr];
    ptr->cntr++;

    return ret;
}

char *prte_rmaps_base_print_ranking(prte_ranking_policy_t ranking)
{
    switch (PRTE_GET_RANKING_POLICY(ranking)) {
    case PRTE_RANK_BY_NODE:
        return "NODE";
    case PRTE_RANK_BY_PACKAGE:
        return "PACKAGE";
    case PRTE_RANK_BY_CORE:
        return "CORE";
    case PRTE_RANK_BY_HWTHREAD:
        return "HWTHREAD";
    case PRTE_RANK_BY_SLOT:
        return "SLOT";
    default:
        return "UNKNOWN";
    }
}
