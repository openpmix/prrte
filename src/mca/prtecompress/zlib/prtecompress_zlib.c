/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 *
 * Copyright (c) 2014-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <zlib.h>

#include "src/util/prte_environ.h"
#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/util/prte_environ.h"
#include "src/util/printf.h"

#include "constants.h"
#include "src/util/basename.h"

#include "src/mca/prtecompress/prtecompress.h"
#include "src/mca/prtecompress/base/base.h"

#include "prtecompress_zlib.h"

int prte_prtecompress_zlib_module_init(void)
{
    return PRTE_SUCCESS;
}

int prte_prtecompress_zlib_module_finalize(void)
{
    return PRTE_SUCCESS;
}

bool prte_prtecompress_zlib_compress_block(uint8_t *inbytes,
                                       size_t inlen,
                                       uint8_t **outbytes,
                                       size_t *olen)
{
    z_stream strm;
    size_t len;
    uint8_t *tmp;

    if (inlen < prte_prtecompress_base.prtecompress_limit) {
        return false;
    }
    prte_output_verbose(2, prte_prtecompress_base_framework.framework_output,
                        "COMPRESSING");

    /* set default output */
    *outbytes = NULL;
    *olen = 0;

    /* setup the stream */
    memset (&strm, 0, sizeof (strm));
    deflateInit (&strm, 9);

    /* get an upper bound on the required output storage */
    len = deflateBound(&strm, inlen);
    if (NULL == (tmp = (uint8_t*)malloc(len))) {
        return false;
    }
    strm.next_in = inbytes;
    strm.avail_in = inlen;

    /* allocating the upper bound guarantees zlib will
     * always successfully prtecompress into the available space */
    strm.avail_out = len;
    strm.next_out = tmp;

    deflate (&strm, Z_FINISH);
    deflateEnd (&strm);

    *outbytes = tmp;
    *olen = len - strm.avail_out;
    prte_output_verbose(2, prte_prtecompress_base_framework.framework_output,
                        "\tINSIZE %d OUTSIZE %d", (int)inlen, (int)*olen);
    return true;  // we did the prtecompression
}

bool prte_prtecompress_zlib_uncompress_block(uint8_t **outbytes, size_t olen,
                                         uint8_t *inbytes, size_t len)
{
    uint8_t *dest;
    z_stream strm;

    /* set the default error answer */
    *outbytes = NULL;
    prte_output_verbose(2, prte_prtecompress_base_framework.framework_output, "DECOMPRESS");

    /* setting destination to the fully deprtecompressed size */
    dest = (uint8_t*)malloc(olen);
    if (NULL == dest) {
        return false;
    }

    memset (&strm, 0, sizeof (strm));
    if (Z_OK != inflateInit(&strm)) {
        free(dest);
        return false;
    }
    strm.avail_in = len;
    strm.next_in = inbytes;
    strm.avail_out = olen;
    strm.next_out = dest;

    if (Z_STREAM_END != inflate (&strm, Z_FINISH)) {
        prte_output(0, "\tDECOMPRESS FAILED: %s", strm.msg);
    }
    inflateEnd (&strm);
    *outbytes = dest;
    prte_output_verbose(2, prte_prtecompress_base_framework.framework_output,
                        "\tINSIZE: %d OUTSIZE %d", (int)len, (int)olen);
    return true;
}
