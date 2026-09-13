/* Auto-memory (Core/Sewn+AutoMemory.swift): every seventh user message, or when the
 * topic changes, the conversation is summarised into a memory note — a bold title of
 * at most seven words, then prose — sanitised into lines and deposited in the owner's
 * `memory-<owner>` group ("Memory"), where the Thread embeds it and folds it into the
 * graph. Sinatra's session-boundary signal is gone; the topic change is the cosine
 * between the last two user messages' embeddings falling under 0.55. */
#ifndef MARY_SEWN_MEMORY_H
#define MARY_SEWN_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

#include "sewn/provider.h"
#include "sewn/server.h"

struct json_object;

#define SEWN_MEMORY_EVERY 7
#define SEWN_MEMORY_TOPIC_COSINE 0.55f
#define SEWN_MEMORY_MAX_TOKENS 777
#define SEWN_MEMORY_GROUP_LABEL "Memory"
#define SEWN_MEMORY_GROUP_PREFIX "memory-"

/* Whether the message count alone fires (userMessageCount % 7 == 0). */
bool sewn_memory_count_due(int user_messages);
/* The summariser's system prompt (also /v1/tools/summarize's). */
const char *sewn_memory_prompt(void);
/* "[role]: content" lines of the history plus the latest user message (empty contents skipped). Heap. */
char *sewn_memory_transcript(struct json_object *messages, const char *recent);
/* Sanitize.sanitizePDFContent: the summary's lines, cleaned, keeping those of more than
 * four words. Heap array of heap strings. */
size_t sewn_memory_lines(const char *summary, char ***out);
void sewn_memory_lines_free(char **lines, size_t n);

/* Summarise → sanitise → deposit through svc->deposit. 0; -ENOENT nothing to keep;
 * -errno from the model or threadd (logged by the caller). */
int sewn_memorize(sewn_service *svc, const char *key, sewn_provider provider, const char *owner, struct json_object *messages,
                  const char *recent, const char *request_id, char *document_id, size_t cap, char *message, size_t msg_cap);

#endif
