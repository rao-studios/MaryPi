#include "common/sse.h"

#include <errno.h>
#include <string.h>

struct feed {
    mc_sse_parser *p;
    mc_sse_fn fn;
    void *user;
    int stopped;
    int error;
};

void mc_sse_init(mc_sse_parser *p, size_t max_line) {
    memset(p, 0, sizeof *p);
    mc_line_reader_init(&p->lines, max_line);
}

static int dispatch(struct feed *f) {
    mc_sse_parser *p = f->p;
    if (!p->have_data) {
        p->event[0] = 0;
        return 0;
    }
    size_t len = p->data.len;
    if (len && p->data.data[len - 1] == '\n') p->data.data[--len] = 0;
    int stop = f->fn(p->event, (const char *)p->data.data, len, f->user);
    mc_buf_clear(&p->data);
    p->have_data = false;
    p->event[0] = 0;
    if (stop) f->stopped = 1;
    return stop;
}

static int on_line(const char *line, size_t len, void *user) {
    struct feed *f = user;
    mc_sse_parser *p = f->p;
    if (len == 0) return dispatch(f);
    if (line[0] == ':') return 0;
    const char *colon = memchr(line, ':', len);
    size_t name_len = colon ? (size_t)(colon - line) : len;
    const char *value = colon ? colon + 1 : line + len;
    size_t value_len = colon ? len - name_len - 1 : 0;
    if (value_len && value[0] == ' ') {
        value++;
        value_len--;
    }
    if (name_len == 4 && memcmp(line, "data", 4) == 0) {
        if (p->data.len + value_len + 1 > p->lines.max_line) {
            f->error = -EMSGSIZE;
            return 1;
        }
        if (mc_buf_append(&p->data, value, value_len) < 0 || mc_buf_append(&p->data, "\n", 1) < 0) {
            f->error = -ENOMEM;
            return 1;
        }
        p->have_data = true;
    } else if (name_len == 5 && memcmp(line, "event", 5) == 0) {
        size_t n = value_len < sizeof p->event - 1 ? value_len : sizeof p->event - 1;
        memcpy(p->event, value, n);
        p->event[n] = 0;
    }
    return 0;
}

int mc_sse_feed(mc_sse_parser *p, const char *bytes, size_t n, mc_sse_fn fn, void *user) {
    struct feed f = { .p = p, .fn = fn, .user = user };
    int rc = mc_line_reader_feed(&p->lines, bytes, n, on_line, &f);
    if (f.error) return f.error;
    if (f.stopped) return 1;
    return rc < 0 ? rc : 0;
}

int mc_sse_finish(mc_sse_parser *p, mc_sse_fn fn, void *user) {
    struct feed f = { .p = p, .fn = fn, .user = user };
    if (p->lines.buf.len && !p->lines.discarding) {
        size_t len = p->lines.buf.len;
        if (p->lines.buf.data[len - 1] == '\r') p->lines.buf.data[--len] = 0;
        on_line((const char *)p->lines.buf.data, len, &f);
    }
    mc_buf_clear(&p->lines.buf);
    p->lines.discarding = false;
    if (f.error) return f.error;
    dispatch(&f);
    return f.stopped;
}

void mc_sse_free(mc_sse_parser *p) {
    mc_line_reader_free(&p->lines);
    mc_buf_free(&p->data);
}
