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

#include <stdint.h>
#include <string.h>

#include "constants.h"

#include "src/util/prte_json_scan.h"
#include "src/util/prte_json_window.h"

/*
 * Three numbers divide the buffer, and each one moves on its own:
 *
 *     buf  [ read already  | in the buffer, not read | free   ]
 *          0               cursor                    loaded   cap
 *
 * cap is the size that the caller allocated and it never changes. loaded is
 * the number of bytes of the document that are in the buffer now. It becomes
 * larger when the window reads more of the document, and smaller when the
 * window discards the bytes behind the cursor. cursor is the number of those
 * bytes that the window read.
 *
 * The buffer shows one part of a document that is usually much larger than
 * it. The cursor is a position in that part and not a position in the
 * document. When the window discards bytes, the part and the cursor move
 * together.
 */

/*
 * Report the number of bytes at the start of the buffer that the window can
 * discard.
 *
 * The window can discard every byte behind the cursor, except when it holds a
 * member name there. The window gives a handler the name where the name is in
 * the buffer and does not copy it. From the time the cursor moves past the
 * name until the handler no longer needs it, a discard would leave the handler
 * with the bytes of something else.
 */
static size_t window_discardable(const prte_json_window_t *win)
{
    return (win->held_name_at < win->cursor) ? win->held_name_at : win->cursor;
}

/*
 * Discard the bytes that the window no longer needs and read more of the
 * document.
 *
 * After the discard, the buffer contains only the bytes that the window
 * keeps. The cursor and a held name move with those bytes, so a value that
 * the scanner measures stays continuous from the cursor. This function reads
 * nothing when the window needs every byte in the buffer. A caller that
 * cannot continue after that must report the condition itself, because
 * nothing here makes it different from a source with no more bytes.
 */
static void window_refill(prte_json_window_t *win)
{
    size_t discard = window_discardable(win);
    size_t room;
    size_t got;

    if (0 < discard) {
        memmove(win->buf, win->buf + discard, win->loaded - discard);
        win->loaded -= discard;
        win->cursor -= discard;

        if (SIZE_MAX != win->held_name_at) {
            win->held_name_at -= discard;
        }
    }

    room = win->cap - win->loaded;

    if (win->source_ended || 0 == room) {
        return;
    }

    got = win->read_fn(win->buf + win->loaded, room, win->cbdata);
    win->loaded += got;

    if (0 == got) {
        win->source_ended = true;
    }
}

/*
 * Release a member name that the window holds.
 *
 * Each entry point that a handler can call starts here. At that first call
 * the name argument of the handler stops being readable, and the space of the
 * name becomes available again.
 */
static void window_release_name(prte_json_window_t *win)
{
    win->held_name_at = SIZE_MAX;
}

/*
 * Make sure that buf[cursor] is a byte of the document. Read more of the
 * document if it is not.
 *
 * PRTE_ERR_JSON_PARSE_FAILURE reports that the document ends at the cursor.
 * PRTE_ERR_MEM_LIMIT_EXCEEDED reports that the buffer is full and that the
 * window can discard no part of it. That takes a held member name and the
 * bytes up to its ':' filling the buffer between them, or a buffer with no
 * room in it at all.
 */
static int window_ensure_cursor_byte(prte_json_window_t *win)
{
    if (win->cursor < win->loaded) {
        return PRTE_SUCCESS;
    }

    if (win->source_ended) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    window_refill(win);

    if (win->cursor < win->loaded) {
        return PRTE_SUCCESS;
    }

    return win->source_ended ? PRTE_ERR_JSON_PARSE_FAILURE : PRTE_ERR_MEM_LIMIT_EXCEEDED;
}

/* The four bytes that JSON permits between one part of a document and the
 * next. */
static bool is_json_whitespace(char c)
{
    return (' ' == c || '\t' == c || '\n' == c || '\r' == c);
}

/*
 * Move the cursor forward across whitespace and report the byte at which it
 * stops. The cursor stays on that byte.
 *
 * Each step of a walk starts here. The window must see what comes next before
 * it can act on it, and JSON permits whitespace in front of all of it.
 */
static int window_peek_nonspace(prte_json_window_t *win, char *out)
{
    int err = window_ensure_cursor_byte(win);

    while (PRTE_SUCCESS == err && is_json_whitespace(win->buf[win->cursor])) {
        win->cursor++;
        err = window_ensure_cursor_byte(win);
    }

    if (PRTE_SUCCESS == err) {
        *out = win->buf[win->cursor];
    }

    return err;
}

/*
 * Move the cursor across one byte of punctuation that the walk knows must
 * come next. That byte is the '{' that opens an object, or the '[' that opens
 * an array, or the ':' between the name of a member and its value.
 *
 * Any other byte shows that the document does not have the shape that the
 * walk expects. The window then refuses the document.
 */
