/* Who is on the other end of sewnd's socket, as the kernel reports it, and what
 * they may do. Sewn authenticates callers with Supabase tokens
 * (API/Middleware/AuthMiddleware.swift); sewnd has no network listener at all, so
 * the socket's owner, group and mode admit only root and members of `sewn`, and
 * this adds the one finer rule: only the administrators — the `sudo` group, the
 * same people 50-maryos-desktop.rules trusts — may replace the key. */
#ifndef MARY_SEWN_PEER_H
#define MARY_SEWN_PEER_H

#include <stdbool.h>
#include <sys/types.h>

#define SEWN_ADMIN_GROUP "sudo"

typedef struct sewn_peer {
    uid_t uid;
    gid_t gid;
    pid_t pid;          /* -1 where the platform does not say */
    bool known;
} sewn_peer;

typedef bool (*sewn_group_fn)(uid_t uid, const char *group);

/* SO_PEERCRED on Linux, getpeereid(3) elsewhere. 0, or -errno. */
int sewn_peer_of(int fd, sewn_peer *out);
/* Whether the user is in the group, as its primary or a supplementary group. */
bool sewn_uid_in_group(uid_t uid, const char *group);
/* Root may do anything; `key.set` needs `admin_group`; everything else needs only
 * the connection. An unknown peer may do nothing. */
bool sewn_peer_may(const sewn_peer *peer, const char *operation, const char *admin_group, sewn_group_fn in_group);

#endif
