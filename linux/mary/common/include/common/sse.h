/* Server-Sent Events, parsed as the bytes arrive (the WHATWG event-stream
 * format): `data:` lines accumulate, `event:` names the event, lines starting
 * with ':' are comments, and a blank line dispatches. Mistral's chat stream is
 * `data: {json}` events ending with `data: [DONE]`; its speech stream names its
 * events. A bare "\r" is not a line end — no server Mary talks to sends one. */
#ifndef MARY_COMMON_SSE_H
#define MARY_COMMON_SSE_H

#include <stdbool.h>
#include <stddef.h>

#include "common/buf.h"
#include "common/lines.h"

/* `event` is "" when the event had no `event:` field; `data` is its data lines
 * joined with "\n", NUL-terminated. Both are valid only during the call. Return
 * nonzero to stop. */
typedef int (*mc_sse_fn)(const char *event, const char *data, size_t len, void *user);

typedef struct mc_sse_parser {
    mc_line_reader lines;
    mc_buf data;
    char event[64];
    bool have_data;
} mc_sse_parser;

/* `max_line` also caps one event's accumulated data. */
void mc_sse_init(mc_sse_parser *p, size_t max_line);
/* 0; 1 when fn stopped; -EMSGSIZE for an oversize line or event; -ENOMEM. */
int mc_sse_feed(mc_sse_parser *p, const char *bytes, size_t n, mc_sse_fn fn, void *user);
/* At the end of the stream: dispatches an event the stream ended without a
 * blank line after. 0, 1 when fn stopped, or -errno. */
int mc_sse_finish(mc_sse_parser *p, mc_sse_fn fn, void *user);
void mc_sse_free(mc_sse_parser *p);

#endif