static int window_require(prte_json_window_t *win, char want)
{
    char c;
    int err = window_peek_nonspace(win, &c);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    if (want != c) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    win->cursor++;
    return PRTE_SUCCESS;
}

/* What the window does with the bytes of a value while it measures them. */
typedef enum {
    /* The cursor follows the scanner. The window discards the bytes as it
     * measures them, so the value can be of any size. */
    WINDOW_DISCARD_VALUE,
    /* The cursor stays on the first byte of the value. Every byte of the
     * value comes into the buffer together, so the value cannot be larger
     * than the buffer. */
    WINDOW_HOLD_VALUE
} window_value_mode_t;

/*
 * Measure the value at the cursor. Give the bytes to the scanner in parts
 * until the scanner reports where the value ends.
 *
 * WINDOW_HOLD_VALUE uses two positions. The cursor stays on the first byte of
 * the value. measured counts the bytes in front of the cursor that the
 * scanner read. WINDOW_DISCARD_VALUE uses only the cursor, and the cursor
 * moves instead.
 */
static int window_measure_value(prte_json_window_t *win, window_value_mode_t mode, size_t *extent)
{
    prte_json_scan_t scan;
    prte_json_scan_status_t status = PRTE_JSON_SCAN_NEED_MORE;
    size_t measured = 0;
    char first;

    /* Put the cursor on the first byte of the value. That byte decides
     * nothing here. It belongs to the scanner. */
    int err = window_peek_nonspace(win, &first);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    prte_json_scan_init(&scan);

    while (PRTE_JSON_SCAN_NEED_MORE == status) {
        size_t used;

        status = prte_json_scan_feed(&scan, win->buf + win->cursor + measured,
                                     win->loaded - win->cursor - measured, &used);

        if (WINDOW_HOLD_VALUE == mode) {
            measured += used;
        } else {
            win->cursor += used;
        }

        if (PRTE_JSON_SCAN_NEED_MORE == status) {
            if (win->source_ended) {
                /* A number, and each of the words true, false and null, can
                 * end at the end of a document, because no other byte is
                 * necessary to end it. Every other value that ends here is
                 * incomplete. */
                status = prte_json_scan_finish(&scan);
            } else if (WINDOW_HOLD_VALUE == mode && 0 == win->cursor
                       && win->loaded == win->cap) {
                /* The buffer holds only the start of this one value. The
                 * window can discard nothing and it has no room to read into,
                 * so no refill can bring in the remainder. This condition
                 * also ends the loop in this mode. WINDOW_DISCARD_VALUE
                 * cannot reach it, because the scanner asks for more bytes
                 * only after it reads all of the bytes that it got, and that
                 * moves the cursor and makes room. */
                return PRTE_ERR_MEM_LIMIT_EXCEEDED;
            } else {
                /* A refill moves the bytes that the window still needs to the
                 * start of the buffer, and it moves the cursor with them. Thus
                 * buf + cursor + measured is still the position of the bytes
                 * that the scanner did not measure. */
                window_refill(win);
            }
        }
    }

    if (PRTE_JSON_SCAN_DONE != status) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    *extent = scan.extent;
    return PRTE_SUCCESS;
}

/*
 * Bring all of the value at the cursor into the buffer, move the cursor
 * across it, and report where the value is.
 *
 *     before   [ read already | { " a " : 1 } , " b " : 2 ... ]
 *                               ^ cursor
 *
 *     after    [ read already | { " a " : 1 } , " b " : 2 ... ]
 *                               ^ *at         ^ cursor
 *                               \-- *extent --/
 *
 * The value stays where it is. Only the cursor moves.
 */
static int window_hold_value(prte_json_window_t *win, size_t *at, size_t *extent)
{
    int err = window_measure_value(win, WINDOW_HOLD_VALUE, extent);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    *at = win->cursor;
    win->cursor += *extent;
    return PRTE_SUCCESS;
}

/* Read one item of a container. An item is a member of an object or an
 * element of an array. Give the item to the function that asked for the
 * walk. */
typedef int (*window_item_fn_t)(prte_json_window_t *win, size_t index, void *cbdata);

/*
 * Read a container. A container is an object between '{' and '}', or an array
 * between '[' and ']'.
 *
 * The two containers are different only in what comes in front of an item, so
 * only the item function is different. The punctuation between items, the
 * empty container and the closing bracket are the same for both.
 */
