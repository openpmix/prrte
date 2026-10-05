/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "constants.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_environ.h"
#include "src/util/proc_info.h"
#include "src/util/prte_dvm_key.h"
#include "src/util/prte_hmac.h"
#include "src/util/prte_show_help.h"
#include "src/runtime/prte_globals.h"

uint8_t prte_dvm_key[PRTE_DVM_KEY_LEN] = {0};
bool prte_dvm_key_ready = false;

/* Nonces are HMAC(seed, counter): a seed nobody else can know makes them
 * unpredictable, and the counter makes them distinct.  That costs one read
 * of the random source per process rather than one per handshake. */
static pmix_mutex_t nonce_lock = PMIX_MUTEX_STATIC_INIT;
static uint8_t nonce_seed[PRTE_DVM_KEY_LEN];
static bool nonce_seeded = false;
static uint64_t nonce_counter = 0;

int prte_dvm_key_random(uint8_t *buf, size_t len)
{
    size_t got = 0;
    ssize_t n;
    int fd, err;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (0 > fd) {
        err = errno;
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-no-random",
                       true, prte_process_info.nodename, "/dev/urandom", strerror(err));
        return PRTE_ERR_SILENT;
    }
    while (got < len) {
        n = read(fd, buf + got, len - got);
        if (0 < n) {
            got += (size_t) n;
            continue;
        }
        if (0 > n && EINTR == errno) {
            continue;
        }
        err = (0 == n) ? EIO : errno;
        close(fd);
        prte_secure_zero(buf, len);
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-no-random",
                       true, prte_process_info.nodename, "/dev/urandom", strerror(err));
        return PRTE_ERR_SILENT;
    }
    close(fd);
    return PRTE_SUCCESS;
}

int prte_dvm_key_generate(void)
{
    int rc;

    rc = prte_dvm_key_random(prte_dvm_key, sizeof(prte_dvm_key));
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    prte_dvm_key_ready = true;
    return PRTE_SUCCESS;
}

void prte_dvm_key_to_hex(const uint8_t key[PRTE_DVM_KEY_LEN], char hex[PRTE_DVM_KEY_HEXLEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < PRTE_DVM_KEY_LEN; i++) {
        hex[2 * i] = digits[key[i] >> 4];
        hex[2 * i + 1] = digits[key[i] & 0x0f];
    }
    hex[PRTE_DVM_KEY_HEXLEN] = '\0';
}

static int hexval(char c)
{
    if ('0' <= c && '9' >= c) {
        return c - '0';
    }
    if ('a' <= c && 'f' >= c) {
        return c - 'a' + 10;
    }
    if ('A' <= c && 'F' >= c) {
        return c - 'A' + 10;
    }
    return -1;
}

bool prte_dvm_key_from_hex(const char *hex, size_t len, uint8_t key[PRTE_DVM_KEY_LEN])
{
    uint8_t tmp[PRTE_DVM_KEY_LEN];
    int hi, lo;
    size_t i;

    if (NULL == hex || PRTE_DVM_KEY_HEXLEN != len) {
        return false;
    }
    for (i = 0; i < PRTE_DVM_KEY_LEN; i++) {
        hi = hexval(hex[2 * i]);
        lo = hexval(hex[2 * i + 1]);
        if (0 > hi || 0 > lo) {
            prte_secure_zero(tmp, sizeof(tmp));
            return false;
        }
        tmp[i] = (uint8_t) ((hi << 4) | lo);
    }
    memcpy(key, tmp, sizeof(tmp));
    prte_secure_zero(tmp, sizeof(tmp));
    return true;
}

int prte_dvm_key_from_env(void)
{
    char *val;
    size_t len;
    bool ok;

    val = getenv(PRTE_DVM_KEY_ENVAR);
    if (NULL == val) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-not-delivered",
                       true, prte_process_info.nodename,
                       "in the " PRTE_DVM_KEY_ENVAR " environment variable",
                       "The variable is not set. The resource manager may have been told "
                       "not to forward the environment (for example, SLURM_EXPORT_ENV=NONE).");
        return PRTE_ERR_SILENT;
    }
    len = strlen(val);
    ok = prte_dvm_key_from_hex(val, len, prte_dvm_key);
    /* take it out of the environment whether or not it was usable */
    prte_dvm_key_scrub_env();
    if (!ok) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-malformed",
                       true, prte_process_info.nodename,
                       "the " PRTE_DVM_KEY_ENVAR " environment variable");
        return PRTE_ERR_SILENT;
    }
    prte_dvm_key_ready = true;
    return PRTE_SUCCESS;
}

void prte_dvm_key_scrub_env(void)
{
    char *val = getenv(PRTE_DVM_KEY_ENVAR);

    if (NULL == val) {
        return;
    }
    /* Wipe the string before dropping it: the bytes are what
     * /proc/<pid>/environ shows, and unsetenv() only drops the pointer to
     * them. */
    prte_secure_zero(val, strlen(val));
    unsetenv(PRTE_DVM_KEY_ENVAR);
}

void prte_dvm_key_setenv(char ***env)
{
    char hex[PRTE_DVM_KEY_HEXLEN + 1];

    prte_dvm_key_to_hex(prte_dvm_key, hex);
    PMIx_Setenv(PRTE_DVM_KEY_ENVAR, hex, true, env);
    prte_secure_zero(hex, sizeof(hex));
}

void prte_dvm_key_scrub_array(char **env)
{
    size_t len = strlen(PRTE_DVM_KEY_ENVAR);
    int i;

    if (NULL == env) {
        return;
    }
    for (i = 0; NULL != env[i]; i++) {
        if (0 == strncmp(env[i], PRTE_DVM_KEY_ENVAR, len) && '=' == env[i][len]) {
            prte_secure_zero(env[i], strlen(env[i]));
        }
    }
}

