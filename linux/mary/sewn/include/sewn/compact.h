/* Core/Commands/Sewn+Compact.swift: the retrieved partitions become the context the
 * model reads. Small retrievals (≤ 6000 characters) are injected verbatim under
 * tagged headings; larger ones are briefed by the utility model, with the [n] tags
 * kept adjacent to their content so the marker protocol can name them. The tiers are
 * the Mac's, plus MaryOS's background tier for Mary's own behavioural records (the behavioral lane),
 * tier for Mary's own application and behavioral records, which she is told never to
 * recite. */
#ifndef MARY_SEWN_COMPACT_H
#define MARY_SEWN_COMPACT_H

#include <stdbool.h>
#include <stddef.h>

#include "sewn/provider.h"
#include "sewn/retrieve.h"
#include "sewn/server.h"

#define SEWN_VERBATIM_CONTEXT_THRESHOLD 6000
#define SEWN_BRIEFING_MAX_TOKENS 600

typedef struct sewn_compact_result {
    char *text;                     /* the context block's body */
    struct json_object *source_index;   /* [document_id, …]: tag n is index n-1 */
    struct json_object *citations;      /* [{document_id, key_words: [...]}] (briefing only) */
    bool used_verbatim;
} sewn_compact_result;

void sewn_compact_result_free(sewn_compact_result *r);

/* `messages` is the history, [{role, content}], excluding the latest user message;
 * `owner` the request's owner (lowercased match, as Sewn does). 0; -EIO with `message`
 * when the briefing could not be made (the verbatim path never fails). */
int sewn_compact(sewn_service *svc, const char *key, sewn_provider provider, struct json_object *messages, const sewn_partition *partitions,
                 size_t n, const char *owner, const char *request_id, sewn_compact_result *out, char *message, size_t cap);

/* The verbatim block alone (no service needed; tests and the turn's empty case). */
char *sewn_compact_verbatim(const sewn_partition *partitions, size_t n, const char *owner, struct json_object **source_index);
/* The briefing's system prompt and input (pinned by tests). */
const char *sewn_compact_briefing_prompt(void);
char *sewn_compact_briefing_input(struct json_object *messages, const sewn_partition *partitions, size_t n, const char *owner);
/* "How to use this" and the memory posture (Sewn.contextUsageGuide / memoryInstruction). */
char *sewn_context_usage_guide(const char *citation_protocol);
const char *sewn_memory_instruction(bool context_empty);
/* Total characters (code points) across the partitions. */
size_t sewn_partitions_chars(const sewn_partition *partitions, size_t n);

#endif
