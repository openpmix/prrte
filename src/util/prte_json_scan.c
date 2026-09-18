/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdbool.h>

#include "src/util/prte_json_scan.h"

typedef enum {
    SCAN_STATE_VALUE = 0,
    SCAN_STATE_STRING,
    SCAN_STATE_ESCAPE,
    SCAN_STATE_BARE,
    SCAN_STATE_DONE,
    SCAN_STATE_ERROR
} scan_state_t;

/* A number, and each of the words true, false and null, has no byte of its
 * own that ends it. It ends at the first byte that can come after it. That
 * byte is whitespace, or the punctuation that separates or closes. */
static bool ends_bare_value(char c)
{
    return (' ' == c || '\t' == c || '\n' == c || '\r' == c || ',' == c || '}' == c || ']' == c);
}

/*
 * Record the bytes that this call read, set the final state, and report the
 * status.
 *
 * DONE and MALFORMED are final states. After the scanner sets one of them, it
 * gives the same answer to each subsequent call.
 */
static prte_json_scan_status_t scan_return(prte_json_scan_t *scan,
                                           prte_json_scan_status_t status, size_t consumed,
                                           size_t *used)
{
    if (PRTE_JSON_SCAN_DONE == status) {
        scan->state = SCAN_STATE_DONE;
    } else if (PRTE_JSON_SCAN_MALFORMED == status) {
        scan->state = SCAN_STATE_ERROR;
    }
    scan->extent += consumed;
    *used = consumed;
    return status;
}

void prte_json_scan_init(prte_json_scan_t *scan)
{
    scan->extent = 0;
    scan->state = SCAN_STATE_VALUE;
    scan->depth = 0;
}

prte_json_scan_status_t
prte_json_scan_feed(prte_json_scan_t *scan,
                              const char *buf, size_t len, size_t *used)
{
    size_t i = 0;

    if (SCAN_STATE_DONE == scan->state) {
        return scan_return(scan, PRTE_JSON_SCAN_DONE, 0, used);
    }
    if (SCAN_STATE_ERROR == scan->state) {
        return scan_return(scan, PRTE_JSON_SCAN_MALFORMED, 0, used);
    }

    while (i < len) {
        char c = buf[i];

        if (SCAN_STATE_BARE == scan->state) {
            if (ends_bare_value(c)) {
                return scan_return(scan, PRTE_JSON_SCAN_DONE, i, used);
            }
            ++i;
            continue;
        }
        ++i;

        switch (scan->state) {
        case SCAN_STATE_VALUE:
            switch (c) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                break;
            case '"':
                scan->state = SCAN_STATE_STRING;
                break;
            case '{':
            case '[':
                if (PRTE_JSON_SCAN_MAX_DEPTH == scan->depth) {
                    return scan_return(scan, PRTE_JSON_SCAN_MALFORMED, i, used);
                }
                scan->stack[scan->depth++] = ('{' == c) ? '}' : ']';
                break;
            case '}':
            case ']':
                if (0 == scan->depth || c != scan->stack[scan->depth - 1]) {
                    return scan_return(scan, PRTE_JSON_SCAN_MALFORMED, i, used);
                }
                if (0 == --scan->depth) {
                    return scan_return(scan, PRTE_JSON_SCAN_DONE, i, used);
                }
                break;
            case ',':
            case ':':
                if (0 == scan->depth) {
                    return scan_return(scan, PRTE_JSON_SCAN_MALFORMED, i, used);
                }
                break;
            default:
                /* A number or a word inside brackets has no quotation mark
                 * or bracket of its own. Only one at the top level needs the
                 * byte that ends it. */
                if (0 == scan->depth) {
                    scan->state = SCAN_STATE_BARE;
                }
                break;
            }
            break;

        case SCAN_STATE_STRING:
            if ('\\' == c) {
                scan->state = SCAN_STATE_ESCAPE;
            } else if ('"' == c) {
                if (0 == scan->depth) {
                    return scan_return(scan, PRTE_JSON_SCAN_DONE, i, used);
                }
                scan->state = SCAN_STATE_VALUE;
            }
            break;

        case SCAN_STATE_ESCAPE:
            scan->state = SCAN_STATE_STRING;
            break;

        default:
            return scan_return(scan, PRTE_JSON_SCAN_MALFORMED, i, used);
        }
    }

    return scan_return(scan, PRTE_JSON_SCAN_NEED_MORE, len, used);
}

prte_json_scan_status_t
prte_json_scan_finish(prte_json_scan_t *scan)
{
    if (SCAN_STATE_BARE == scan->state || SCAN_STATE_DONE == scan->state) {
        scan->state = SCAN_STATE_DONE;
        return PRTE_JSON_SCAN_DONE;
    }
    scan->state = SCAN_STATE_ERROR;
    return PRTE_JSON_SCAN_MALFORMED;
}
