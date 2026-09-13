/* A client for the daemons that speak frames on a unix socket (common/frame.h):
 * sewnd today, and whoever else answers a JSON request with JSON frames. One
 * request per connection, the way sewnd serves one op per connection. What
 * sewn/client.h and thread's embedder share, so a package that must reach sewnd
 * (threadd asks it for embeddings) links nothing of sewn's. */
#ifndef MARY_COMMON_SERVICE_H
#define MARY_COMMON_SERVICE_H

struct json_object;

/* $<env> when set and non-empty, else `fallback`. */
const char *mc_service_default_socket(const char *env, const char *fallback);
/* A connected socket, or -errno. */
int mc_service_connect(const char *path);
/* Reads frames until the first JSON one; *reply is owned by the caller. 0, -EBADMSG, or -errno. */
int mc_service_read_reply(int fd, struct json_object **reply);
/* Sends `request` as a JSON frame and reads the reply. */
int mc_service_call(int fd, struct json_object *request, struct json_object **reply);

#endif
