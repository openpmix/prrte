/*
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_ALFG_H
#define PRRTE_ALFG_H

#include "prrte_config.h"

#include "prrte_stdint.h"


struct prrte_rng_buff_t {
    uint32_t alfg[127];
    int tap1;
    int tap2;
};
typedef struct prrte_rng_buff_t prrte_rng_buff_t;


/* NOTE: UNLIKE OTHER PRRTE FUNCTIONS, THIS FUNCTION RETURNS A 1 IF
 * SUCCESSFUL INSTEAD OF PRRTE_SUCCESS */
PRRTE_EXPORT int prrte_srand(prrte_rng_buff_t *buff, uint32_t seed);

PRRTE_EXPORT uint32_t prrte_rand(prrte_rng_buff_t *buff);

PRRTE_EXPORT int prrte_random(void);

#endif /* PRRTE_ALFG_H */
