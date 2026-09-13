#include "sewn/memory.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/buf.h"
#include "common/json.h"
#include "sewn/complete.h"
#include "sewn/mistral.h"
#include "sewn/retrieve.h"

bool sewn_memory_count_due(int user_messages) { return user_messages > 0 && user_messages % SEWN_MEMORY_EVERY == 0; }

const char *sewn_memory_prompt(void) {
    return "You are a summarization assistant. Summarize the provided conversation into a concise memory note.\n"
           "\n"
           "Format your response as follows:\n"
           "- First line: a bold title (e.g. **Title Here**), 7 words max.\n"
           "- Followed by a blank line.\n"
           "- Then a clear, thorough summary in prose. Capture the key topics, decisions, and takeaways. Write in third person. "
           "Be specific and complete \xE2\x80\x94 this will be used as a memory of what was discussed.";
}

char *sewn_memory_transcript(struct json_object *messages, const char *recent) {
    mc_buf b = { 0 };
    size_t n = messages ? json_object_array_length(messages) : 0;
    for (size_t i = 0; i <= n; i++) {
        const char *role, *content;
        if (i < n) {
            struct json_object *m = json_object_array_get_idx(messages, i);
            role = mc_json_string(m, "role");
            content = mc_json_string(m, "content");
        } else {
            role = "user";
            content = recent;
        }
        if (!role || !content || !*content) continue;
        if (b.len) mc_buf_append_str(&b, "\n");
        mc_buf_append_str(&b, "[");
        mc_buf_append_str(&b, role);
        mc_buf_append_str(&b, "]: ");
        mc_buf_append_str(&b, content);
    }
    if (!b.data) mc_buf_append_str(&b, "");
    return (char *)b.data;
}

/* Sanitize.sanitizePDFContent's patterns, on one line. The cleaned line is only the
 * gate: Swift appends the ORIGINAL line when it has more than four words. */
static bool worth_keeping(const char *line) {
    int words = 1;
    for (const char *p = line; *p; p++) if (*p == ' ') words++;
    return words > 4;
}

size_t sewn_memory_lines(const char *summary, char ***out) {
    *out = NULL;
    size_t n = 0;
    const char *p = summary;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        if (len) {
            char *line = strndup(p, len);
            bool empty = true;
            for (size_t i = 0; i < len; i++) if (!isspace((unsigned char)line[i])) empty = false;
            if (!empty && worth_keeping(line)) {
                *out = realloc(*out, (n + 1) * sizeof **out);
                (*out)[n++] = line;
            } else {
                free(line);
            }
        }
        if (!end) break;
        p = end + 1;
    }
    return n;
}

void sewn_memory_lines_free(char **lines, size_t n) {
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

int sewn_memorize(sewn_service *svc, const char *key, sewn_provider provider, const char *owner, struct json_object *messages,
                  const char *recent, const char *request_id, char *document_id, size_t cap, char *message, size_t msg_cap) {
    char *transcript = sewn_memory_transcript(messages, recent);
    if (!*transcript) {
        free(transcript);
        return -ENOENT;
    }
    char *summary = NULL;
    int rc = sewn_complete_text(svc, key, provider, sewn_memory_prompt(), transcript, SEWN_MEMORY_MAX_TOKENS, SEWN_DEFAULT_TEMPERATURE,
                                "summarize", request_id, &summary, message, msg_cap);
    free(transcript);
    if (rc) return rc;
    char **lines = NULL;
    size_t n = sewn_memory_lines(summary, &lines);
    free(summary);
    if (!n) {
        snprintf(message, msg_cap, "the summary produced no usable text");
        return -ENOENT;
    }
    if (!svc->deposit) {
        sewn_memory_lines_free(lines, n);
        snprintf(message, msg_cap, "no thread to deposit into");
        return -ENOSYS;
    }
    char group[128];
    snprintf(group, sizeof group, "%s%s", SEWN_MEMORY_GROUP_PREFIX, owner);
    rc = svc->deposit(owner, group, SEWN_MEMORY_GROUP_LABEL, "memory", (const char *const *)lines, n, document_id, cap, svc->deposit_user);
    sewn_memory_lines_free(lines, n);
    if (rc) snprintf(message, msg_cap, "threadd refused the memory: %s", strerror(-rc));
    return rc;
}
