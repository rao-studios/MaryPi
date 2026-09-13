#include "common/frame.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/secure.h"

void mc_frame_reader_init(mc_frame_reader *r, size_t max_payload, bool secure) {
    memset(r, 0, sizeof *r);
    r->max_payload = max_payload ? max_payload : MC_FRAME_MAX;
    r->secure = secure;
}

int mc_frame_reader_feed(mc_frame_reader *r, const unsigned char *bytes, size_t n, mc_frame_fn fn, void *user) {
    if (n && mc_buf_append(&r->buf, bytes, n) < 0) return -ENOMEM;
    size_t off = 0;
    int result = 0;
    while (r->buf.len - off >= MC_FRAME_HEADER) {
        const unsigned char *h = r->buf.data + off;
        uint32_t len = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        if (len > r->max_payload) {
            result = -EMSGSIZE;
            break;
        }
        if (r->buf.len - off < MC_FRAME_HEADER + (size_t)len) break;
        off += MC_FRAME_HEADER + len;
        if (fn(h[4], h + MC_FRAME_HEADER, len, user)) {
            result = 1;
            break;
        }
    }
    size_t before = r->buf.len;
    mc_buf_consume(&r->buf, off);
    if (r->secure && off && r->buf.data) mc_secure_zero(r->buf.data + r->buf.len, before - r->buf.len);
    return result;
}

int mc_frame_reader_read_fd(mc_frame_reader *r, int fd, mc_frame_fn fn, void *user) {
    unsigned char chunk[16384];
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got > 0) {
            int rc = mc_frame_reader_feed(r, chunk, (size_t)got, fn, user);
            if (r->secure) mc_secure_zero(chunk, (size_t)got);
            if (rc < 0) return rc;
            return rc == 1 ? MC_IO_STOPPED : MC_IO_OK;
        }
        if (got == 0) return MC_IO_EOF;
        if (errno == EINTR) continue;
        return -errno;
    }
}

void mc_frame_reader_free(mc_frame_reader *r) {
    if (r->secure) mc_buf_free_secure(&r->buf);
    else mc_buf_free(&r->buf);
}

static void header(unsigned char h[MC_FRAME_HEADER], uint8_t kind, size_t len) {
    h[0] = len & 0xff;
    h[1] = len >> 8 & 0xff;
    h[2] = len >> 16 & 0xff;
    h[3] = len >> 24 & 0xff;
    h[4] = kind;
}

int mc_frame_append(mc_buf *out, uint8_t kind, const unsigned char *payload, size_t len) {
    if (len > MC_FRAME_MAX) return -EMSGSIZE;
    unsigned char h[MC_FRAME_HEADER];
    header(h, kind, len);
    int rc = mc_buf_append(out, h, sizeof h);
    return rc ? rc : mc_buf_append(out, payload, len);
}

int mc_frame_write_fd(int fd, uint8_t kind, const unsigned char *payload, size_t len) {
    if (len > MC_FRAME_MAX) return -EMSGSIZE;
    unsigned char h[MC_FRAME_HEADER];
    header(h, kind, len);
    int rc = mc_write_all(fd, h, sizeof h);
    return rc || !len ? rc : mc_write_all(fd, payload, len);
}

int mc_frame_write_json(int fd, struct json_object *obj) {
    size_t len = 0;
    const char *text = mc_json_compact(obj, &len);
    if (!text) return -ENOMEM;
    return mc_frame_write_fd(fd, MC_FRAME_JSON, (const unsigned char *)text, len);
}
