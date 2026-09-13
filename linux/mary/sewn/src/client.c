#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/client.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "sewn/key.h"

const char *sewn_default_socket(void) {
    const char *path = getenv("SEWN_SOCKET");
    return path && *path ? path : SEWN_SOCKET_PATH;
}

int sewn_connect(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof addr.sun_path) return -ENAMETOOLONG;
    memcpy(addr.sun_path, path, n + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    mc_set_cloexec(fd);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

struct reply_ctx {
    struct json_object **reply;
    int error;
};

static int take_json(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct reply_ctx *ctx = user;
    if (kind != MC_FRAME_JSON) return 0;
    *ctx->reply = mc_json_parse((const char *)bytes, len);
    if (!*ctx->reply) ctx->error = -EBADMSG;
    return 1;
}

int sewn_read_reply(int fd, struct json_object **reply) {
    *reply = NULL;
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    struct reply_ctx ctx = { reply, 0 };
    int status;
    while ((status = mc_frame_reader_read_fd(&reader, fd, take_json, &ctx)) == MC_IO_OK) {}
    mc_frame_reader_free(&reader);
    if (status == MC_IO_STOPPED) return ctx.error;
    return status < 0 ? status : -ECONNRESET;
}

int sewn_call(int fd, struct json_object *request, struct json_object **reply) {
    int rc = mc_frame_write_json(fd, request);
    return rc ? rc : sewn_read_reply(fd, reply);
}

int sewn_call_key_set(int fd, const char *key, size_t len, struct json_object **reply) {
    static const char head[] = "{\"type\":\"key.set\",\"key\":\"", tail[] = "\"}";
    if (!sewn_key_valid(key, len)) return -EINVAL;
    mc_buf frame = { 0 };
    /* Reserved up front: a realloc would leave a copy of the key behind. */
    int rc = mc_buf_reserve(&frame, sizeof head + len + sizeof tail);
    if (rc == 0) rc = mc_buf_append(&frame, head, sizeof head - 1);
    if (rc == 0) rc = mc_buf_append(&frame, key, len);
    if (rc == 0) rc = mc_buf_append(&frame, tail, sizeof tail - 1);
    if (rc == 0) rc = mc_frame_write_fd(fd, MC_FRAME_JSON, frame.data, frame.len);
    mc_buf_free_secure(&frame);
    return rc ? rc : sewn_read_reply(fd, reply);
}
