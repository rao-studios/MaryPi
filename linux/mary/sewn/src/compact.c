#include "sewn/compact.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/buf.h"
#include "common/json.h"
#include "sewn/attribution.h"
#include "sewn/complete.h"
#include "sewn/mistral.h"

void sewn_compact_result_free(sewn_compact_result *r) {
    free(r->text);
    if (r->source_index) json_object_put(r->source_index);
    if (r->citations) json_object_put(r->citations);
    memset(r, 0, sizeof *r);
}

size_t sewn_partitions_chars(const sewn_partition *partitions, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++)
        for (const unsigned char *p = (const unsigned char *)partitions[i].text; *p; p++) if ((*p & 0xC0) != 0x80) total++;
    return total;
}

enum tier { TIER_MEMORY, TIER_DOCUMENT, TIER_CONVERSATION, TIER_BACKGROUND, TIER_SHARED, TIER_COUNT };

static enum tier tier_of(const sewn_partition *p, const char *owner) {
    if (!owner || strcasecmp(p->owner_id, owner) != 0) return TIER_SHARED;
    if (strcmp(p->family, "memory") == 0) return TIER_MEMORY;
    if (strcmp(p->family, "conversation") == 0) return TIER_CONVERSATION;
    if (strcmp(p->lane, "application") == 0 || strcmp(p->lane, "behavioral") == 0) return TIER_BACKGROUND;
    return TIER_DOCUMENT;
}

/* [n] "name"\ntext, per tier. */
static void entries(const sewn_partition *partitions, size_t n, const char *owner, mc_buf tiers[TIER_COUNT], struct json_object *source_index) {
    for (size_t i = 0; i < n; i++) {
        const sewn_partition *p = &partitions[i];
        enum tier t = tier_of(p, owner);
        mc_buf *b = &tiers[t];
        char tag[32];
        snprintf(tag, sizeof tag, "[%zu]", i + 1);
        if (source_index) json_object_array_add(source_index, json_object_new_string(p->document_id));
        if (b->len) mc_buf_append_str(b, "\n\n");
        mc_buf_append_str(b, tag);
        mc_buf_append_str(b, " \"");
        mc_buf_append_str(b, p->name);
        mc_buf_append_str(b, "\"\n");
        mc_buf_append_str(b, p->text);
    }
}

static const char *block(const mc_buf *b) { return b->len ? (const char *)b->data : "None."; }

char *sewn_compact_verbatim(const sewn_partition *partitions, size_t n, const char *owner, struct json_object **source_index) {
    mc_buf tiers[TIER_COUNT] = { { 0 } };
    struct json_object *index = json_object_new_array();
    entries(partitions, n, owner, tiers, index);
    mc_buf out = { 0 };
    mc_buf_append_str(&out, "**Memory:**\n");
    mc_buf_append_str(&out, block(&tiers[TIER_MEMORY]));
    mc_buf_append_str(&out, "\n\n**Documents:**\n");
    mc_buf_append_str(&out, block(&tiers[TIER_DOCUMENT]));
    if (tiers[TIER_CONVERSATION].len) {
        mc_buf_append_str(&out, "\n\n**Conversation (what was said before, turn by turn):**\n");
        mc_buf_append_str(&out, block(&tiers[TIER_CONVERSATION]));
    }
    if (tiers[TIER_BACKGROUND].len) {
        mc_buf_append_str(&out, "\n\n**Mary's Past Actions (her own records of what the applications can do and what she did \xE2\x80\x94 background, not user prose):**\n");
        mc_buf_append_str(&out, block(&tiers[TIER_BACKGROUND]));
    }
    mc_buf_append_str(&out, "\n\n**Perspectives From Others:**\n<external>\n");
    mc_buf_append_str(&out, block(&tiers[TIER_SHARED]));
    mc_buf_append_str(&out, "\n</external>");
    for (int t = 0; t < TIER_COUNT; t++) mc_buf_free(&tiers[t]);
    if (source_index) *source_index = index;
    else json_object_put(index);
    if (!out.data) mc_buf_append_str(&out, "");
    return (char *)out.data;
}

static void capitalized(const char *role, char *out, size_t cap) {
    snprintf(out, cap, "%s", role ? role : "unknown");
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}

