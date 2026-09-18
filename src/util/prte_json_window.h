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
 * Read a JSON document with a buffer that is smaller than the document. The
 * caller supplies the buffer and a function that reads the bytes of the
 * document. This code reads the document into the buffer in parts and moves a
 * cursor along it. The bytes behind the cursor are no longer necessary, so
 * the next part of the document replaces them. The buffer does not change
 * size.
 *
 * This code does not parse JSON. It finds the punctuation between one value
 * and the next. It gives the bytes of a value to the caller, and the caller
 * parses them. It measures a value that the caller does not want, and then it
 * discards that value. Make the buffer larger than the longest value that the
 * caller takes and larger than the longest member name. For a value that is
 * too large, this code returns PRTE_ERR_MEM_LIMIT_EXCEEDED.
 */

#ifndef PRTE_JSON_WINDOW_H
#define PRTE_JSON_WINDOW_H

#include "prte_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

BEGIN_C_DECLS

/**
 * @brief Read the next bytes of the document.
 *
 * The window calls this function each time it needs more of the document. A
 * source that has fewer bytes than the window asks for gives what it has. The
 * window then calls the function again.
 *
 * @param[out] dest    where to write the bytes
 * @param[in]  len     the number of bytes that dest can hold
 * @param[in]  cbdata  the value given to prte_json_window_init()
 *
 * @return the number of bytes written to dest. Zero ends the document. The
 *         window then completes the value it reads, or reports an incomplete
 *         document. A source that fails also returns zero, so the window
 *         reports that failure as an incomplete document. The caller learns
 *         the true cause from the source itself.
 */
typedef size_t (*prte_json_window_read_fn_t)(char *dest, size_t len, void *cbdata);

/**
 * @brief A window on a JSON document.
 *
 * The caller allocates this structure and the buffer. Both belong to the
 * window from prte_json_window_init() until the last call on the window.
 *
 *     buf  [ read already  | in the buffer, not read | free   ]
 *          0               cursor                    loaded   cap
 */
typedef struct {
    prte_json_window_read_fn_t read_fn;
    void *cbdata;
    char *buf;
    size_t cap;         /* the number of bytes that buf holds              */
    size_t loaded;      /* the number of bytes of the document in buf      */
    size_t cursor;      /* the number of those bytes that the window read  */
    size_t held_name_at; /* where the window holds a member name, or
                            SIZE_MAX when it holds no name                 */
    bool source_ended;  /* read_fn reported the end of the document        */
} prte_json_window_t;

/**
 * @brief The window calls this function one time for each member of an
 *        object.
 *
 * The cursor is at the value of the member. The function must read that value
 * with exactly one call to prte_json_window_take(), prte_json_window_skip(),
 * prte_json_window_walk_object() or prte_json_window_walk_array(). If the
 * function makes no such call, or more than one, the window loses its
 * position in the document and stops with an error.
 *
 * The name is a string in the buffer of the window and it ends with a NUL
 * byte. The first of those calls makes the space of the name available again,
 * so the function must not read the name after that call. A function that
 * needs the name later must copy it.
 *
 * A return value other than PRTE_SUCCESS stops the walk. The caller of
 * prte_json_window_walk_object() receives that value without a change, so
 * this function can refuse a document for its own reasons.
 *
 * @param[in,out] win     the window, at the value of the member
 * @param[in]     name    the name of the member, without the quotation marks
 *                        and with each escape as the document writes it
 * @param[in]     cbdata  the value given to prte_json_window_walk_object()
 */
typedef int (*prte_json_window_member_fn_t)(prte_json_window_t *win, const char *name,
                                            void *cbdata);

/**
 * @brief The window calls this function one time for each element of an
 *        array.
 *
 * The function reads the element, and can refuse the document, in the same
 * way as a function for a member.
 *
 * @param[in,out] win     the window, at the element
 * @param[in]     index   the position in the array. The first element is 0.
 * @param[in]     cbdata  the value given to prte_json_window_walk_array()
 */
typedef int (*prte_json_window_element_fn_t)(prte_json_window_t *win, size_t index,
                                             void *cbdata);

