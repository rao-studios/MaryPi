#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "index/record.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/io.h"
#include "common/json.h"
#include "common/sha256.h"
#include "index/kind.h"
#include "index/walk.h"

int ix_hash_file(const char *path, char out[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    mc_sha256_ctx ctx;
    mc_sha256_init(&ctx);
    unsigned char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) mc_sha256_update(&ctx, buf, (size_t)n);
    int err = n < 0 ? errno : 0;
    close(fd);
    if (err) return -err;
    unsigned char digest[32];
    mc_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
    return 0;
}

const char *ix_relative(const char *root, const char *path) {
    size_t rl = strlen(root);
    if (strncmp(path, root, rl) == 0 && path[rl] == '/') return path + rl + 1;
    return path;
}

static struct json_object *entity(const char *name, const char *kind) {
    struct json_object *e = json_object_new_object();
    json_object_object_add(e, "name", json_object_new_string(name));
    json_object_object_add(e, "kind", json_object_new_string(kind));
    return e;
}

static struct json_object *relation(const char *subject, const char *predicate, const char *object) {
    struct json_object *r = json_object_new_object();
    json_object_object_add(r, "subject", json_object_new_string(subject));
    json_object_object_add(r, "predicate", json_object_new_string(predicate));
    json_object_object_add(r, "object", json_object_new_string(object));
    return r;
}

/* "12.3 KB" the way Finder says it. */
static void size_text(int64_t size, char *out, size_t cap) {
    if (size < 1000) snprintf(out, cap, "%lld bytes", (long long)size);
    else if (size < 1000000) snprintf(out, cap, "%.1f KB", size / 1000.0);
    else if (size < 1000000000) snprintf(out, cap, "%.1f MB", size / 1000000.0);
    else snprintf(out, cap, "%.2f GB", size / 1000000000.0);
}

struct json_object *ix_parity_entry(const char *path, int64_t size, int64_t mtime_ms, const char *hash) {
    struct json_object *e = json_object_new_object();
    json_object_object_add(e, "path", json_object_new_string(path));
    json_object_object_add(e, "size", json_object_new_int64(size));
    json_object_object_add(e, "mtime_ms", json_object_new_int64(mtime_ms));
    if (hash) json_object_object_add(e, "hash", json_object_new_string(hash));
    return e;
}

struct json_object *ix_record(const char *root, const char *owner, const char *path, int *error) {
    struct stat st;
    if (stat(path, &st) != 0) {
        *error = -errno;
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        *error = -EINVAL;
        return NULL;
    }
    if (st.st_size > (off_t)IX_FILE_MAX) {
        *error = -EFBIG;
        return NULL;
    }
#if defined(__APPLE__)
    int64_t mtime_ms = (int64_t)st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
    int64_t mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
    char hash[65];
    int rc = ix_hash_file(path, hash);
    if (rc) {
        *error = rc;
        return NULL;
    }
    const char *kind = ix_kind_of(path);
    bool text = strcmp(kind, "text") == 0 || strcmp(kind, "code") == 0;
    const char *name = ix_basename(path), *relative = ix_relative(root, path);
    struct json_object *req = json_object_new_object();
    json_object_object_add(req, "type", json_object_new_string("deposit"));
    json_object_object_add(req, "source", json_object_new_string("indexd"));
    char group[128];
    snprintf(group, sizeof group, "%s%s", IX_GROUP_PREFIX, owner);
    json_object_object_add(req, "group", json_object_new_string(group));
    json_object_object_add(req, "label", json_object_new_string(IX_GROUP_LABEL));
    json_object_object_add(req, "family", json_object_new_string("file"));
    json_object_object_add(req, "name", json_object_new_string(name));
    if (text && st.st_size > 0) {
        mc_buf body = { 0 };
        rc = mc_read_file(path, &body, IX_FILE_MAX);
        if (rc) {
            json_object_put(req);
            *error = rc;
            return NULL;
        }
        /* NUL bytes would end the text early; a file that has them is not text after all */
        if (memchr(body.data, 0, body.len)) text = false;
        else {
            json_object_object_add(req, "text", json_object_new_string_len((const char *)body.data, (int)body.len));
            json_object_object_add(req, "chunk", json_object_new_boolean(true));
        }
        mc_buf_free(&body);
    }
    if (!text || st.st_size == 0) {
        char size[32], when[32], line[512];
        size_text((int64_t)st.st_size, size, sizeof size);
        time_t t = (time_t)(mtime_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        snprintf(line, sizeof line, "%s \xE2\x80\x94 %s, %s, %s", name, kind, size, when);
        json_object_object_add(req, "text", json_object_new_string(line));
        json_object_object_add(req, "media_type", json_object_new_string(strcmp(kind, "image") == 0 ? "image" : "text"));
    }
    struct json_object *metadata = json_object_new_object();
    json_object_object_add(metadata, "family", json_object_new_string("file"));
    json_object_object_add(metadata, "path", json_object_new_string(path));
    json_object_object_add(metadata, "size", json_object_new_int64((int64_t)st.st_size));
    json_object_object_add(metadata, "mtime_ms", json_object_new_int64(mtime_ms));
    json_object_object_add(metadata, "hash", json_object_new_string(hash));
    json_object_object_add(metadata, "kind", json_object_new_string(kind));
    json_object_object_add(req, "metadata", metadata);
    /* the graph: the file, its folders up to the home, and `in` between them */
    struct json_object *entities = json_object_new_array(), *relationships = json_object_new_array();
    json_object_array_add(entities, entity(relative, "file"));
    char folder[4096];
    snprintf(folder, sizeof folder, "%s", relative);
    const char *child = relative;
    char child_copy[4096];
    snprintf(child_copy, sizeof child_copy, "%s", relative);
    for (;;) {
        char *slash = strrchr(folder, '/');
        const char *parent = slash ? (*slash = 0, folder) : "~";
        json_object_array_add(entities, entity(parent, "folder"));
        json_object_array_add(relationships, relation(child_copy, "in", parent));
        if (!slash) break;
        snprintf(child_copy, sizeof child_copy, "%s", parent);
        child = parent;
    }
    (void)child;
    json_object_object_add(req, "entities", entities);
    json_object_object_add(req, "relationships", relationships);
    struct json_object *file = json_object_new_object();
    json_object_object_add(file, "path", json_object_new_string(path));
    json_object_object_add(file, "kind", json_object_new_string(kind));
    json_object_object_add(file, "size", json_object_new_int64((int64_t)st.st_size));
    json_object_object_add(file, "mtime_ms", json_object_new_int64(mtime_ms));
    json_object_object_add(file, "hash", json_object_new_string(hash));
    json_object_object_add(file, "text_indexed", json_object_new_boolean(text));
    json_object_object_add(req, "file", file);
    *error = 0;
    return req;
}
