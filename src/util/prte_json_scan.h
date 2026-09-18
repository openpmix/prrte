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

/*
 * Measure one JSON value in bytes.
 *
 * The scanner reads the bytes of a value and reports where the value ends. It
 * does not build the value and it does not keep any of the bytes, so it can
 * measure a value that is much larger than the memory available to it. The
 * state of the scanner is a fixed size.
 *
 * The scanner does not say whether a document is valid. It finds the end of
 * one value and it does nothing else.
 */

#ifndef PRTE_JSON_SCAN_H
#define PRTE_JSON_SCAN_H

#include "prte_config.h"

#include <stddef.h>
#include <stdint.h>

BEGIN_C_DECLS

/*
 * src/util/prte_json_window.c is the only caller of these functions in this
 * tree. A user of the window does not call them, and does not need to know
 * how a JSON value ends. The functions are PRTE_EXPORT because the unit tests
 * link libprrte, and libprrte gives them only its exported symbols.
 */

/* The scanner keeps one byte of state for each open bracket. A malformed
 * document must not increase that depth with no limit, so the scanner stops
 * at 64 levels. 64 bytes also keep the scanner state in one cache line. The
 * scanner refuses a document that nests more deeply than this. */
#define PRTE_JSON_SCAN_MAX_DEPTH 64

typedef enum {
    PRTE_JSON_SCAN_NEED_MORE = 0,
    PRTE_JSON_SCAN_DONE,
    PRTE_JSON_SCAN_MALFORMED
} prte_json_scan_status_t;

/**
 * @brief The state of a scanner. Its size does not change with the size of
 *        the value.
 *
 * The caller reads only the extent field. It holds the number of bytes that
 * the scanner measured, and it is the size of the value after the scanner
 * reports PRTE_JSON_SCAN_DONE.
 */
typedef struct {
    size_t extent;
    uint8_t state;
    uint8_t depth;
    uint8_t stack[PRTE_JSON_SCAN_MAX_DEPTH];
} prte_json_scan_t;

/**
 * @brief Prepare a scanner to measure one value. The extent starts at zero.
 *
 * @param[out] scan  the state of the scanner
 */
PRTE_EXPORT void prte_json_scan_init(prte_json_scan_t *scan);

/**
 * @brief Give the scanner more bytes of the value.
 *
 * Whitespace in front of the value is part of the value. A string, an object
 * and an array end after the quotation mark or the bracket that closes them.
 * A number, and each of the words true, false and null, ends at the first
 * whitespace byte or at the first ',' or '}' or ']'. The scanner does not
 * read that byte.
 *
 * If the caller divides a range of bytes into more than one call, the states,
 * the statuses and the total extent stay the same.
 *
 * After the scanner reports DONE or MALFORMED, it reports the same status for
 * each subsequent call and it reads no more bytes. A caller that supplies a
 * stream can stop at either status.
 *
 * @param[in,out] scan  a scanner from prte_json_scan_init()
 * @param[in]     buf   the bytes at the current position of the scanner
 * @param[in]     len   the number of bytes to read from buf
 * @param[out]    used  the number of bytes of buf that belong to the value
 *
 * @retval PRTE_JSON_SCAN_DONE       the value ends in these bytes
 * @retval PRTE_JSON_SCAN_NEED_MORE  the value continues after these bytes
 * @retval PRTE_JSON_SCAN_MALFORMED  a closing bracket does not agree with its
 *                                   opening bracket, or the value nests more
 *                                   deeply than PRTE_JSON_SCAN_MAX_DEPTH, or
 *                                   no value starts here
 */
PRTE_EXPORT prte_json_scan_status_t
prte_json_scan_feed(prte_json_scan_t *scan, const char *buf, size_t len, size_t *used);

/**
 * @brief Tell the scanner that the document has no more bytes.
 *
 * Use this function when prte_json_scan_feed() reports
 * PRTE_JSON_SCAN_NEED_MORE and you have no more bytes to give it. Only a
 * number, or one of the words true, false and null, can end at the end of a
 * document, because no other byte is necessary to end it. Every other value
 * ends with a quotation mark or a bracket, and the scanner reads that byte
 * itself.
 *
 * @param[in,out] scan  a scanner from prte_json_scan_init()
 *
 * @retval PRTE_JSON_SCAN_DONE       the value is complete and its extent is
 *                                   final
 * @retval PRTE_JSON_SCAN_MALFORMED  the document ends in the middle of the
 *                                   value
 */
PRTE_EXPORT prte_json_scan_status_t
prte_json_scan_finish(prte_json_scan_t *scan);

END_C_DECLS

#endif /* PRTE_JSON_SCAN_H */
