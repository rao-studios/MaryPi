#if defined(__APPLE__)
#define _DARWIN_C_SOURCE    /* CLOCK_MONOTONIC under _POSIX_C_SOURCE */
#endif
#include "common/io.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
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

static int64_t clock_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t mc_now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }
int64_t mc_wall_ms(void) { return clock_ms(CLOCK_REALTIME); }
