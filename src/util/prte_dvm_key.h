/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The DVM key: a value shared by every daemon of one DVM and by nothing
 * else.
 *
 * A daemon's OOB port accepts a TCP connection from whatever reaches it, and
 * a connected peer is trusted with everything the DVM does - launch commands
 * included.  So a peer has to show it belongs to this DVM before it is
 * treated as a daemon, and it does so by showing it holds this key, through a
 * challenge and response in the connect handshake (src/rml/oob).  The key
 * itself never crosses the OOB.
 *
 * Nothing already used to identify a DVM can serve: the namespace and the
 * HNP's contact URI are on every prted's command line, where anything on the
 * node can see them.  The key therefore travels only by channels that are
 * private to the DVM's own processes:
 *
 *  - the HNP generates it (prte_dvm_key_generate);
 *  - plm/ssh writes it down the stdin of each ssh it starts, and the prted at
 *    the other end reads it from its own stdin (prte_dvm_key_from_fd), which
 *    ssh carries over its encrypted channel.  Nothing that crosses a command
 *    line can carry it - the remote command is the ssh process's argv on
 *    this node and the shell's on the other;
 *  - plm/slurm, plm/lsf and plm/pals place it in the environment of the
 *    launcher they start, which the resource manager hands to every prted it
 *    spawns (prte_dvm_key_from_env).  A process's environment is visible
 *    only to its own user and root;
 *  - a bootstrapped DVM has no launcher, so each daemon reads it from the
 *    file prte.conf names in DVMKeyFile, which must be owned by the daemon's
 *    user and accessible to nobody else (prte_dvm_key_from_file).
 *
 * Whatever delivered it, a prted removes the key from its environment as
 * soon as it has it, so no process the daemon starts inherits it.
 */

#ifndef PRTE_UTIL_DVM_KEY_H
#define PRTE_UTIL_DVM_KEY_H

#include "prte_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

BEGIN_C_DECLS

/* 256 bits of key; it travels as twice that many hex digits */
#define PRTE_DVM_KEY_LEN     32
#define PRTE_DVM_KEY_HEXLEN  (2 * PRTE_DVM_KEY_LEN)

/* the environment variable a resource-manager launch carries it in.  It is
 * deliberately NOT a PRTE_MCA_ name: those are copied onto every prted's
 * command line (prte_plm_base_prted_append_basic_args), which is exactly
 * where the key must never appear. */
#define PRTE_DVM_KEY_ENVAR   "PRTE_DVM_KEY"

/* the key, once one of the functions below has established it */
PRTE_EXPORT extern uint8_t prte_dvm_key[PRTE_DVM_KEY_LEN];
PRTE_EXPORT extern bool prte_dvm_key_ready;

/* Fill `buf` from the operating system's random source - for values that
 * must not be guessable: key and nonce material, and names that must not
 * collide (filem's temporaries). */
PRTE_EXPORT int prte_dvm_key_random(uint8_t *buf, size_t len);

/* The HNP's key: freshly generated, never stored anywhere but memory. */
PRTE_EXPORT int prte_dvm_key_generate(void);

/* A daemon's key, as its launcher delivered it.  Each fails - with a
 * show_help explaining what was expected - if the key is absent or
 * malformed; a daemon that cannot prove it belongs to the DVM cannot join
 * it, and must say why rather than be refused by every peer it dials. */
PRTE_EXPORT int prte_dvm_key_from_env(void);
PRTE_EXPORT int prte_dvm_key_from_fd(int fd, int timeout_secs);
PRTE_EXPORT int prte_dvm_key_from_file(const char *path);

/* Encode/decode the key for transport.  The encoding writes exactly
 * PRTE_DVM_KEY_HEXLEN digits plus a terminator; the decoding accepts exactly
 * that many hex digits and nothing else. */
PRTE_EXPORT void prte_dvm_key_to_hex(const uint8_t key[PRTE_DVM_KEY_LEN],
                                     char hex[PRTE_DVM_KEY_HEXLEN + 1]);
PRTE_EXPORT bool prte_dvm_key_from_hex(const char *hex, size_t len,
                                       uint8_t key[PRTE_DVM_KEY_LEN]);

/* A fresh nonce for one connect handshake: not guessable,
 * and never repeated within this process. */
PRTE_EXPORT int prte_dvm_key_nonce(uint8_t nonce[PRTE_DVM_KEY_LEN]);

/* How long a daemon waits for its key on stdin.  plm/ssh writes it before
 * the ssh it starts can even connect, so this is only ever reached when the
 * launch agent does not forward stdin at all. */
#define PRTE_DVM_KEY_STDIN_TIMEOUT 60

/* Remove the key from this process's environment, wiping the string first,
 * so that nothing it starts inherits it.  Harmless if it is not there. */
PRTE_EXPORT void prte_dvm_key_scrub_env(void);

/* Add the key to an environment array being built for a launcher, and wipe
 * it out of one before the array is freed. */
PRTE_EXPORT void prte_dvm_key_setenv(char ***env);
PRTE_EXPORT void prte_dvm_key_scrub_array(char **env);

/* Forget the key (finalize, and between unit tests). */
PRTE_EXPORT void prte_dvm_key_clear(void);

END_C_DECLS

#endif /* PRTE_UTIL_DVM_KEY_H */
