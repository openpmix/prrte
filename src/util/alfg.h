/*
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_ALFG_H
#define PRTE_ALFG_H

#include "prte_config.h"

#include "prte_stdint.h"

struct prte_rng_buff_t {
    uint32_t alfg[127];
    int tap1;
    int tap2;
};
typedef struct prte_rng_buff_t prte_rng_buff_t;

/* NOTE: UNLIKE OTHER PRTE FUNCTIONS, THIS FUNCTION RETURNS A 1 IF
 * SUCCESSFUL INSTEAD OF PRTE_SUCCESS */
PRTE_EXPORT int prte_srand(prte_rng_buff_t *buff, uint32_t seed);

PRTE_EXPORT uint32_t prte_rand(prte_rng_buff_t *buff);

PRTE_EXPORT int prte_random(void);

#endif /* PRTE_ALFG_H */
