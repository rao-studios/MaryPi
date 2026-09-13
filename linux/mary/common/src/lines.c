#include "common/lines.h"

#include <errno.h>
#include <string.h>

void mc_line_reader_init(mc_line_reader *r, size_t max_line) {
    memset(r, 0, sizeof *r);
    r->max_line = max_line;
}

int mc_line_reader_feed(mc_line_reader *r, const char *bytes, size_t n, mc_line_fn fn, void *user) {
    int result = 0;
    size_t i = 0;
    while (i < n) {
        const char *nl = memchr(bytes + i, '\n', n - i);
        size_t chunk = nl ? (size_t)(nl - (bytes + i)) : n - i;
        if (r->discarding) {
            if (!nl) return result;
            r->discarding = false;
            i += chunk + 1;
            continue;
        }
        if (r->buf.len + chunk > r->max_line) {
            mc_buf_clear(&r->buf);
            result = -EMSGSIZE;
            if (!nl) {
                r->discarding = true;
                return result;
            }
            i += chunk + 1;
            continue;
        }
        if (mc_buf_append(&r->buf, bytes + i, chunk) < 0) return -ENOMEM;
        i += chunk;
        if (!nl) break;
        i++;
        size_t len = r->buf.len;
        if (len && r->buf.data[len - 1] == '\r') r->buf.data[--len] = 0;
        int stop = fn((const char *)r->buf.data, len, user);
        mc_buf_clear(&r->buf);
        if (stop) return 1;
    }
    return result;
}

void mc_line_reader_free(mc_line_reader *r) { mc_buf_free(&r->buf); }
