/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the DVM key and the primitives behind it.
 *
 * The digest and the HMAC are checked against the published vectors (FIPS
 * 180-4's examples and RFC 4231): a hash that is merely self-consistent
 * would still let two daemons agree with each other, so agreement between
 * daemons proves nothing about it.  The key's delivery channels are checked
 * for what they refuse as much as what they accept - a key file other users
 * can access, a symlink, a short or garbled key - because each refusal is a
 * way the key could otherwise stop being private to the DVM or be replaced.
 */

#include "prte_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "constants.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/prte_dvm_key.h"
#include "src/util/prte_hmac.h"

int test_dvm_key(void);

#define CHECK(label, cond)                                             \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL [dvm_key:%s]: %s\n", label, #cond);  \
            failures++;                                                \
        }                                                              \
    } while (0)

static void to_hex(const uint8_t *d, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static bool sha256_is(const void *msg, size_t len, const char *want)
{
    prte_sha256_ctx_t ctx;
    uint8_t d[PRTE_SHA256_DIGEST_LEN];
    char hex[2 * PRTE_SHA256_DIGEST_LEN + 1];

    prte_sha256_init(&ctx);
    prte_sha256_update(&ctx, msg, len);
    prte_sha256_final(&ctx, d);
    to_hex(d, sizeof(d), hex);
    return (0 == strcmp(hex, want));
}

static bool hmac_is(const uint8_t *key, size_t keylen, const char *msg, const char *want)
{
    uint8_t d[PRTE_SHA256_DIGEST_LEN];
    char hex[2 * PRTE_SHA256_DIGEST_LEN + 1];

    prte_hmac_sha256(key, keylen, msg, strlen(msg), d);
    to_hex(d, sizeof(d), hex);
    return (0 == strcmp(hex, want));
}

static int test_digests(void)
{
    int failures = 0, i;
    prte_sha256_ctx_t ctx;
    uint8_t d[PRTE_SHA256_DIGEST_LEN], key[131];
    char hex[2 * PRTE_SHA256_DIGEST_LEN + 1], *million;

    /* FIPS 180-4 / NIST examples, including both padding boundaries */
    CHECK("sha256 empty",
          sha256_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK("sha256 abc",
          sha256_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK("sha256 two blocks",
          sha256_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* a million 'a's, fed in uneven pieces so that block boundaries fall in
     * the middle of updates */
    million = malloc(1000000);
    CHECK("alloc", NULL != million);
    if (NULL != million) {
        size_t off = 0, step = 1;
        memset(million, 'a', 1000000);
        prte_sha256_init(&ctx);
        while (off < 1000000) {
            size_t take = (1000000 - off < step) ? 1000000 - off : step;
            prte_sha256_update(&ctx, million + off, take);
            off += take;
            step = (step * 7) % 997 + 1;
        }
        prte_sha256_final(&ctx, d);
        to_hex(d, sizeof(d), hex);
        CHECK("sha256 million a",
              0 == strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
        free(million);
    }

    /* RFC 4231 test cases 1, 2 and 6 - the last with a key longer than a
     * block, which has to be hashed first */
    memset(key, 0x0b, 20);
    CHECK("hmac rfc4231 #1",
          hmac_is(key, 20, "Hi There",
                  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK("hmac rfc4231 #2",
          hmac_is((const uint8_t *) "Jefe", 4, "what do ya want for nothing?",
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    memset(key, 0xaa, sizeof(key));
    CHECK("hmac rfc4231 #6",
          hmac_is(key, sizeof(key), "Test Using Larger Than Block-Size Key - Hash Key First",
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

    /* the comparison a received proof is checked with */
    memset(key, 1, 32);
    memset(d, 1, 32);
    CHECK("equal buffers compare equal", prte_secure_equal(key, d, 32));
    for (i = 0; i < 32; i += 31) {
        d[i] ^= 0x80;
        CHECK("a difference anywhere is seen", !prte_secure_equal(key, d, 32));
        d[i] ^= 0x80;
    }
    return failures;
}

static int test_hex(void)
{
    int failures = 0;
    uint8_t key[PRTE_DVM_KEY_LEN], back[PRTE_DVM_KEY_LEN];
    char hex[PRTE_DVM_KEY_HEXLEN + 1];
    size_t i;

    for (i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t) (i * 37 + 11);
    }
    prte_dvm_key_to_hex(key, hex);
    CHECK("hex is the right length", PRTE_DVM_KEY_HEXLEN == strlen(hex));
    CHECK("hex round trips",
          prte_dvm_key_from_hex(hex, strlen(hex), back) && 0 == memcmp(key, back, sizeof(key)));
    for (i = 0; i < PRTE_DVM_KEY_HEXLEN; i++) {
        if ('a' <= hex[i] && 'f' >= hex[i]) {
            hex[i] = (char) (hex[i] - 'a' + 'A');
        }
    }
    CHECK("upper case is accepted",
          prte_dvm_key_from_hex(hex, strlen(hex), back) && 0 == memcmp(key, back, sizeof(key)));
    CHECK("one digit short is refused", !prte_dvm_key_from_hex(hex, strlen(hex) - 1, back));
    hex[10] = 'g';
    CHECK("a non-hex digit is refused", !prte_dvm_key_from_hex(hex, strlen(hex), back));
    CHECK("NULL is refused", !prte_dvm_key_from_hex(NULL, 0, back));
    return failures;
}

static int test_from_fd(void)
{
    int failures = 0, p[2];
    uint8_t want[PRTE_DVM_KEY_LEN];
    char line[PRTE_DVM_KEY_HEXLEN + 2];

    /* the line plm/ssh writes */
    CHECK("generate", PRTE_SUCCESS == prte_dvm_key_generate());
    memcpy(want, prte_dvm_key, sizeof(want));
    prte_dvm_key_to_hex(want, line);
    strcat(line, "\n");
    CHECK("pipe", 0 == pipe(p));
    CHECK("wrote", (ssize_t) strlen(line) == write(p[1], line, strlen(line)));
    close(p[1]);
    prte_dvm_key_clear();
    CHECK("a key on stdin is taken", PRTE_SUCCESS == prte_dvm_key_from_fd(p[0], 5));
    CHECK("and is the one sent",
          prte_dvm_key_ready && 0 == memcmp(want, prte_dvm_key, sizeof(want)));
    close(p[0]);

    fprintf(stdout, "-- the next cases refuse a key; the messages they print are expected --\n");
    /* an agent that forwards nothing */
    CHECK("pipe", 0 == pipe(p));
    close(p[1]);
    prte_dvm_key_clear();
    CHECK("an empty stdin is refused", PRTE_SUCCESS != prte_dvm_key_from_fd(p[0], 5));
    CHECK("and no key is held", !prte_dvm_key_ready);
    close(p[0]);

    /* a line that is not a key - a login script talking, say */
    CHECK("pipe", 0 == pipe(p));
    CHECK("wrote", 6 == write(p[1], "hello\n", 6));
    close(p[1]);
    CHECK("a line that is not a key is refused", PRTE_SUCCESS != prte_dvm_key_from_fd(p[0], 5));
    close(p[0]);

    /* one that never ends */
    CHECK("pipe", 0 == pipe(p));
    memset(line, 'a', sizeof(line));
    CHECK("wrote", (ssize_t) sizeof(line) == write(p[1], line, sizeof(line)));
    close(p[1]);
    CHECK("an overlong line is refused", PRTE_SUCCESS != prte_dvm_key_from_fd(p[0], 5));
    close(p[0]);

    /* and an agent that holds stdin open and sends nothing */
    CHECK("pipe", 0 == pipe(p));
    CHECK("silence is refused once the time is up",
          PRTE_SUCCESS != prte_dvm_key_from_fd(p[0], 1));
    close(p[0]);
    close(p[1]);
    return failures;
}

static int test_from_file(void)
{
    int failures = 0, fd;
    char path[] = "/tmp/prte_dvm_key_test_XXXXXX";
    char link[sizeof(path) + 8];
    char line[PRTE_DVM_KEY_HEXLEN + 2];
    uint8_t want[PRTE_DVM_KEY_LEN];

    CHECK("generate", PRTE_SUCCESS == prte_dvm_key_generate());
    memcpy(want, prte_dvm_key, sizeof(want));
    prte_dvm_key_to_hex(want, line);
    strcat(line, "\n");

    fd = mkstemp(path);
    CHECK("mkstemp", 0 <= fd);
    if (0 > fd) {
        return failures;
    }
    CHECK("wrote", (ssize_t) strlen(line) == write(fd, line, strlen(line)));
    close(fd);
    CHECK("0600", 0 == chmod(path, 0600));
    prte_dvm_key_clear();
    CHECK("a private key file is read", PRTE_SUCCESS == prte_dvm_key_from_file(path));
    CHECK("and holds the key written",
          prte_dvm_key_ready && 0 == memcmp(want, prte_dvm_key, sizeof(want)));

    CHECK("0644", 0 == chmod(path, 0644));
    prte_dvm_key_clear();
    CHECK("a key file others can read is refused", PRTE_SUCCESS != prte_dvm_key_from_file(path));
    CHECK("and no key is held", !prte_dvm_key_ready);
    CHECK("0620", 0 == chmod(path, 0620));
    CHECK("as is one others can write", PRTE_SUCCESS != prte_dvm_key_from_file(path));

    CHECK("0600", 0 == chmod(path, 0600));
    snprintf(link, sizeof(link), "%s.link", path);
    CHECK("symlink", 0 == symlink(path, link));
    CHECK("a symlink to a good key file is refused", PRTE_SUCCESS != prte_dvm_key_from_file(link));
    unlink(link);

    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK("reopen", 0 <= fd);
    if (0 <= fd) {
        CHECK("wrote", 9 == write(fd, "not-a-key", 9));
        close(fd);
    }
    CHECK("a file without a key is refused", PRTE_SUCCESS != prte_dvm_key_from_file(path));
    unlink(path);
    CHECK("a missing file is refused", PRTE_SUCCESS != prte_dvm_key_from_file(path));
    return failures;
}

static int test_from_env(void)
{
    int failures = 0;
    uint8_t want[PRTE_DVM_KEY_LEN];
    char hex[PRTE_DVM_KEY_HEXLEN + 1];
    char **env = NULL;
    int i;
    bool found;

    CHECK("generate", PRTE_SUCCESS == prte_dvm_key_generate());
    memcpy(want, prte_dvm_key, sizeof(want));

    /* the launcher's half: into an environment array, and out again */
    prte_dvm_key_setenv(&env);
    found = false;
    for (i = 0; NULL != env && NULL != env[i]; i++) {
        if (0 == strncmp(env[i], PRTE_DVM_KEY_ENVAR "=", strlen(PRTE_DVM_KEY_ENVAR) + 1)) {
            found = true;
            prte_dvm_key_to_hex(want, hex);
            CHECK("the array carries the key", 0 == strcmp(env[i] + strlen(PRTE_DVM_KEY_ENVAR) + 1, hex));
        }
    }
    CHECK("the key was added to the array", found);
    prte_dvm_key_scrub_array(env);
    for (i = 0; NULL != env && NULL != env[i]; i++) {
        CHECK("and wiped from it", 0 != strncmp(env[i], PRTE_DVM_KEY_ENVAR, strlen(PRTE_DVM_KEY_ENVAR)));
    }
    PMIx_Argv_free(env);

    /* the daemon's half */
    prte_dvm_key_to_hex(want, hex);
    setenv(PRTE_DVM_KEY_ENVAR, hex, 1);
    prte_dvm_key_clear();
    CHECK("a key in the environment is taken", PRTE_SUCCESS == prte_dvm_key_from_env());
    CHECK("and is the one set", prte_dvm_key_ready && 0 == memcmp(want, prte_dvm_key, sizeof(want)));
    CHECK("and is no longer in the environment", NULL == getenv(PRTE_DVM_KEY_ENVAR));

    setenv(PRTE_DVM_KEY_ENVAR, "abc", 1);
    prte_dvm_key_clear();
    CHECK("a malformed key is refused", PRTE_SUCCESS != prte_dvm_key_from_env());
    CHECK("but removed all the same", NULL == getenv(PRTE_DVM_KEY_ENVAR));
    CHECK("a missing key is refused", PRTE_SUCCESS != prte_dvm_key_from_env());
    return failures;
}

static int test_nonces(void)
{
    int failures = 0;
    uint8_t a[PRTE_DVM_KEY_LEN], b[PRTE_DVM_KEY_LEN];

    CHECK("nonce", PRTE_SUCCESS == prte_dvm_key_nonce(a));
    CHECK("nonce", PRTE_SUCCESS == prte_dvm_key_nonce(b));
    CHECK("no two nonces alike", 0 != memcmp(a, b, sizeof(a)));
    return failures;
}

int test_dvm_key(void)
{
    int failures = 0;

    failures += test_digests();
    failures += test_hex();
    failures += test_from_fd();
    failures += test_from_file();
    failures += test_from_env();
    failures += test_nonces();
    prte_dvm_key_clear();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_dvm_key\n");
    }
    return failures;
}
