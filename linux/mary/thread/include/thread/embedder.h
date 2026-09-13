/* Where threadd gets embeddings and graph extraction: from sewnd, over its socket
 * (embed{texts} and graph.extract{texts, policy}), because threadd itself has no
 * network — the Swift node calls Mistral directly (Providers/EmbeddingModelProvider,
 * MistralGraphExtractionProvider); on MaryOS only sewnd does. Both calls are
 * function pointers so tests script them. */
#ifndef MARY_THREAD_EMBEDDER_H
#define MARY_THREAD_EMBEDDER_H

#include <stddef.h>

#define THREAD_EMBED_BATCH 64
#define THREAD_SEWN_SOCKET "/run/sewn/sewn.sock"

/* Writes n × dim floats to out. 0; -EIO with message; -EAGAIN (rate-limited, try later);
 * -ENOENT (sewnd has no key); -ENOSYS (no embedder). */
typedef int (*thread_embed_fn)(const char *const *texts, size_t n, float *out, size_t dim, char *message, size_t cap, void *user);
/* The model's answer as text (the caller parses it with thread_graph_payload_parse);
 * heap in *json_out. Same errors. `prompt` is the policy's system prompt. */
typedef int (*thread_extract_fn)(const char *const *texts, size_t n, const char *prompt, char **json_out, char *message, size_t cap, void *user);

typedef struct thread_embedder {
    thread_embed_fn embed;
    thread_extract_fn extract;
    void *user;
    size_t dim;             /* 1024 */
    size_t batch_max;       /* texts per embed call: 64 keeps a reply under the 1 MiB frame */
} thread_embedder;

/* sewnd at `socket_path` ($SEWN_SOCKET, else THREAD_SEWN_SOCKET when NULL). */
int thread_sewn_embedder_init(thread_embedder *e, const char *socket_path);
void thread_sewn_embedder_free(thread_embedder *e);
/* Embeds any number of texts through the seam, batch by batch. */
int thread_embedder_embed(const thread_embedder *e, const char *const *texts, size_t n, float *out, char *message, size_t cap);

#endif
