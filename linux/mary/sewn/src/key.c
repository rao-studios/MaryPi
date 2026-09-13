#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/key.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/secure.h"

#define KEY_FILE "mistral.key"
#define VERIFIED_FILE "mistral.verified"

void sewn_key_store_init(sewn_key_store *s, const char *state_dir) {
    snprintf(s->dir, sizeof s->dir, "%s", state_dir && *state_dir ? state_dir : SEWN_STATE_DIR);
}

bool sewn_key_valid(const char *key, size_t len) {
    if (!key || len < SEWN_KEY_MIN || len > SEWN_KEY_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

static int path_of(const sewn_key_store *s, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s", s->dir, name);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

static int write_atomically(const sewn_key_store *s, const char *name, const char *bytes, size_t len) {
    char final[512], tmp[520];
    int rc = path_of(s, name, final, sizeof final);
    if (rc) return rc;
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", final);
    int fd = mkstemp(tmp);   /* created 0600 */
    if (fd < 0) return -errno;
    rc = fchmod(fd, 0600) < 0 ? -errno : 0;
    if (rc == 0) rc = mc_write_all(fd, bytes, len);
    if (rc == 0 && fsync(fd) < 0) rc = -errno;
    if (close(fd) < 0 && rc == 0) rc = -errno;
    if (rc == 0 && rename(tmp, final) < 0) rc = -errno;
    if (rc) {
        unlink(tmp);
        return rc;
    }
    int dir = open(s->dir, O_RDONLY);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
    return 0;
}

int sewn_key_store_set(const sewn_key_store *s, const char *key, size_t len) {
    if (!sewn_key_valid(key, len)) return -EINVAL;
    int rc = write_atomically(s, KEY_FILE, key, len);
    if (rc == 0) {
        char verified[512];
        if (path_of(s, VERIFIED_FILE, verified, sizeof verified) == 0) unlink(verified);
    }
    return rc;
}

/* Reads a private regular file of this user into out[0..cap-1], NUL-terminated. */
static ssize_t read_private(const sewn_key_store *s, const char *name, char *out, size_t cap) {
    char path[512];
    int rc = path_of(s, name, path, sizeof path);
    if (rc) return rc;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (st.st_mode & 077) || st.st_uid != geteuid()) {
        close(fd);
        return -EPERM;
    }
    size_t got = 0;
    ssize_t result = 0;
    while (got + 1 < cap) {
        ssize_t r = read(fd, out + got, cap - 1 - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            result = -errno;
            break;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    char extra;
    if (result == 0 && got + 1 >= cap && read(fd, &extra, 1) == 1) result = -EFBIG;
    close(fd);
    out[got] = 0;
    return result < 0 ? result : (ssize_t)got;
}

int sewn_key_store_get(const sewn_key_store *s, char *out, size_t cap) {
    if (!out || cap == 0) return -EINVAL;
    ssize_t got = read_private(s, KEY_FILE, out, cap);
    int rc = got < 0 ? (int)got : sewn_key_valid(out, (size_t)got) ? 0 : -EINVAL;
    if (rc) mc_secure_zero(out, cap);
    return rc;
}

bool sewn_key_store_present(const sewn_key_store *s) {
    char path[512];
    struct stat st;
    return path_of(s, KEY_FILE, path, sizeof path) == 0 && lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int64_t sewn_key_store_verified_at(const sewn_key_store *s) {
    char text[32];
    if (read_private(s, VERIFIED_FILE, text, sizeof text) <= 0) return 0;
    long long at = strtoll(text, NULL, 10);
    return at > 0 ? (int64_t)at : 0;
}

int sewn_key_store_mark_verified(const sewn_key_store *s, int64_t at_ms) {
    char text[32];
    int n = snprintf(text, sizeof text, "%lld\n", (long long)at_ms);
    return write_atomically(s, VERIFIED_FILE, text, (size_t)n);
}
