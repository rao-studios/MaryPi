#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/secure.h"
#include "sewn/http.h"
#include "sewn/transcribe.h"
#include "sewn/turn.h"
#include "sewn/ws.h"

void sewn_service_init(sewn_service *svc, const char *state_dir) {
    memset(svc, 0, sizeof *svc);
    sewn_key_store_init(&svc->keys, state_dir);
    svc->admin_group = SEWN_ADMIN_GROUP;
    svc->in_group = sewn_uid_in_group;
#ifdef HAVE_CURL
    svc->verify = sewn_mistral_verify;
    svc->post_stream = sewn_http_post_stream;
#endif
#ifdef HAVE_LWS
    svc->ws = &sewn_lws_ops;
#endif
}

static int send_error(int fd, const char *stage, const char *message) {
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("error"));
    json_object_object_add(reply, "stage", json_object_new_string(stage));
    json_object_object_add(reply, "message", json_object_new_string(message));
    int rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

/* ok < 0: no verdict to report. */
static int send_status(int fd, const sewn_service *svc, int ok, const char *message) {
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("key.status"));
    json_object_object_add(reply, "present", json_object_new_boolean(sewn_key_store_present(&svc->keys)));
    int64_t at = sewn_key_store_verified_at(&svc->keys);
    json_object_object_add(reply, "verified_at", at > 0 ? json_object_new_int64(at) : NULL);
    if (ok >= 0) json_object_object_add(reply, "ok", json_object_new_boolean(ok));
    if (message) json_object_object_add(reply, "message", json_object_new_string(message));
    int rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static int key_set(sewn_service *svc, int fd, struct json_object *request) {
    struct json_object *value;
    if (!json_object_object_get_ex(request, "key", &value) || !json_object_is_type(value, json_type_string))
        return send_error(fd, "key", "key.set needs a key");
    char *key = (char *)json_object_get_string(value);
    size_t stored = (size_t)json_object_get_string_len(value), len = stored;
    while (len && (key[len - 1] == '\n' || key[len - 1] == '\r')) len--;
    int rc = sewn_key_store_set(&svc->keys, key, len);
    mc_secure_zero(key, stored);
    if (rc == -EINVAL) return send_error(fd, "key", "that is not a Mistral API key");
    if (rc < 0) {
        mc_log(MC_LOG_ERROR, "the key could not be stored in %s: %s", svc->keys.dir, strerror(-rc));
        return send_error(fd, "key", "the key could not be stored");
    }
    mc_log(MC_LOG_NOTICE, "a new key was stored");
    return send_status(fd, svc, -1, NULL);
}

static int key_verify(sewn_service *svc, int fd) {
    char key[SEWN_KEY_MAX + 1], message[256] = "";
    int rc = sewn_key_store_get(&svc->keys, key, sizeof key);
    if (rc == -ENOENT) return send_error(fd, "key", "no key is stored");
    if (rc < 0) {
        mc_log(MC_LOG_ERROR, "the stored key could not be read: %s", strerror(-rc));
        return send_error(fd, "key", "the stored key could not be read");
    }
    if (!svc->verify) {
        mc_secure_zero(key, sizeof key);
        return send_error(fd, "network", "sewnd was built without libcurl");
    }
    int verdict = svc->verify(key, message, sizeof message, svc->verify_user);
    mc_secure_zero(key, sizeof key);
    if (verdict < 0) return send_error(fd, "network", message[0] ? message : "Mistral could not be reached");
    if (verdict > 0) sewn_key_store_mark_verified(&svc->keys, mc_wall_ms());
    mc_log(MC_LOG_NOTICE, "Mistral %s the key", verdict > 0 ? "accepted" : "refused");
    return send_status(fd, svc, verdict > 0, message[0] ? message : NULL);
}

struct first_frame {
    mc_buf *payload;
    int kind;
};

static int take_first(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct first_frame *first = user;
    first->kind = mc_buf_append(first->payload, bytes, len) < 0 ? -ENOMEM : kind;
    return 1;
}

/* An operation name fit for a log line: short, printable, or a stand-in. */
static const char *loggable(const char *type) {
    size_t n = strlen(type);
    if (n == 0 || n > 32) return "(unnamed)";
    for (size_t i = 0; i < n; i++) if (type[i] < 0x21 || type[i] > 0x7E) return "(unnamed)";
    return type;
}

static int dispatch(sewn_service *svc, int fd, const sewn_peer *peer, mc_frame_reader *reader, struct json_object *request) {
    const char *type = mc_json_type(request);
    if (!type) return send_error(fd, "request", "the first frame must be a JSON message with a type");
    if (!sewn_peer_may(peer, type, svc->admin_group, svc->in_group)) {
        mc_log(MC_LOG_WARNING, "refused %s from uid %u", loggable(type), (unsigned)peer->uid);
        return send_error(fd, "auth", "this user may not do that");
    }
    mc_log(MC_LOG_INFO, "%s from uid %u", loggable(type), (unsigned)peer->uid);
    if (strcmp(type, "key.status") == 0) return send_status(fd, svc, -1, NULL);
    if (strcmp(type, "key.set") == 0) return key_set(svc, fd, request);
    if (strcmp(type, "key.verify") == 0) return key_verify(svc, fd);
    if (strcmp(type, "turn.start") == 0) return sewn_run_turn(svc, fd, reader, request);
    if (strcmp(type, "transcribe.start") == 0) return sewn_run_transcribe(svc, fd, reader, request);
    return send_error(fd, "request", "unknown operation");
}

int sewn_serve_connection(sewn_service *svc, int fd, const sewn_peer *peer) {
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, true);
    mc_buf payload = { 0 };
    struct first_frame first = { &payload, 0 };
    int status, rc;
    while ((status = mc_frame_reader_read_fd(&reader, fd, take_first, &first)) == MC_IO_OK) {}
    if (status != MC_IO_STOPPED) {
        rc = status < 0 ? status : -ECONNRESET;
    } else if (first.kind < 0) {
        rc = first.kind;
    } else if (first.kind != MC_FRAME_JSON) {
        rc = send_error(fd, "request", "the first frame must be JSON");
    } else {
        struct json_object *request = mc_json_parse_secret((const char *)payload.data, payload.len);
        mc_secure_zero(payload.data, payload.len);
        rc = dispatch(svc, fd, peer, &reader, request);
        json_object_put(request);
    }
    mc_buf_free_secure(&payload);
    mc_frame_reader_free(&reader);
    return rc;
}

int sewn_listen(const char *path, int mode) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof addr.sun_path) return -ENAMETOOLONG;
    memcpy(addr.sun_path, path, n + 1);
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) return -EEXIST;
        unlink(path);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    mc_set_cloexec(fd);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || chmod(path, (mode_t)mode) < 0 || listen(fd, 16) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}
