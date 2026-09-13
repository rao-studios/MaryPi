/* Embeddings, the one call threadd cannot make for itself (Providers/
 * EmbeddingModelProvider.swift): mistral-embed, 1024 float32 per text, batches of
 * 256 inputs, one call per batch. */
#ifndef MARY_SEWN_EMBED_H
#define MARY_SEWN_EMBED_H

#include <stddef.h>

#include "sewn/server.h"

#define SEWN_EMBED_MODEL "mistral-embed"
#define SEWN_EMBED_PATH "/v1/embeddings"
#define SEWN_EMBED_DIM 1024
#define SEWN_EMBED_BATCH 256
#define SEWN_EMBED_BODY_MAX (32u << 20)

/* *out is n × *dim floats on the heap in text order. 0; -EINVAL for no texts; -ENOSYS
 * without a transport; -EIO with `message` and *status (429: rate-limited). */
int sewn_embed(sewn_service *svc, const char *key, const char *const *texts, size_t n, const char *purpose, const char *request_id,
               float **out, size_t *dim, long *status, char *message, size_t cap);

/* {model, input, encoding_format: "float"}. */
struct json_object *sewn_embed_body(const char *const *texts, size_t n);
/* data[].embedding, in index order, into out (n × dim); *dim from the first vector. */
int sewn_embed_parse(const char *body, size_t len, size_t n, float *out, size_t *dim);

/* cos(a, b) over dim; 0 when either is zero. */
float sewn_cosine(const float *a, const float *b, size_t dim);

#endif
