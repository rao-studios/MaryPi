#include "sewn/http.h"

#ifdef HAVE_CURL
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/secure.h"
#include "sewn/key.h"
#include "sewn/mistral.h"

void sewn_http_harden(CURL *curl, char *errbuf) {
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)SEWN_HTTP_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sewnd/" MARY_VERSION);
    if (errbuf) {
        errbuf[0] = 0;
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    }
}

struct curl_slist *sewn_http_headers(const char *key, const char *content_type) {
    char auth[SEWN_KEY_MAX + 32];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    mc_secure_zero(auth, sizeof auth);
    if (headers && content_type) {
        char type[128];
        snprintf(type, sizeof type, "Content-Type: %s", content_type);
        struct curl_slist *more = curl_slist_append(headers, type);
        if (!more) {
            sewn_http_free_headers(headers);
            return NULL;
        }
        headers = more;
    }
    return headers;
}

void sewn_http_free_headers(struct curl_slist *headers) {
    for (struct curl_slist *h = headers; h; h = h->next)
        if (h->data) mc_secure_zero(h->data, strlen(h->data));
    curl_slist_free_all(headers);
}

static size_t discard(char *bytes, size_t size, size_t n, void *user) { return size * n; }

int sewn_mistral_verify(const char *key, char *message, size_t cap, void *user) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(message, cap, "libcurl did not start");
        return -ENOMEM;
    }
    char errbuf[CURL_ERROR_SIZE];
    sewn_http_harden(curl, errbuf);
    struct curl_slist *headers = sewn_http_headers(key, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, "https://" SEWN_MISTRAL_HOST "/v1/models");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
    CURLcode res = headers ? curl_easy_perform(curl) : CURLE_OUT_OF_MEMORY;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    sewn_http_free_headers(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        snprintf(message, cap, "could not reach Mistral: %s", errbuf[0] ? errbuf : curl_easy_strerror(res));
        return -EIO;
    }
    if (status == 200) {
        snprintf(message, cap, "Mistral accepted the key");
        return 1;
    }
    if (status == 401 || status == 403) snprintf(message, cap, "Mistral rejected the key");
    else snprintf(message, cap, "Mistral answered HTTP %ld", status);
    return 0;
}
#else
typedef int sewn_http_without_curl;
#endif
