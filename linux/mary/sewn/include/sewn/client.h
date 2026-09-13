/* Talking to sewnd: what sewnctl uses now, and maryd later. */
#ifndef MARY_SEWN_CLIENT_H
#define MARY_SEWN_CLIENT_H

#include <stddef.h>

#define SEWN_SOCKET_PATH "/run/sewn/sewn.sock"

struct json_object;

/* $SEWN_SOCKET, or SEWN_SOCKET_PATH. */
const char *sewn_default_socket(void);
/* A connected socket, or -errno. */
int sewn_connect(const char *path);
/* Reads frames until the first JSON one; *reply is owned by the caller. 0, -EBADMSG, or -errno. */
int sewn_read_reply(int fd, struct json_object **reply);
/* Sends `request` and reads the reply. */
int sewn_call(int fd, struct json_object *request, struct json_object **reply);
/* key.set, built by hand in a buffer that is zeroed, so the key never sits in a
 * json-c object on this side. -EINVAL for an implausible key. */
int sewn_call_key_set(int fd, const char *key, size_t len, struct json_object **reply);

#endif
