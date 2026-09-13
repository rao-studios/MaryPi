#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "common/service.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"

const char *mc_service_default_socket(const char *env, const char *fallback) {
    const char *path = env ? getenv(env) : NULL;
    return path && *path ? path : fallback;
}

int mc_service_connect(const char *path) {
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

int mc_service_read_reply(int fd, struct json_object **reply) {
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

int mc_service_call(int fd, struct json_object *request, struct json_object **reply) {
    int rc = mc_frame_write_json(fd, request);
    return rc ? rc : mc_service_read_reply(fd, reply);
}