static int window_walk_container(prte_json_window_t *win, char open, char close,
                                 window_item_fn_t item, void *cbdata)
{
    size_t index = 0;
    char c;
    int err = window_require(win, open);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    err = window_peek_nonspace(win, &c);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    if (close == c) {
        win->cursor++;
        return PRTE_SUCCESS;
    }

    do {
        err = item(win, index, cbdata);

        if (PRTE_SUCCESS != err) {
            return err;
        }

        err = window_peek_nonspace(win, &c);

        if (PRTE_SUCCESS != err) {
            return err;
        }

        win->cursor++;
        index++;
    } while (',' == c);

    return (close == c) ? PRTE_SUCCESS : PRTE_ERR_JSON_PARSE_FAILURE;
}

/* What a walk of an object needs and a walk of a container does not hold. */
typedef struct {
    prte_json_window_member_fn_t handler;
    void *cbdata;
} window_object_walk_t;

/*
 * Read one member of an object and give it to the function of the caller.
 *
 * A member is a name, a ':' and a value. This function reads the name and the
 * ':'. The value is what the window gives to the function of the caller, and
 * that function reads it.
 */
static int window_object_item(prte_json_window_t *win, size_t index __prte_attribute_unused__,
                              void *cbdata)
{
    window_object_walk_t *walk = (window_object_walk_t *) cbdata;
    size_t at;
    size_t extent;
    char c;
    int err = window_peek_nonspace(win, &c);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    if ('"' != c) {
        return PRTE_ERR_JSON_PARSE_FAILURE;
    }

    err = window_hold_value(win, &at, &extent);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    /* The name stays where it is, and a NUL byte replaces the quotation mark
     * that closes it. Thus the window needs no second buffer for names. The
     * name is now behind the cursor, where a refill would move it, so the
     * window holds it until the first call that the handler makes. The ':'
     * alone can cause a refill. This is why the window holds the name first,
     * and why it reads the name argument through held_name_at. */
    win->held_name_at = at;
    win->buf[at + extent - 1] = '\0';

    err = window_require(win, ':');

    if (PRTE_SUCCESS == err) {
        err = walk->handler(win, win->buf + win->held_name_at + 1, walk->cbdata);
    }

    /* A handler can return without a call on the window, so the window
     * releases the name here also. */
    window_release_name(win);
    return err;
}

/* What a walk of an array needs and a walk of a container does not hold. */
typedef struct {
    prte_json_window_element_fn_t handler;
    void *cbdata;
} window_array_walk_t;

/*
 * Give one element of an array to the function of the caller.
 *
 * An element is a value with nothing in front of it, so there is nothing to
 * read first. This function exists to give a walk of an array the same shape
 * as a walk of an object.
 */
static int window_array_item(prte_json_window_t *win, size_t index, void *cbdata)
{
    window_array_walk_t *walk = (window_array_walk_t *) cbdata;

    return walk->handler(win, index, walk->cbdata);
}

void prte_json_window_init(prte_json_window_t *win, prte_json_window_read_fn_t read_fn,
                           void *cbdata, char *buf, size_t cap)
{
    win->read_fn = read_fn;
    win->cbdata = cbdata;
    win->buf = buf;
    win->cap = cap;
    win->loaded = 0;
    win->cursor = 0;
    win->held_name_at = SIZE_MAX;
    win->source_ended = false;
}

int prte_json_window_skip(prte_json_window_t *win)
{
    size_t extent;

    window_release_name(win);
    return window_measure_value(win, WINDOW_DISCARD_VALUE, &extent);
}

int prte_json_window_take(prte_json_window_t *win, const char **bytes, size_t *extent)
{
    size_t at;
    int err;

    window_release_name(win);
    err = window_hold_value(win, &at, extent);

    if (PRTE_SUCCESS == err) {
        *bytes = win->buf + at;
    }

    return err;
}

int prte_json_window_walk_object(prte_json_window_t *win, prte_json_window_member_fn_t handler,
                                 void *cbdata)
{
    window_object_walk_t walk = {handler, cbdata};

    window_release_name(win);
    return window_walk_container(win, '{', '}', window_object_item, &walk);
}

int prte_json_window_walk_array(prte_json_window_t *win, prte_json_window_element_fn_t handler,
                                void *cbdata)
{
    window_array_walk_t walk = {handler, cbdata};

    window_release_name(win);
    return window_walk_container(win, '[', ']', window_array_item, &walk);
}

void prte_json_window_drain(prte_json_window_t *win)
{
    window_release_name(win);

    /* The cursor moves to the end of the bytes that the window read, so the
     * window keeps nothing and each refill replaces all of the buffer. A
     * buffer of no size is the one case that a read cannot pass, and it never
     * reports the end of the document. */
    while (!win->source_ended && 0 < win->cap) {
        win->cursor = win->loaded;
        window_refill(win);
    }
}
