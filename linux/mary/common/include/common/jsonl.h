/* One request, one reply, as newline-delimited JSON over a unix socket: the shape
 * threadd's local socket and the desktop's socket speak. */
#ifndef MARY_COMMON_JSONL_H
#define MARY_COMMON_JSONL_H

#include <stddef.h>

struct json_object;

#define MC_JSONL_LINE_MAX (16u << 20)

/* Connects, writes `request` as one line, reads one line back. *reply is a new
 * reference (NULL on failure). 0; -errno when the socket could not be reached or
 * hung up (`message` says which, when given); -EIO when the reply is
 * {"type": "error", …} — *reply then holds it, so the caller can read the message. */
int mc_jsonl_call(const char *socket_path, int timeout_ms, struct json_object *request, struct json_object **reply, char *message, size_t cap);

#endif
