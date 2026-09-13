/* File descriptors and clocks. Daemons ignore SIGPIPE (mc_ignore_sigpipe) so a
 * peer that hangs up surfaces as -EPIPE, never a signal. */
#ifndef MARY_COMMON_IO_H
#define MARY_COMMON_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "common/buf.h"

enum mc_io_status {
    MC_IO_EOF = 0,
    MC_IO_OK = 1,
    MC_IO_STOPPED = 2,      /* a callback asked to stop */
};

/* Writes all n bytes to a blocking fd, retrying EINTR. 0, or -errno. */
int mc_write_all(int fd, const void *buf, size_t n);
int mc_set_nonblocking(int fd, bool on);
int mc_set_cloexec(int fd);
void mc_ignore_sigpipe(void);

/* Appends a whole file to `out`. 0, -EFBIG when it is longer than `max`, or -errno. */
int mc_read_file(const char *path, mc_buf *out, size_t max);
/* Replaces `path` atomically: a temporary in the same directory with `mode`, fsync,
 * rename, then fsync the directory. 0, or -errno. */
int mc_write_file_atomic(const char *path, const void *bytes, size_t len, mode_t mode);
/* A listening unix socket at `path` with `mode`, replacing a stale socket but never
 * another kind of file. The fd, or -errno. */
int mc_listen_unix(const char *path, int mode);
/* A connected unix socket, or -errno. */
int mc_connect_unix(const char *path);

int64_t mc_now_ms(void);    /* monotonic */
int64_t mc_wall_ms(void);   /* milliseconds since the Unix epoch */

#endif
