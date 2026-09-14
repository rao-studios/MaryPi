#include "sewn/complete.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/buf.h"
#include "common/json.h"
#include "common/log.h"
#include "common/secure.h"
#include "sewn/key.h"
#include "sewn/mistral.h"
#include "sewn/outbound.h"
#include "sewn/speech.h"

static struct json_object *number(double value) {
    char text[32];
    snprintf(text, sizeof text, "%.15g", value);
    return json_object_new_double_s(value, text);
}

static struct json_object *role_content(const char *role, const char *content) {
    struct json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string(role));
    json_object_object_add(m, "content", json_object_new_string(content));
    return m;
}

struct json_object *sewn_complete_body(const sewn_complete_request *req) {
    struct json_object *body = json_object_new_object(), *messages = json_object_new_array();
    const char *model = req->model ? req->model : req->tools ? sewn_provider_chat_model(req->provider, NULL) : SEWN_UTILITY_MODEL;
    json_object_object_add(body, "model", json_object_new_string(model));
    if (req->system && *req->system) json_object_array_add(messages, role_content("system", req->system));
    size_t n = req->messages ? json_object_array_length(req->messages) : 0;
    for (size_t i = 0; i < n; i++) {
        struct json_object *m = json_object_array_get_idx(req->messages, i);
        const char *role = mc_json_string(m, "role"), *content = mc_json_string(m, "content");
        if (!role) continue;
        /* skillsCompleteMessages: empty prose drops, roles stay intact — but a tool round keeps its shape: the
         * assistant message that carries tool_calls (its content is "") and every tool result stay, or Mistral
         * answers "Unexpected role 'tool' after role 'user'" (HTTP 400) and the lane never gets its prose. */
        bool tool_round = mc_json_array(m, "tool_calls") != NULL || strcmp(role, "tool") == 0;
        const char *p = content ? content : "";
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p && !tool_round) continue;
        json_object_array_add(messages, json_object_get(m));
    }
    json_object_object_add(body, "messages", messages);
    json_object_object_add(body, "max_tokens", json_object_new_int(req->max_tokens > 0 ? req->max_tokens : SEWN_DEFAULT_MAX_TOKENS));
    json_object_object_add(body, "temperature", number(req->temperature));
    double top_p = req->top_p > 0 ? req->top_p : req->temperature == 0 ? 1.0 : 0.9;
    json_object_object_add(body, "top_p", number(top_p));
    if (req->tools && json_object_array_length(req->tools)) json_object_object_add(body, "tools", json_object_get(req->tools));
    if (req->json_object) {
        struct json_object *format = json_object_new_object();
        json_object_object_add(format, "type", json_object_new_string("json_object"));
        json_object_object_add(body, "response_format", format);
    }
    return body;
}

/* Mistral may answer content as a string or as [{type: "text", text}] parts. */
static char *content_text(struct json_object *content) {
    if (!content) return strdup("");
    if (json_object_is_type(content, json_type_string)) return strdup(json_object_get_string(content));
    mc_buf b = { 0 };
    if (json_object_is_type(content, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(content); i++) {
            const char *text = mc_json_string(json_object_array_get_idx(content, i), "text");
            if (text) mc_buf_append_str(&b, text);
        }
    }
    if (!b.data) mc_buf_append_str(&b, "");
    return (char *)b.data;
}

int sewn_completion_parse(const char *body, size_t len, sewn_completion *out) {
    memset(out, 0, sizeof *out);
    struct json_object *o = mc_json_parse(body, len);
    struct json_object *choices = mc_json_array(o, "choices");
    struct json_object *choice = choices && json_object_array_length(choices) ? json_object_array_get_idx(choices, 0) : NULL;
    struct json_object *message = mc_json_object(choice, "message");
    if (!message) {
        json_object_put(o);
        return -EBADMSG;
    }
    struct json_object *content = NULL;
    json_object_object_get_ex(message, "content", &content);
    out->text = content_text(content);
    struct json_object *calls = mc_json_array(message, "tool_calls");
    for (size_t i = 0; calls && i < json_object_array_length(calls); i++) {
        struct json_object *call = json_object_array_get_idx(calls, i), *function = mc_json_object(call, "function");
        const char *name = mc_json_string(function, "name");
        if (!name || !*name) continue;
        struct json_object *args = NULL;
        json_object_object_get_ex(function, "arguments", &args);
        struct json_object *entry = json_object_new_object();
        const char *id = mc_json_string(call, "id");
        json_object_object_add(entry, "id", json_object_new_string(id ? id : ""));
        json_object_object_add(entry, "name", json_object_new_string(name));
        if (args && json_object_is_type(args, json_type_string)) json_object_object_add(entry, "arguments", json_object_new_string(json_object_get_string(args)));
        else if (args) json_object_object_add(entry, "arguments", json_object_new_string(json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)));
        else json_object_object_add(entry, "arguments", json_object_new_string("{}"));
        if (!out->tool_calls) out->tool_calls = json_object_new_array();
        json_object_array_add(out->tool_calls, entry);
    }
    struct json_object *usage = mc_json_object(o, "usage");
    int64_t v = 0;
    if (mc_json_int64(usage, "prompt_tokens", &v)) out->prompt_tokens = (int)v;
    v = 0;
    if (mc_json_int64(usage, "completion_tokens", &v)) out->completion_tokens = (int)v;
    json_object_put(o);
    return 0;
}

void sewn_completion_free(sewn_completion *c) {
    free(c->text);
    if (c->tool_calls) json_object_put(c->tool_calls);
    memset(c, 0, sizeof *c);
}

/* MARK: - The prose fallback (skillsCompleteParseToolCalls) */

