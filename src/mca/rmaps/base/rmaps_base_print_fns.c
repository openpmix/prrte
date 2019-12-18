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
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/if.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/threads/tsd.h"

#include "types.h"
#include "src/util/show_help.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"

#define PRRTE_RMAPS_PRINT_MAX_SIZE   50
#define PRRTE_RMAPS_PRINT_NUM_BUFS   16

static bool fns_init=false;
static prrte_tsd_key_t print_tsd_key;
static char* prrte_rmaps_print_null = "NULL";
typedef struct {
    char *buffers[PRRTE_RMAPS_PRINT_NUM_BUFS];
    int cntr;
} prrte_rmaps_print_buffers_t;

static void buffer_cleanup(void *value)
{
    int i;
    prrte_rmaps_print_buffers_t *ptr;

    if (NULL != value) {
        ptr = (prrte_rmaps_print_buffers_t*)value;
        for (i=0; i < PRRTE_RMAPS_PRINT_NUM_BUFS; i++) {
            free(ptr->buffers[i]);
        }
    }
}

static prrte_rmaps_print_buffers_t *get_print_buffer(void)
{
    prrte_rmaps_print_buffers_t *ptr;
    int ret, i;

    if (!fns_init) {
        /* setup the print_args function */
        if (PRRTE_SUCCESS != (ret = prrte_tsd_key_create(&print_tsd_key, buffer_cleanup))) {
            PRRTE_ERROR_LOG(ret);
            return NULL;
        }
        fns_init = true;
    }

    ret = prrte_tsd_getspecific(print_tsd_key, (void**)&ptr);
    if (PRRTE_SUCCESS != ret) return NULL;

    if (NULL == ptr) {
        ptr = (prrte_rmaps_print_buffers_t*)malloc(sizeof(prrte_rmaps_print_buffers_t));
        for (i=0; i < PRRTE_RMAPS_PRINT_NUM_BUFS; i++) {
            ptr->buffers[i] = (char *) malloc((PRRTE_RMAPS_PRINT_MAX_SIZE+1) * sizeof(char));
        }
        ptr->cntr = 0;
        ret = prrte_tsd_setspecific(print_tsd_key, (void*)ptr);
    }

    return (prrte_rmaps_print_buffers_t*) ptr;
}

char* prrte_rmaps_base_print_mapping(prrte_mapping_policy_t mapping)
{
    char *ret, *map, *mymap, *tmp;
    prrte_rmaps_print_buffers_t *ptr;

    if (PRRTE_MAPPING_CONFLICTED & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        return "CONFLICTED";
    }

    ptr = get_print_buffer();
    if (NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return prrte_rmaps_print_null;
    }
    /* cycle around the ring */
    if (PRRTE_RMAPS_PRINT_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    switch(PRRTE_GET_MAPPING_POLICY(mapping)) {
    case PRRTE_MAPPING_BYNODE:
        map = "BYNODE";
        break;
    case PRRTE_MAPPING_BYBOARD:
        map = "BYBOARD";
        break;
    case PRRTE_MAPPING_BYNUMA:
        map = "BYNUMA";
        break;
    case PRRTE_MAPPING_BYSOCKET:
        map = "BYSOCKET";
        break;
    case PRRTE_MAPPING_BYL3CACHE:
        map = "BYL3CACHE";
        break;
    case PRRTE_MAPPING_BYL2CACHE:
        map = "BYL2CACHE";
        break;
    case PRRTE_MAPPING_BYL1CACHE:
        map = "BYL1CACHE";
        break;
    case PRRTE_MAPPING_BYCORE:
        map = "BYCORE";
        break;
    case PRRTE_MAPPING_BYHWTHREAD:
        map = "BYHWTHREAD";
        break;
    case PRRTE_MAPPING_BYSLOT:
        map = "BYSLOT";
        break;
    case PRRTE_MAPPING_SEQ:
        map = "SEQUENTIAL";
        break;
    case PRRTE_MAPPING_BYUSER:
        map = "BYUSER";
        break;
    case PRRTE_MAPPING_BYDIST:
        map = "MINDIST";
        break;
    default:
        if (PRRTE_MAPPING_PPR & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
            map = "PPR";
        } else {
            map = "UNKNOWN";
        }
    }
    if (0 != strcmp(map, "PPR") && (PRRTE_MAPPING_PPR & PRRTE_GET_MAPPING_DIRECTIVE(mapping))) {
        prrte_asprintf(&mymap, "%s[PPR]:", map);
    } else {
        prrte_asprintf(&mymap, "%s:", map);
    }
    if (PRRTE_MAPPING_NO_USE_LOCAL & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prrte_asprintf(&tmp, "%sNO_USE_LOCAL,", mymap);
        free(mymap);
        mymap = tmp;
    }
    if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prrte_asprintf(&tmp, "%sNOOVERSUBSCRIBE,", mymap);
        free(mymap);
        mymap = tmp;
    } else if (PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prrte_asprintf(&tmp, "%sOVERSUBSCRIBE,", mymap);
        free(mymap);
        mymap = tmp;
    }
    if (PRRTE_MAPPING_SPAN & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
        prrte_asprintf(&tmp, "%sSPAN,", mymap);
        free(mymap);
        mymap = tmp;
    }

    /* remove the trailing mark */
    mymap[strlen(mymap)-1] = '\0';

    snprintf(ptr->buffers[ptr->cntr], PRRTE_RMAPS_PRINT_MAX_SIZE, "%s", mymap);
    free(mymap);
    ret = ptr->buffers[ptr->cntr];
    ptr->cntr++;

    return ret;
}

char* prrte_rmaps_base_print_ranking(prrte_ranking_policy_t ranking)
{
    switch(PRRTE_GET_RANKING_POLICY(ranking)) {
    case PRRTE_RANK_BY_NODE:
        return "NODE";
    case PRRTE_RANK_BY_BOARD:
        return "BOARD";
    case PRRTE_RANK_BY_NUMA:
        return "NUMA";
    case PRRTE_RANK_BY_SOCKET:
        return "SOCKET";
    case PRRTE_RANK_BY_CORE:
        return "CORE";
    case PRRTE_RANK_BY_HWTHREAD:
        return "HWTHREAD";
    case PRRTE_RANK_BY_SLOT:
        return "SLOT";
    default:
        return "UNKNOWN";
    }
}
