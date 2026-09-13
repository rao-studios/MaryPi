/* One completion that is not the spoken turn: Sewn's ModelProvider.run (a one-shot on the
 * utility model — compaction's briefing, the auto-memory note, graph extraction) and
 * runWithTools (the skills lane, /v1/skills/complete: roles kept, the caller's tool
 * roster offered, tool_calls answered). Mistral's chat-completions wire, unstreamed. */
#ifndef MARY_SEWN_COMPLETE_H
#define MARY_SEWN_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

#include "sewn/provider.h"
#include "sewn/server.h"

struct json_object;

#define SEWN_COMPLETE_BODY_MAX (4u << 20)
#define SEWN_SKILLS_MAX_TOKENS_DEFAULT 800
#define SEWN_SKILLS_MAX_TOKENS_MIN 32
#define SEWN_SKILLS_MAX_TOKENS_MAX 2048

typedef struct sewn_complete_request {
    sewn_provider provider;
    const char *model;              /* NULL: the provider's chat model (tools) or SEWN_UTILITY_MODEL */
    const char *system;             /* or NULL */
    struct json_object *messages;   /* [{role, content}], borrowed */
    struct json_object *tools;      /* [{type, function{name, description, parameters}}] or NULL */
    int max_tokens;
    double temperature;
    double top_p;                   /* <= 0: 1 when temperature is 0, else 0.9 (runWithTools) */
    bool json_object;               /* response_format json_object */
    const char *purpose;
    const char *request_id;
} sewn_complete_request;

typedef struct sewn_completion {
    char *text;                     /* heap; "" when the model said nothing */
    struct json_object *tool_calls; /* [{id, name, arguments}] — arguments a JSON string; NULL when none */
    long status;
    int prompt_tokens, completion_tokens;
} sewn_completion;

/* 0; -ENOENT when there is no key; -ENOSYS without a transport; -EIO with `message`
 * (Mistral's own reason, never the key) and *out.status set. */
int sewn_complete(sewn_service *svc, const char *key, const sewn_complete_request *req, sewn_completion *out, char *message, size_t cap);
void sewn_completion_free(sewn_completion *c);

/* ModelProvider.run(prompt, systemPrompt:): one user message on the utility model. */
int sewn_complete_text(sewn_service *svc, const char *key, sewn_provider provider, const char *system, const char *prompt,
                       int max_tokens, double temperature, const char *purpose, const char *request_id, char **text, char *message, size_t cap);

/* The wire body: {model, messages (system first when given), max_tokens, temperature, top_p, [tools], [response_format]}. */
struct json_object *sewn_complete_body(const sewn_complete_request *req);
/* Parses Mistral's chat-completions answer into *out. -EBADMSG when it is not one. */
int sewn_completion_parse(const char *body, size_t len, sewn_completion *out);
/* When the model wrote prose instead of native tool_calls: Mary's
 * <tool_call>{"name","arguments"}</tool_call> form, or a bare JSON object with those
 * keys (skillsCompleteParseToolCalls). Appends to *calls; the tags are removed from *text. */
int sewn_completion_recover_tool_calls(char **text, struct json_object **calls);

#endif
