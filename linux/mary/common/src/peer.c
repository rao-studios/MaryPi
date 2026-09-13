#if defined(__linux__)
#define _GNU_SOURCE         /* struct ucred */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE    /* getpeereid */
#endif
#include "common/peer.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int mc_peer_credentials(int fd, uid_t *uid, gid_t *gid, pid_t *pid) {
#if defined(__linux__)
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) return -errno;
    if (uid) *uid = cred.uid;
    if (gid) *gid = cred.gid;
    if (pid) *pid = cred.pid;
#else
    uid_t u;
    gid_t g;
    if (getpeereid(fd, &u, &g) < 0) return -errno;
    if (uid) *uid = u;
    if (gid) *gid = g;
    if (pid) *pid = -1;
#endif
    return 0;
}

void mc_user_name(uid_t uid, char *out, size_t cap) {
    struct passwd pw, *found = NULL;
    char buf[4096];
    if (getpwuid_r(uid, &pw, buf, sizeof buf, &found) == 0 && found && found->pw_name && *found->pw_name)
        snprintf(out, cap, "%s", found->pw_name);
    else
        snprintf(out, cap, "uid-%u", (unsigned)uid);
}
