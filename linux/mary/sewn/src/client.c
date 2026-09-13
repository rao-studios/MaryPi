#include "sewn/client.h"

#include <errno.h>

#include "common/buf.h"
#include "common/frame.h"
#include "common/service.h"
#include "sewn/key.h"

const char *sewn_default_socket(void) { return mc_service_default_socket("SEWN_SOCKET", SEWN_SOCKET_PATH); }

int sewn_connect(const char *path) { return mc_service_connect(path); }

int sewn_read_reply(int fd, struct json_object **reply) { return mc_service_read_reply(fd, reply); }

int sewn_call(int fd, struct json_object *request, struct json_object **reply) { return mc_service_call(fd, request, reply); }

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
    return rc ? rc : mc_service_read_reply(fd, reply);
}