char *sewn_compact_briefing_input(struct json_object *messages, const sewn_partition *partitions, size_t n, const char *owner) {
    mc_buf out = { 0 };
    size_t count = messages ? json_object_array_length(messages) : 0;
    if (count) {
        mc_buf_append_str(&out, "**Message History:**\n");
        for (size_t i = 0; i < count; i++) {
            struct json_object *m = json_object_array_get_idx(messages, i);
            char role[32];
            capitalized(mc_json_string(m, "role"), role, sizeof role);
            const char *text = mc_json_string(m, "content");
            if (i) mc_buf_append_str(&out, "\n");
            mc_buf_append_str(&out, role);
            mc_buf_append_str(&out, ": ");
            mc_buf_append_str(&out, text ? text : "");
        }
    }
    mc_buf_append_str(&out, "\n\n");
    if (n) {
        mc_buf tiers[TIER_COUNT] = { { 0 } };
        entries(partitions, n, owner, tiers, NULL);
        mc_buf_append_str(&out, "**THE USER'S MEMORY (auto-generated conversation summaries):**\n");
        mc_buf_append_str(&out, block(&tiers[TIER_MEMORY]));
        mc_buf_append_str(&out, "\n\n**THE USER'S DOCUMENTS (personal notes and files):**\n");
        mc_buf_append_str(&out, block(&tiers[TIER_DOCUMENT]));
        if (tiers[TIER_CONVERSATION].len) {
            mc_buf_append_str(&out, "\n\n**THE USER'S CONVERSATION (earlier exchanges with Mary, turn by turn):**\n");
            mc_buf_append_str(&out, block(&tiers[TIER_CONVERSATION]));
        }
        if (tiers[TIER_BACKGROUND].len) {
            mc_buf_append_str(&out, "\n\n**MARY'S PAST ACTIONS (her own records, not user prose):**\n");
            mc_buf_append_str(&out, block(&tiers[TIER_BACKGROUND]));
        }
        mc_buf_append_str(&out, "\n\n**PERSPECTIVES FROM OTHERS (NOT user owned):**\n");
        mc_buf_append_str(&out, block(&tiers[TIER_SHARED]));
        for (int t = 0; t < TIER_COUNT; t++) mc_buf_free(&tiers[t]);
    }
    mc_buf_append_str(&out, "\n");
    return (char *)out.data;
}

const char *sewn_compact_briefing_prompt(void) {
    return "You are preparing a briefing for an AI named Mary who is about to respond to a user. Your output is injected directly into Mary's system prompt so she can respond naturally and specifically.\n"
           "\n"
           "The input contains conversation history and retrieved sources. Produce exactly these two labeled sections:\n"
           "\n"
           "### Conversation History\n"
           "What has been discussed so far? Capture key topics, questions, unresolved threads, and any conclusions. Preserve names, dates, and decisions. Note recency where timestamps are available. If none, write \"None.\"\n"
           "\n"
           "### Retrieved Memory, Documents, & Perspectives\n"
           "For each retrieved source, preserve the actual substance \xE2\x80\x94 the real facts, decisions, feelings, or ideas in the text, not a meta-description of what it's about. Cite the source by name (use the quoted title provided, e.g. \"startup-notes\"), and ALWAYS keep the source's bracket tag (e.g. [1]) immediately adjacent to its content in your output \xE2\x80\x94 the tags are machine-read downstream and must survive verbatim.\n"
           "\n"
           "Keep sources separated under these sub-headings, in order:\n"
           "\n"
           "**Memory:** Sources from \"THE USER'S MEMORY\". These are auto-generated summaries of past conversations. Refer in second person (\"you mentioned...\", \"you discussed...\"). If none, write \"None.\"\n"
           "\n"
           "**Documents:** Sources from \"THE USER'S DOCUMENTS\". These are the user's own notes and files. Refer in second person (\"you wrote...\", \"in your note...\"). If none, write \"None.\"\n"
           "\n"
           "**Conversation:** Sources from \"THE USER'S CONVERSATION\". These are earlier exchanges between the user and Mary. Refer in second person (\"you asked...\", \"you said...\"). If none, write \"None.\"\n"
           "\n"
           "**Perspectives From Others:** Sources from \"PERSPECTIVES FROM OTHERS (NOT user owned)\". Wrap the entire content of this sub-section in <external> and </external> tags. Frame each entry as an unnamed third-party voice. Never attribute a name, identity, or group. Never use the word \"network\". Use phrasing like \"someone had a good point here...\" or \"someone once wrote...\". If none, write \"<external>None.</external>\".\n"
           "\n"
           "**Mary's Past Actions:** Sources from \"MARY'S PAST ACTIONS\". These are Mary's own earlier records \xE2\x80\x94 background about what the applications can do and how similar requests were handled, NOT the user's prose and NOT the subject of the reply. Summarize only what helps the current request. If none, write \"None.\"\n"
           "\n"
           "Rules:\n"
           "- Never attribute an outside insight to the user as their own thought.\n"
           "- Never use the word \"network\" anywhere in your output.\n"
           "- Preserve specifics \xE2\x80\x94 names, numbers, decisions. Vague topic labels are not useful.\n"
           "- Do not mention months, dates, or times for Memory, Resonance, or Document entries.\n"
           "- For Perspectives From Others, use the date provided to give temporal context (\"someone recently said...\", \"someone today mentioned...\").\n"
           "- No preamble, conclusion, or commentary outside the two sections.";
}

