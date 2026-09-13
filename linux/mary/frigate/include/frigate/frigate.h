/* Frigate in C: the on-device model seams (Frigate/Sources/Frigate). Nothing is
 * backed yet — MaryOS reaches models through sewnd — so every constructor
 * returns NULL with errno ENOSYS and frigate_available() is false. */
#ifndef MARY_FRIGATE_H
#define MARY_FRIGATE_H

#include <stdbool.h>
#include <stddef.h>

/* The defaults FrigateEmbedder.init and FrigateLLM.init take. */
#define FRIGATE_DEFAULT_EMBEDDER_MODEL "mlx-community/Qwen3-Embedding-0.6B-8bit"
#define FRIGATE_DEFAULT_LLM_MODEL "mlx-community/Qwen3-0.6B-4bit"
#define FRIGATE_DEFAULT_MAX_TOKENS 512

bool frigate_available(void);

/* FrigateEmbedder.swift: texts in, one vector per text out. */
typedef struct frigate_embedder frigate_embedder;
frigate_embedder *frigate_embedder_new(const char *model_id);        /* NULL: the default */
/* On success *vectors is count × *dimensions floats the caller frees. 0, or -errno. */
int frigate_embed(frigate_embedder *embedder, const char *const *texts, size_t count, float **vectors, size_t *dimensions);
void frigate_embedder_free(frigate_embedder *embedder);

/* FrigateLLM.swift: generate(prompt:maxTokens:) streams tokens. The callback
 * returns false to stop early. 0, or -errno. */
typedef struct frigate_llm frigate_llm;
typedef bool (*frigate_token_fn)(const char *text, void *user);
frigate_llm *frigate_llm_new(const char *model_id);                  /* NULL: the default */
int frigate_llm_generate(frigate_llm *llm, const char *prompt, int max_tokens, frigate_token_fn on_token, void *user);
void frigate_llm_free(frigate_llm *llm);

/* FrigateBoost.swift: an XGBoost model saved as JSON, predicting one value per row. */
typedef struct frigate_boost frigate_boost;
frigate_boost *frigate_boost_open(const char *model_json_path);
int frigate_boost_feature_count(const frigate_boost *boost);
int frigate_boost_predict(const frigate_boost *boost, const float *rows, size_t row_count, float *out);
void frigate_boost_free(frigate_boost *boost);

#endif
