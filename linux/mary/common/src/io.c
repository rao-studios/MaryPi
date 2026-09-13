#if defined(__APPLE__)
#define _DARWIN_C_SOURCE    /* CLOCK_MONOTONIC under _POSIX_C_SOURCE */
#endif
#include "common/io.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int mc_write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w;
#ifdef MSG_NOSIGNAL
        w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == ENOTSOCK) w = write(fd, p, n);
#else
        w = write(fd, p, n);
#endif
        if (w < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int mc_set_nonblocking(int fd, bool on) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -errno;
    flags = on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) < 0 ? -errno : 0;
}

int mc_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -errno;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -errno : 0;
}

void mc_ignore_sigpipe(void) {
    struct sigaction sa = { 0 };
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

int mc_read_file(const char *path, mc_buf *out, size_t max) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -errno;
    size_t start = out->len;
    int rc = 0;
    char chunk[8192];
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0) {
            if (errno == EINTR) continue;
            rc = -errno;
            break;
        }
        if (got == 0) break;
        if (out->len - start + (size_t)got > max) {
            rc = -EFBIG;
            break;
        }
        if ((rc = mc_buf_append(out, chunk, (size_t)got)) < 0) break;
    }
    close(fd);
    if (rc < 0) {
        out->len = start;
        if (out->data) out->data[start] = 0;
    }
    return rc;
}

int mc_write_file_atomic(const char *path, const void *bytes, size_t len, mode_t mode) {
    char tmp[1024];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) return -ENAMETOOLONG;
    int fd = mkstemp(tmp);
    if (fd < 0) return -errno;
    int rc = fchmod(fd, mode) < 0 ? -errno : 0;
    if (rc == 0) rc = mc_write_all(fd, bytes, len);
    if (rc == 0 && fsync(fd) < 0) rc = -errno;
    if (close(fd) < 0 && rc == 0) rc = -errno;
    if (rc == 0 && rename(tmp, path) < 0) rc = -errno;
    if (rc) {
        unlink(tmp);
        return rc;
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *(slash == dir ? slash + 1 : slash) = 0;
        int dfd = open(dir, O_RDONLY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return 0;
}

static int unix_address(const char *path, struct sockaddr_un *addr) {
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof addr->sun_path) return -ENAMETOOLONG;
    memcpy(addr->sun_path, path, n + 1);
    return 0;
}

int mc_listen_unix(const char *path, int mode) {
    struct sockaddr_un addr;
    int rc = unix_address(path, &addr);
    if (rc) return rc;
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

int mc_connect_unix(const char *path) {
    struct sockaddr_un addr;
    int rc = unix_address(path, &addr);
    if (rc) return rc;
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

static int64_t clock_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t mc_now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }
int64_t mc_wall_ms(void) { return clock_ms(CLOCK_REALTIME); }