static struct json_object *call_of(struct json_object *object) {
    const char *name = mc_json_string(object, "name");
    if (!name || !*name) return NULL;
    struct json_object *args = NULL, *entry = json_object_new_object();
    json_object_object_get_ex(object, "arguments", &args);
    json_object_object_add(entry, "id", json_object_new_string(""));
    json_object_object_add(entry, "name", json_object_new_string(name));
    if (args && json_object_is_type(args, json_type_string)) json_object_object_add(entry, "arguments", json_object_new_string(json_object_get_string(args)));
    else if (args) json_object_object_add(entry, "arguments", json_object_new_string(json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)));
    else json_object_object_add(entry, "arguments", json_object_new_string("{}"));
    return entry;
}

/* The outermost {...} of `text`, parsed, or NULL. */
static struct json_object *object_in(const char *text, size_t len) {
    const char *start = memchr(text, '{', len);
    if (!start) return NULL;
    const char *end = NULL;
    for (const char *p = text + len; p > start; p--) if (p[-1] == '}') { end = p; break; }
    if (!end) return NULL;
    struct json_object *o = mc_json_parse(start, (size_t)(end - start));
    if (o && !json_object_is_type(o, json_type_object)) {
        json_object_put(o);
        return NULL;
    }
    return o;
}

int sewn_completion_recover_tool_calls(char **text, struct json_object **calls) {
    static const char open[] = "<tool_call>", close[] = "</tool_call>";
    mc_buf stripped = { 0 };
    const char *p = *text;
    int found = 0;
    for (;;) {
        const char *a = strstr(p, open);
        if (!a) break;
        const char *b = strstr(a + sizeof open - 1, close);
        if (!b) break;
        mc_buf_append(&stripped, p, (size_t)(a - p));
        struct json_object *o = object_in(a + sizeof open - 1, (size_t)(b - a - (sizeof open - 1)));
        struct json_object *call = o ? call_of(o) : NULL;
        if (call) {
            if (!*calls) *calls = json_object_new_array();
            json_object_array_add(*calls, call);
            found++;
        }
        json_object_put(o);
        p = b + sizeof close - 1;
    }
    if (found) {
        mc_buf_append_str(&stripped, p);
        /* skillsCompleteTextStrippingTags: trimmed */
        char *s = (char *)stripped.data;
        size_t n = stripped.len;
        while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
        size_t lead = 0;
        while (lead < n && isspace((unsigned char)s[lead])) lead++;
        free(*text);
        *text = strdup(s + lead);
        mc_buf_free(&stripped);
        return found;
    }
    mc_buf_free(&stripped);
    if (!*calls) {
        struct json_object *o = object_in(*text, strlen(*text));
        struct json_object *call = o ? call_of(o) : NULL;
        if (call) {
            *calls = json_object_new_array();
            json_object_array_add(*calls, call);
            found = 1;
        }
        json_object_put(o);
    }
    return found;
}

/* MARK: - The call */

struct collecting {
    mc_buf body;
    long status;
};

static int on_bytes(const char *bytes, size_t len, long status, void *user) {
    struct collecting *c = user;
    c->status = status;
    if (c->body.len + len > SEWN_COMPLETE_BODY_MAX) return 1;
    return mc_buf_append(&c->body, bytes, len) < 0;
}

static bool never_stop(void *user) { return false; }

int sewn_complete(sewn_service *svc, const char *key, const sewn_complete_request *req, sewn_completion *out, char *message, size_t cap) {
    memset(out, 0, sizeof *out);
    if (!sewn_provider_available(req->provider)) {
        snprintf(message, cap, "%s", SEWN_ENGINE_UNAVAILABLE);
        return -ENOTSUP;
    }
    struct json_object *body = sewn_complete_body(req);
    size_t body_len = 0;
    const char *body_text = mc_json_compact(body, &body_len);
    struct collecting c = { { 0 }, 0 };
    long status = 0;
    sewn_outbound o = { req->provider, req->purpose ? req->purpose : "complete", req->request_id };
    int rc = sewn_post(svc, &o, SEWN_MISTRAL_CHAT_PATH, key, body_text, body_len, on_bytes, never_stop, &c, &status, message, cap);
    json_object_put(body);
    out->status = status;
    if (rc < 0 && rc != SEWN_STREAM_STOPPED) {
        mc_buf_free(&c.body);
        if (!message[0]) snprintf(message, cap, "Mistral could not be reached");
        return rc == -ENOSYS ? -ENOSYS : -EIO;
    }
    if (status < 200 || status > 299) {
        sewn_speech_failure(status, (const char *)c.body.data, c.body.len, message, cap);
        mc_buf_free(&c.body);
        return -EIO;
    }
    rc = sewn_completion_parse((const char *)c.body.data, c.body.len, out);
    mc_buf_free(&c.body);
    if (rc < 0) {
        snprintf(message, cap, "Mistral's answer could not be read");
        return -EIO;
    }
    out->status = status;
    return 0;
}

int sewn_complete_text(sewn_service *svc, const char *key, sewn_provider provider, const char *system, const char *prompt,
                       int max_tokens, double temperature, const char *purpose, const char *request_id, char **text, char *message, size_t cap) {
    struct json_object *messages = json_object_new_array();
    json_object_array_add(messages, role_content("user", prompt));
    sewn_complete_request req = { .provider = provider, .model = SEWN_UTILITY_MODEL, .system = system, .messages = messages,
                                  .max_tokens = max_tokens, .temperature = temperature, .top_p = SEWN_DEFAULT_TOP_P,
                                  .purpose = purpose, .request_id = request_id };
    sewn_completion out;
    int rc = sewn_complete(svc, key, &req, &out, message, cap);
    json_object_put(messages);
    if (rc) return rc;
    *text = out.text;
    out.text = NULL;
    sewn_completion_free(&out);
    return 0;
}
