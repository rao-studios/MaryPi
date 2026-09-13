#if defined(__linux__)
#define _GNU_SOURCE         /* struct ucred */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE    /* getpeereid, getgrouplist */
#endif
#include "sewn/peer.h"

#include "common/peer.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int sewn_peer_of(int fd, sewn_peer *out) {
    memset(out, 0, sizeof *out);
    int rc = mc_peer_credentials(fd, &out->uid, &out->gid, &out->pid);
    if (rc < 0) return rc;
    out->known = true;
    return 0;
}

bool sewn_uid_in_group(uid_t uid, const char *group) {
    struct passwd pw, *pwp = NULL;
    struct group gr, *grp = NULL;
    char pwbuf[4096], grbuf[16384];
    if (!group || getpwuid_r(uid, &pw, pwbuf, sizeof pwbuf, &pwp) != 0 || !pwp) return false;
    if (getgrnam_r(group, &gr, grbuf, sizeof grbuf, &grp) != 0 || !grp) return false;
    if (pw.pw_gid == gr.gr_gid) return true;
#if defined(__APPLE__)
    int groups[256], n = 256;
    getgrouplist(pw.pw_name, (int)pw.pw_gid, groups, &n);
    for (int i = 0; i < n && i < 256; i++) if ((gid_t)groups[i] == gr.gr_gid) return true;
#else
    gid_t groups[256];
    int n = 256;
    getgrouplist(pw.pw_name, pw.pw_gid, groups, &n);
    for (int i = 0; i < n && i < 256; i++) if (groups[i] == gr.gr_gid) return true;
#endif
    return false;
}

bool sewn_peer_may(const sewn_peer *peer, const char *operation, const char *admin_group, sewn_group_fn in_group) {
    if (!peer || !peer->known || !operation) return false;
    if (peer->uid == 0) return true;
    if (strcmp(operation, "key.set") == 0) return admin_group && in_group && in_group(peer->uid, admin_group);
    return true;
}
