/* Where sewnd keeps the Mistral API key: one file, mode 0600, owned by the `sewn`
 * user, under a 0700 state directory. It is written atomically (a private
 * temporary in the same directory, fsync, rename) and never read if anyone but
 * its owner could read it. Sewn reads MISTRAL_API_KEY from a .env file instead
 * (API/Network/NetworkService+Center.swift); on MaryOS the key arrives from
 * System Settings and never touches the environment. */
#ifndef MARY_SEWN_KEY_H
#define MARY_SEWN_KEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEWN_STATE_DIR "/var/lib/sewn"
#define SEWN_KEY_MIN 16
#define SEWN_KEY_MAX 256

typedef struct sewn_key_store {
    char dir[256];
} sewn_key_store;

void sewn_key_store_init(sewn_key_store *s, const char *state_dir);

/* A plausible key: 16…256 printable ASCII bytes, no spaces, quotes or backslashes
 * (so it needs no escaping in JSON and cannot break a header line). */
bool sewn_key_valid(const char *key, size_t len);

/* 0, -EINVAL for an implausible key (the stored one is kept), or -errno. A new
 * key forgets when the old one was last verified. */
int sewn_key_store_set(const sewn_key_store *s, const char *key, size_t len);
/* The key into `out` (NUL-terminated). 0, -ENOENT when none is stored, -EPERM
 * when the file is not a private regular file of this user, or -errno. On any
 * failure `out` is zeroed. The caller zeroes it after use. */
int sewn_key_store_get(const sewn_key_store *s, char *out, size_t cap);
bool sewn_key_store_present(const sewn_key_store *s);

/* When Mistral last accepted the stored key, in ms since the epoch; 0: never. */
int64_t sewn_key_store_verified_at(const sewn_key_store *s);
int sewn_key_store_mark_verified(const sewn_key_store *s, int64_t at_ms);

#endif
