#include "sewn/embed.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/buf.h"
#include "common/json.h"
#include "sewn/mistral.h"
#include "sewn/outbound.h"
#include "sewn/speech.h"

struct json_object *sewn_embed_body(const char *const *texts, size_t n) {
    struct json_object *body = json_object_new_object(), *input = json_object_new_array();
    json_object_object_add(body, "model", json_object_new_string(SEWN_EMBED_MODEL));
    for (size_t i = 0; i < n; i++) json_object_array_add(input, json_object_new_string(texts[i]));
    json_object_object_add(body, "input", input);
    json_object_object_add(body, "encoding_format", json_object_new_string("float"));
    return body;
}

int sewn_embed_parse(const char *body, size_t len, size_t n, float *out, size_t *dim) {
    struct json_object *o = mc_json_parse(body, len);
    struct json_object *data = mc_json_array(o, "data");
    if (!data || json_object_array_length(data) != n) {
        json_object_put(o);
        return -EBADMSG;
    }
    size_t d = 0;
    for (size_t i = 0; i < n; i++) {
        struct json_object *item = json_object_array_get_idx(data, i);
        int64_t index = (int64_t)i;
        mc_json_int64(item, "index", &index);
        struct json_object *vec = mc_json_array(item, "embedding");
        if (index < 0 || (size_t)index >= n || !vec) {
            json_object_put(o);
            return -EBADMSG;
        }
        size_t vd = json_object_array_length(vec);
        if (!d) d = vd;
        if (vd != d || d == 0) {
            json_object_put(o);
            return -EBADMSG;
        }
        float *row = out + (size_t)index * d;
        for (size_t k = 0; k < d; k++) row[k] = (float)json_object_get_double(json_object_array_get_idx(vec, k));
    }
    *dim = d;
    json_object_put(o);
    return 0;
}

struct collecting {
    mc_buf body;
};

static int on_bytes(const char *bytes, size_t len, long status, void *user) {
    struct collecting *c = user;
    if (c->body.len + len > SEWN_EMBED_BODY_MAX) return 1;
    return mc_buf_append(&c->body, bytes, len) < 0;
}

static bool never_stop(void *user) { return false; }

int sewn_embed(sewn_service *svc, const char *key, const char *const *texts, size_t n, const char *purpose, const char *request_id,
               float **out, size_t *dim, long *status, char *message, size_t cap) {
    *out = NULL;
    *dim = 0;
    *status = 0;
    if (!n) return -EINVAL;
    float *vectors = NULL;
    size_t d = 0;
    for (size_t off = 0; off < n; off += SEWN_EMBED_BATCH) {
        size_t count = n - off < SEWN_EMBED_BATCH ? n - off : SEWN_EMBED_BATCH;
        struct json_object *body = sewn_embed_body(texts + off, count);
        size_t body_len = 0;
        const char *body_text = mc_json_compact(body, &body_len);
        struct collecting c = { { 0 } };
        sewn_outbound o = { SEWN_PROVIDER_MISTRAL, purpose ? purpose : "embed", request_id };
        int rc = sewn_post(svc, &o, SEWN_EMBED_PATH, key, body_text, body_len, on_bytes, never_stop, &c, status, message, cap);
        json_object_put(body);
        if (rc < 0 && rc != SEWN_STREAM_STOPPED) {
            mc_buf_free(&c.body);
            free(vectors);
            if (!message[0]) snprintf(message, cap, "Mistral could not be reached");
            return rc == -ENOSYS ? -ENOSYS : -EIO;
        }
        if (*status < 200 || *status > 299) {
            sewn_speech_failure(*status, (const char *)c.body.data, c.body.len, message, cap);
            mc_buf_free(&c.body);
            free(vectors);
            return -EIO;
        }
        /* The first batch says the dimension; the arena is sized from it. */
        if (!vectors) {
            size_t probe = 0;
            float *first = malloc(count * 8192 * sizeof *first);   /* room for any model's width */
            if (!first || sewn_embed_parse((const char *)c.body.data, c.body.len, count, first, &probe) < 0 || probe > 8192) {
                free(first);
                mc_buf_free(&c.body);
                snprintf(message, cap, "Mistral's embeddings could not be read");
                return -EIO;
            }
            d = probe;
            vectors = malloc(n * d * sizeof *vectors);
            if (!vectors) {
                free(first);
                mc_buf_free(&c.body);
                return -ENOMEM;
            }
            memcpy(vectors, first, count * d * sizeof *vectors);
            free(first);
        } else {
            size_t got = 0;
            if (sewn_embed_parse((const char *)c.body.data, c.body.len, count, vectors + off * d, &got) < 0 || got != d) {
                mc_buf_free(&c.body);
                free(vectors);
                snprintf(message, cap, "Mistral's embeddings could not be read");
                return -EIO;
            }
        }
        mc_buf_free(&c.body);
    }
    *out = vectors;
    *dim = d;
    return 0;
}

float sewn_cosine(const float *a, const float *b, size_t dim) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    if (na <= 0 || nb <= 0) return 0;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}