int prte_dvm_key_from_fd(int fd, int timeout_secs)
{
    char buf[PRTE_DVM_KEY_HEXLEN + 2];
    size_t have = 0;
    struct pollfd pfd;
    time_t deadline;
    ssize_t n;
    int rc, wait_ms;
    const char *why = NULL;

    /* Read one line - the hex key and its newline - a byte at a time, so
     * nothing after it is taken from whoever reads this descriptor next.
     * Bounded in time as well as length: a launcher that never forwards our
     * stdin must produce an error, not a daemon that waits forever. */
    deadline = time(NULL) + timeout_secs;
    while (have < sizeof(buf)) {
        wait_ms = (int) (deadline - time(NULL)) * 1000;
        if (0 >= wait_ms) {
            why = "Nothing arrived before the time allowed for it ran out.";
            break;
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1, wait_ms);
        if (0 > rc) {
            if (EINTR == errno) {
                continue;
            }
            why = strerror(errno);
            break;
        }
        if (0 == rc) {
            continue;
        }
        n = read(fd, buf + have, 1);
        if (0 > n) {
            if (EINTR == errno || EAGAIN == errno) {
                continue;
            }
            why = strerror(errno);
            break;
        }
        if (0 == n) {
            why = (0 == have) ? "Standard input was empty: the launch agent did not forward it."
                              : "Standard input ended part way through the key.";
            break;
        }
        if ('\n' == buf[have]) {
            break;
        }
        ++have;
    }
    if (NULL == why && sizeof(buf) == have) {
        why = "The line on standard input is longer than a key.";
    }
    if (NULL != why) {
        prte_secure_zero(buf, sizeof(buf));
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-not-delivered",
                       true, prte_process_info.nodename, "on standard input", why);
        return PRTE_ERR_SILENT;
    }
    if (!prte_dvm_key_from_hex(buf, have, prte_dvm_key)) {
        prte_secure_zero(buf, sizeof(buf));
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-malformed",
                       true, prte_process_info.nodename, "standard input");
        return PRTE_ERR_SILENT;
    }
    prte_secure_zero(buf, sizeof(buf));
    prte_dvm_key_ready = true;
    return PRTE_SUCCESS;
}

int prte_dvm_key_from_file(const char *path)
{
    char buf[PRTE_DVM_KEY_HEXLEN + 64];
    struct stat st;
    size_t have = 0;
    ssize_t n;
    int fd;
    const char *why = NULL;

    if (NULL == path) {
        return PRTE_ERR_BAD_PARAM;
    }
    /* O_NOFOLLOW: the checks below are about the file we read, and a
     * symlink would let them be made on one file and the key taken from
     * another */
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (0 > fd) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-file", true,
                       prte_process_info.nodename, path, strerror(errno));
        return PRTE_ERR_SILENT;
    }
    if (0 != fstat(fd, &st)) {
        why = strerror(errno);
    } else if (!S_ISREG(st.st_mode)) {
        why = "It is not a regular file.";
    } else if (st.st_uid != geteuid()) {
        why = "It is not owned by the user this daemon runs as.";
    } else if (0 != (st.st_mode & (S_IRWXG | S_IRWXO))) {
        why = "Its permissions let other users access it (it must be mode 0600 or 0400).";
    }
    while (NULL == why && have < sizeof(buf)) {
        n = read(fd, buf + have, sizeof(buf) - have);
        if (0 > n) {
            if (EINTR == errno) {
                continue;
            }
            why = strerror(errno);
            break;
        }
        if (0 == n) {
            break;
        }
        have += (size_t) n;
    }
    close(fd);
    if (NULL == why && sizeof(buf) == have) {
        why = "It is longer than a key.";
    }
    /* a trailing newline (or any trailing whitespace an editor left) is not
     * part of the key */
    while (NULL == why && 0 < have
           && ('\n' == buf[have - 1] || '\r' == buf[have - 1] || ' ' == buf[have - 1]
               || '\t' == buf[have - 1])) {
        --have;
    }
    if (NULL == why && !prte_dvm_key_from_hex(buf, have, prte_dvm_key)) {
        why = "It does not hold a key: exactly 64 hexadecimal digits are expected.";
    }
    prte_secure_zero(buf, sizeof(buf));
    if (NULL != why) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "dvm-key-file", true,
                       prte_process_info.nodename, path, why);
        return PRTE_ERR_SILENT;
    }
    prte_dvm_key_ready = true;
    return PRTE_SUCCESS;
}

int prte_dvm_key_nonce(uint8_t nonce[PRTE_DVM_KEY_LEN])
{
    uint8_t msg[12];
    uint64_t count;
    uint32_t pid;
    int rc, i;

    pmix_mutex_lock(&nonce_lock);
    if (!nonce_seeded) {
        rc = prte_dvm_key_random(nonce_seed, sizeof(nonce_seed));
        if (PRTE_SUCCESS != rc) {
            pmix_mutex_unlock(&nonce_lock);
            return rc;
        }
        nonce_seeded = true;
    }
    count = ++nonce_counter;
    pid = (uint32_t) getpid();
    for (i = 0; i < 8; i++) {
        msg[i] = (uint8_t) (count >> (56 - 8 * i));
    }
    for (i = 0; i < 4; i++) {
        msg[8 + i] = (uint8_t) (pid >> (24 - 8 * i));
    }
    prte_hmac_sha256(nonce_seed, sizeof(nonce_seed), msg, sizeof(msg), nonce);
    pmix_mutex_unlock(&nonce_lock);
    return PRTE_SUCCESS;
}

void prte_dvm_key_clear(void)
{
    prte_secure_zero(prte_dvm_key, sizeof(prte_dvm_key));
    prte_dvm_key_ready = false;
}