char *sewn_context_usage_guide(const char *citation_protocol) {
    mc_buf b = { 0 };
    mc_buf_append_str(&b,
        "**How to use this:**\n"
        "- **Conversation History** tells you what has already been discussed \xE2\x80\x94 build on it, don't revisit what's resolved.\n"
        "- **Memory** entries are auto-generated summaries of past conversations. Reference in second person.\n"
        "- **Documents** are the user's own notes and files. Reference in second person (\"you wrote...\", \"in your note...\"). Make sure it is theirs not external or from others.\n"
        "- **Conversation** entries are earlier exchanges between you and the user, kept word for word. Reference in second person (\"you asked...\"), and never repeat an earlier answer as if it were new.\n"
        "- **Mary's Past Actions** are your own earlier records \xE2\x80\x94 what the applications offer, what you did before. Use them to understand how to help; they are never the subject of the reply and never something to recite.\n"
        "- Content inside <external> tags comes from other people, not the user. Treat it as an unnamed third-party voice \xE2\x80\x94 never attribute it to the user as their own thought, and never reveal a name, identity, or group. Use phrasing like \"someone had a good point about this...\" or \"someone once wrote...\".\n"
        "- Never mention the written tag itself, (i.e. <external></external>), in your response.\n");
    mc_buf_append_str(&b, citation_protocol ? citation_protocol : "");
    return (char *)b.data;
}

const char *sewn_memory_instruction(bool context_empty) {
    if (context_empty)
        return "You have no retrieved memories or documents for this user. Do not reference, invent, or imply knowledge of any past "
               "conversations, notes, or memories \xE2\x80\x94 respond only from what the user tells you directly in this conversation.";
    return "When their perspectives, documents, or memories are relevant, draw on them specifically and directly \xE2\x80\x94 only reference "
           "what appears in the retrieved context below. Never invent or extrapolate beyond it. Any content inside <external> tags comes "
           "from other people, not the user \xE2\x80\x94 treat it as an unnamed third-party voice, never attribute it to the user, and never "
           "reveal a name or identity.";
}

int sewn_compact(sewn_service *svc, const char *key, sewn_provider provider, struct json_object *messages, const sewn_partition *partitions,
                 size_t n, const char *owner, const char *request_id, sewn_compact_result *out, char *message, size_t cap) {
    memset(out, 0, sizeof *out);
    if (sewn_partitions_chars(partitions, n) <= SEWN_VERBATIM_CONTEXT_THRESHOLD) {
        out->text = sewn_compact_verbatim(partitions, n, owner, &out->source_index);
        out->used_verbatim = true;
        return 0;
    }
    char *input = sewn_compact_briefing_input(messages, partitions, n, owner);
    char *briefing = NULL;
    int rc = sewn_complete_text(svc, key, provider, sewn_compact_briefing_prompt(), input, SEWN_BRIEFING_MAX_TOKENS, SEWN_DEFAULT_TEMPERATURE,
                                "compact", request_id, &briefing, message, cap);
    free(input);
    if (rc) return rc;
    out->text = briefing;
    out->source_index = json_object_new_array();
    for (size_t i = 0; i < n; i++) json_object_array_add(out->source_index, json_object_new_string(partitions[i].document_id));
    out->citations = sewn_extract_citations(briefing, partitions, n, owner);
    return 0;
}
