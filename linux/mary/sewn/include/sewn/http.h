/* The one HTTPS client sewnd uses, libcurl configured once for everything sent to
 * Mistral: HTTPS only (redirects included, and none are followed), TLS 1.2 or
 * newer with the peer and host verified against the system's CA store, no
 * signals, bounded connect time. The bearer header is built in a buffer that is
 * zeroed, and the header list is zeroed before it is freed. */
#ifndef MARY_SEWN_HTTP_H
#define MARY_SEWN_HTTP_H

#include <stddef.h>

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
#endif

#endif
