#include "frigate/frigate.h"

#include <errno.h>
#include <stdlib.h>

bool frigate_available(void) { return false; }

frigate_embedder *frigate_embedder_new(const char *model_id) { errno = ENOSYS; return NULL; }
int frigate_embed(frigate_embedder *embedder, const char *const *texts, size_t count, float **vectors, size_t *dimensions) { return -ENOSYS; }
void frigate_embedder_free(frigate_embedder *embedder) { free(embedder); }

frigate_llm *frigate_llm_new(const char *model_id) { errno = ENOSYS; return NULL; }
int frigate_llm_generate(frigate_llm *llm, const char *prompt, int max_tokens, frigate_token_fn on_token, void *user) { return -ENOSYS; }
void frigate_llm_free(frigate_llm *llm) { free(llm); }

frigate_boost *frigate_boost_open(const char *model_json_path) { errno = ENOSYS; return NULL; }
int frigate_boost_feature_count(const frigate_boost *boost) { return -ENOSYS; }
int frigate_boost_predict(const frigate_boost *boost, const float *rows, size_t row_count, float *out) { return -ENOSYS; }
void frigate_boost_free(frigate_boost *boost) { free(boost); }
