/* The one HTTPS client sewnd uses, libcurl configured once for everything sent to
 * Mistral: HTTPS only (redirects included, and none are followed), TLS 1.2 or
 * newer with the peer and host verified against the system's CA store, no
 * signals, bounded connect time. The bearer header is built in a buffer that is
 * zeroed, and the header list is zeroed before it is freed. */
#ifndef MARY_SEWN_HTTP_H
#define MARY_SEWN_HTTP_H

#include <stddef.h>

#include "sewn/transport.h"

#define SEWN_HTTP_CONNECT_TIMEOUT_MS 10000

#ifdef HAVE_CURL
#include <curl/curl.h>

void sewn_http_harden(CURL *curl, char *errbuf);
/* "Authorization: Bearer <key>" and, when given, a Content-Type. NULL out of memory. */
struct curl_slist *sewn_http_headers(const char *key, const char *content_type);
void sewn_http_free_headers(struct curl_slist *headers);

/* GET /v1/models with the key: 1 when Mistral accepts it, 0 when it refuses
 * (`message` says how), or -EIO when Mistral could not be reached. */
int sewn_mistral_verify(const char *key, char *message, size_t cap, void *user);

/* sewn_post_stream_fn over libcurl: POST to https://api.mistral.ai<path> with
 * Accept: text/event-stream. A stalled stream (under 1 byte a second for 60 s) fails. */
int sewn_http_post_stream(const char *path, const char *key, const char *body, size_t body_len,
                          sewn_bytes_fn on_bytes, sewn_stop_fn should_stop, void *user,
                          long *status, char *message, size_t cap, void *transport_user);
#endif

#endif