/**
 * @brief Point a window at a document.
 *
 * @param[out] win      the state of the window
 * @param[in]  read_fn  the function that reads the bytes of the document
 * @param[in]  cbdata   the value to give to read_fn
 * @param[in]  buf      the buffer to read the document into
 * @param[in]  cap      the number of bytes that buf holds. Each value that
 *                      the caller takes, and each member name, must fit in
 *                      the buffer. A number, and each of the words true,
 *                      false and null, also needs the byte that ends it.
 *                      Thus cap must be larger than the longest of those
 *                      values.
 */
PRTE_EXPORT void prte_json_window_init(prte_json_window_t *win,
                                       prte_json_window_read_fn_t read_fn, void *cbdata,
                                       char *buf, size_t cap);

/**
 * @brief Read an object and give each member to a function.
 *
 * @param[in,out] win      the window, at the object
 * @param[in]     handler  the function to call for each member, in the order
 *                         of the document
 * @param[in]     cbdata   the value to give to handler
 *
 * @retval PRTE_SUCCESS                 the window read the object to its end
 * @retval PRTE_ERR_JSON_PARSE_FAILURE  no object starts here, or the object
 *                                      is malformed, or the document ends in
 *                                      the middle of it
 * @retval PRTE_ERR_MEM_LIMIT_EXCEEDED  a member name, or a value that the
 *                                      handler took, does not fit the buffer
 * @return                              the value that a handler returned, if
 *                                      that value was not PRTE_SUCCESS
 */
PRTE_EXPORT int prte_json_window_walk_object(prte_json_window_t *win,
                                             prte_json_window_member_fn_t handler, void *cbdata);

/**
 * @brief Read an array and give each element to a function.
 *
 * @param[in,out] win      the window, at the array
 * @param[in]     handler  the function to call for each element, in the order
 *                         of the document
 * @param[in]     cbdata   the value to give to handler
 *
 * @retval PRTE_SUCCESS                 the window read the array to its end
 * @retval PRTE_ERR_JSON_PARSE_FAILURE  no array starts here, or the array is
 *                                      malformed, or the document ends in the
 *                                      middle of it
 * @retval PRTE_ERR_MEM_LIMIT_EXCEEDED  a value that the handler took does not
 *                                      fit the buffer
 * @return                              the value that a handler returned, if
 *                                      that value was not PRTE_SUCCESS
 */
PRTE_EXPORT int prte_json_window_walk_array(prte_json_window_t *win,
                                            prte_json_window_element_fn_t handler, void *cbdata);

/**
 * @brief Discard the value at the cursor.
 *
 * The window measures the value as it arrives and holds no part of it, so the
 * value can be of any size.
 *
 * @param[in,out] win  the window, at a value
 *
 * @retval PRTE_SUCCESS                 the window discarded the value
 * @retval PRTE_ERR_JSON_PARSE_FAILURE  no value starts here, or its brackets
 *                                      do not agree, or it nests more deeply
 *                                      than prte_json_scan.h permits, or the
 *                                      document ends in the middle of it
 */
PRTE_EXPORT int prte_json_window_skip(prte_json_window_t *win);

/**
 * @brief Give the bytes of the value at the cursor to the caller.
 *
 * The bytes are the value as the document writes it. They do not end with a
 * NUL byte. They are in the buffer of the window, so the caller can read them
 * only until the next call on this window.
 *
 * @param[in,out] win     the window, at a value
 * @param[out]    bytes   the first byte of the value
 * @param[out]    extent  the number of bytes that the value occupies
 *
 * @retval PRTE_SUCCESS                 the value starts at bytes
 * @retval PRTE_ERR_JSON_PARSE_FAILURE  no value starts here, or its brackets
 *                                      do not agree, or it nests more deeply
 *                                      than prte_json_scan.h permits, or the
 *                                      document ends in the middle of it
 * @retval PRTE_ERR_MEM_LIMIT_EXCEEDED  the value does not fit the buffer
 */
PRTE_EXPORT int prte_json_window_take(prte_json_window_t *win, const char **bytes,
                                      size_t *extent);

/**
 * @brief Read the remainder of the document and discard it.
 *
 * A walk stops at the byte that closes the value that the caller asked for.
 * The window reads no more of the document. If the source is a command that
 * runs, this is the difference between a command that completes and a command
 * that stops early. Call this function before you examine the exit status of
 * such a command.
 *
 * @param[in,out] win  the window, at any position
 */
PRTE_EXPORT void prte_json_window_drain(prte_json_window_t *win);

END_C_DECLS

#endif /* PRTE_JSON_WINDOW_H */
