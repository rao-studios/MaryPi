/* Newline-delimited reading from a stream that arrives in arbitrary pieces.
 * A line ends at "\n"; a "\r" before it is dropped. Lines longer than
 * `max_line` bytes are discarded whole and reported, and reading carries on
 * after them. */
#ifndef MARY_COMMON_LINES_H
#define MARY_COMMON_LINES_H

#include <stdbool.h>
#include <stddef.h>

#include "common/buf.h"

/* `line` is NUL-terminated and valid only during the call. Return nonzero to
 * stop: the bytes after that line are dropped. */
typedef int (*mc_line_fn)(const char *line, size_t len, void *user);

typedef struct mc_line_reader {
    mc_buf buf;
    size_t max_line;
    bool discarding;        /* inside a line that passed max_line */
} mc_line_reader;

void mc_line_reader_init(mc_line_reader *r, size_t max_line);
/* 0; 1 when fn stopped; -EMSGSIZE when a line passed max_line (the lines around
 * it are still delivered); -ENOMEM. */
int mc_line_reader_feed(mc_line_reader *r, const char *bytes, size_t n, mc_line_fn fn, void *user);
void mc_line_reader_free(mc_line_reader *r);

#endif
