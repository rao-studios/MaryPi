/* Who is on the other end of a unix socket, as the kernel reports it. sewnd and
 * threadd both decide what a caller may do from this, never from what it says. */
#ifndef MARY_COMMON_PEER_H
#define MARY_COMMON_PEER_H

#include <stddef.h>
#include <sys/types.h>

/* SO_PEERCRED on Linux, getpeereid(3) elsewhere (where *pid is -1). 0, or -errno. */
int mc_peer_credentials(int fd, uid_t *uid, gid_t *gid, pid_t *pid);
/* The user's login name, or "uid-<n>" for a user without one. */
void mc_user_name(uid_t uid, char *out, size_t cap);

#endif
