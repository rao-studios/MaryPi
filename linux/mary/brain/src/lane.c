#include "brain/lane.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brain/prompt.h"
#include "common/json.h"
#include "foundation/behavior.h"

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

void mb_lane_result_free(mb_lane_result *r) {
    free(r->text);
    r->text = NULL;
}

const mb_lane_outcome *mb_lane_already_failed(const mb_lane_outcome *outcomes, int n, const char *invocation, const char *args) {
    for (int i = 0; i < n; i++) {
        const mb_lane_outcome *o = &outcomes[i];
        if (strcmp(o->invocation, invocation) == 0 && strcmp(o->arguments, args) == 0 && !o->ok && !o->requested && !o->deferred) return o;
    }
    return NULL;
}

const mb_lane_outcome *mb_lane_already_ran_unproven(const mb_lane_outcome *outcomes, int n, const char *invocation, const char *args) {
    if (!n) return NULL;
    const mb_lane_outcome *last = &outcomes[n - 1];
    if (strcmp(last->invocation, invocation) != 0 || strcmp(last->arguments, args) != 0) return NULL;
    if (!last->ok || last->landed || last->deferred || last->found_nothing || last->asks_the_person || last->is_read) return NULL;
    return last;
}

struct json_object *mb_lane_assistant_message(const char *text, struct json_object *tool_calls) {
    struct json_object *m = json_object_new_object(), *calls = json_object_new_array();
    json_object_object_add(m, "role", json_object_new_string("assistant"));
    json_object_object_add(m, "content", json_object_new_string(text ? text : ""));
    for (size_t i = 0; tool_calls && i < json_object_array_length(tool_calls); i++) {
        struct json_object *call = json_object_array_get_idx(tool_calls, i), *c = json_object_new_object(), *fn = json_object_new_object();
        const char *id = mc_json_string(call, "id"), *name = mc_json_string(call, "name"), *arguments = mc_json_string(call, "arguments");
        json_object_object_add(c, "id", json_object_new_string(id ? id : ""));
        json_object_object_add(c, "type", json_object_new_string("function"));
        json_object_object_add(fn, "name", json_object_new_string(name ? name : ""));
        json_object_object_add(fn, "arguments", json_object_new_string(arguments ? arguments : "{}"));
        json_object_object_add(c, "function", fn);
        json_object_array_add(calls, c);
    }
    json_object_object_add(m, "tool_calls", calls);
    return m;
}

struct json_object *mb_lane_tool_message(const char *call_id, const char *name, const char *content) {
    struct json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string("tool"));
    json_object_object_add(m, "tool_call_id", json_object_new_string(call_id ? call_id : ""));
    json_object_object_add(m, "name", json_object_new_string(name ? name : ""));
    json_object_object_add(m, "content", json_object_new_string(content ? content : ""));
    return m;
}

void mb_lane_result_line(const mb_lane_outcome *o, char *out, size_t n) {
    if (o->requested) snprintf(out, n, "ASKED: %s", o->summary);
    else if (o->ok && o->is_read) snprintf(out, n, "DONE: %s", o->summary);
    else if (o->ok && o->landed) snprintf(out, n, "DONE: %s", o->summary);
    else if (o->ok) snprintf(out, n, "RAN, unproven: %s", o->summary);
    else snprintf(out, n, "FAILED: %s", o->summary);
}

static void set_summary(mb_lane_outcome *o, const char *text) {
    snprintf(o->summary, sizeof o->summary, "%s", text ? text : "");
    for (char *c = o->summary; *c; c++) if (*c == '\n') *c = ' ';
}

/* Whether a JSON result says it landed / found nothing / asks the person; a `summary` sentence in it is the
 * run's word (the receipt maryd speaks when the lane ends without prose), over the raw JSON. */
static void read_result_flags(struct json_object *result, mb_lane_outcome *o) {
    if (!result) return;
    bool b;
    const char *summary = mc_json_string(result, "summary");
    if (summary && *summary && !o->is_read) set_summary(o, summary);   /* a read keeps its JSON: the model answers from it */
    if (mc_json_bool(result, "landed", &b)) o->landed = b;
    if (mc_json_bool(result, "found_nothing", &b)) o->found_nothing = b;
    const char *ask = mc_json_string(result, "ask");
    if (ask && *ask) { o->asks_the_person = true; set_summary(o, ask); }
}

int mb_lane_run(const mb_lane_request *req, const mb_lane_hooks *hooks, mb_lane_result *out) {
    memset(out, 0, sizeof *out);
    out->text = strdup("");
    if (!req->messages || !hooks || !hooks->complete) return -EINVAL;
    int max_rounds = req->max_rounds > 0 ? req->max_rounds : MB_LANE_MAX_ROUNDS;
    struct json_object *history = json_object_new_array();
    for (size_t i = 0; i < json_object_array_length(req->messages); i++) json_object_array_add(history, json_object_get(json_object_array_get_idx(req->messages, i)));
    struct json_object *tools = sk_tools_json(req->registry);
    char system[16384];
    snprintf(system, sizeof system, "%s\n\n%s", req->system ? req->system : "", mb_orchestrator_addendum());
    int rc = 0;
    for (int round = 1; round <= max_rounds; round++) {
        if (hooks->cancelled && hooks->cancelled(hooks->user)) { out->cancelled = true; break; }
        out->rounds = round;
        struct json_object *request = json_object_new_object(), *reply = NULL;
        json_object_object_add(request, "type", json_object_new_string("complete"));
        json_object_object_add(request, "messages", json_object_get(history));
        json_object_object_add(request, "tools", json_object_get(tools));
        json_object_object_add(request, "instructions", json_object_new_string(system));
        if (req->scope) json_object_object_add(request, "scope", json_object_get(req->scope));
        json_object_object_add(request, "provider", json_object_new_string(req->provider ? req->provider : "mistral"));
        json_object_object_add(request, "purpose", json_object_new_string("skills"));
        json_object_object_add(request, "max_tokens", json_object_new_int(MB_LANE_MAX_TOKENS));
        if (req->request_id) json_object_object_add(request, "request_id", json_object_new_string(req->request_id));
        char message[240] = "";
        rc = hooks->complete(request, &reply, message, sizeof message, hooks->user);
        json_object_put(request);
        if (rc < 0 || !reply) {
            snprintf(out->error, sizeof out->error, "%s", message[0] ? message : "sewnd could not complete");
            if (round == 1) { json_object_put(history); json_object_put(tools); return rc < 0 ? rc : -EIO; }
            break;
        }
        const char *text = mc_json_string(reply, "text");
        struct json_object *calls = mc_json_array(reply, "tool_calls");
        size_t n_calls = calls ? json_object_array_length(calls) : 0;
        if (!n_calls) {
            /* NOOP: keep the prose; nudge once when the change asked for has not landed */
            free(out->text);
            out->text = strdup(text ? text : "");
            bool only_read = true;
            for (int i = 0; i < out->outcome_count; i++) only_read = only_read && (out->outcomes[i].is_read || !out->outcomes[i].ok);
            bool landed = false;
            for (int i = 0; i < out->outcome_count; i++) landed = landed || out->outcomes[i].landed;
            if (!out->nudged && req->implies_action && !landed && only_read) {
                out->nudged = true;
                struct json_object *m = json_object_new_object();
                json_object_object_add(m, "role", json_object_new_string("assistant"));
                json_object_object_add(m, "content", json_object_new_string(text ? text : ""));
                json_object_array_add(history, m);
                struct json_object *nudge = json_object_new_object();
                json_object_object_add(nudge, "role", json_object_new_string("user"));
                json_object_object_add(nudge, "content", json_object_new_string(mb_continuation_nudge()));
                json_object_array_add(history, nudge);
                json_object_put(reply);
                continue;
            }
            json_object_put(reply);
            break;
        }
        json_object_array_add(history, mb_lane_assistant_message(text, calls));
        for (size_t i = 0; i < n_calls; i++) {
            struct json_object *call = json_object_array_get_idx(calls, i);
            const char *id = mc_json_string(call, "id"), *name = mc_json_string(call, "name"), *arguments = mc_json_string(call, "arguments");
            char call_id[64];
            if (id && *id) snprintf(call_id, sizeof call_id, "%s", id);
            else snprintf(call_id, sizeof call_id, "call%02d%03d", round % 100, (int)(i % 1000));   /* Mistral wants 9 alphanumerics */
            if (hooks->cancelled && hooks->cancelled(hooks->user)) {
                json_object_array_add(history, mb_lane_tool_message(call_id, name, "(cancelled before running)"));
                out->cancelled = true;
                continue;
            }
            mb_lane_outcome o;
            memset(&o, 0, sizeof o);
            snprintf(o.call_id, sizeof o.call_id, "%s", call_id);
            snprintf(o.invocation, sizeof o.invocation, "%s", name ? name : "");
            char *canonical = mf_canonical_json(arguments ? arguments : "{}");
            snprintf(o.arguments, sizeof o.arguments, "%s", canonical);
            free(canonical);
            o.started = now_seconds();
            const sk_app *app = NULL;
            const sk_skill *skill = NULL;
            char line[512];
            const mb_lane_outcome *prior;
            if ((prior = mb_lane_already_failed(out->outcomes, out->outcome_count, o.invocation, o.arguments))) {
                snprintf(line, sizeof line, "Already tried %s with the same words this turn \xE2\x80\x94 %s", o.invocation, prior->summary);
                json_object_array_add(history, mb_lane_tool_message(call_id, name, line));
                continue;
            }
            if (mb_lane_already_ran_unproven(out->outcomes, out->outcome_count, o.invocation, o.arguments)) {
                snprintf(line, sizeof line, "%s already ran with those words and the change is unproven \xE2\x80\x94 look before running it again", o.invocation);
                json_object_array_add(history, mb_lane_tool_message(call_id, name, line));
                continue;
            }
            if (!sk_tool_lookup(req->registry, name, &app, &skill)) {
                set_summary(&o, "no such skill");
                snprintf(line, sizeof line, "FAILED: %s is not a skill this desktop offers", o.invocation);
                json_object_array_add(history, mb_lane_tool_message(call_id, name, line));
                if (out->outcome_count < MB_LANE_OUTCOMES) out->outcomes[out->outcome_count++] = o;
                continue;
            }
            snprintf(o.app, sizeof o.app, "%s", app->id);
            snprintf(o.skill, sizeof o.skill, "%s", skill->id);
            o.is_read = skill->effect == SK_EFFECT_READ;
            struct json_object *args = mc_json_parse(o.arguments, strlen(o.arguments));
            if (!args || !json_object_is_type(args, json_type_object)) { if (args) json_object_put(args); args = json_object_new_object(); }
            sk_decision decision = sk_registry_decide(req->registry, app->id, skill->id);
            if (decision == SK_DENIED) {
                set_summary(&o, "turned off in Settings");
                snprintf(line, sizeof line, "FAILED: %s is turned off in System Settings \xE2\x80\xBA Mary", skill->title);
            } else if (decision == SK_NEEDS_CONFIRMATION) {
                char ask[300];
                snprintf(ask, sizeof ask, "%s in %s?", skill->title, app->name);
                if (hooks->run_started) { o.requested = true; hooks->run_started(&o, hooks->user); o.requested = false; }
                int answer = hooks->confirm ? hooks->confirm(call_id, app, skill, args, ask, hooks->user) : 0;
                if (answer == 1) {
                    struct json_object *result = NULL;
                    char error[120] = "";
                    int irc = hooks->invoke ? hooks->invoke(app->id, skill->id, args, true, &result, error, sizeof error, hooks->user) : -ENOSYS;
                    o.ok = irc == 0;        /* delivered; `landed` only when the result says so (a receipt) */
                    if (o.ok) { const char *r = result ? mc_json_compact(result, NULL) : "ok"; set_summary(&o, r); read_result_flags(result, &o); }
                    else set_summary(&o, error[0] ? error : strerror(-irc));
                    if (result) json_object_put(result);
                    mb_lane_result_line(&o, line, sizeof line);
                } else {
                    o.requested = true;
                    set_summary(&o, answer == 0 ? "the person declined" : "nobody answered in time");
                    snprintf(line, sizeof line, "ASKED: %s \xE2\x80\x94 %s", ask, o.summary);
                    o.asks_the_person = true;
                }
            } else {
                if (hooks->run_started) hooks->run_started(&o, hooks->user);
                struct json_object *result = NULL;
                char error[120] = "";
                int irc = hooks->invoke ? hooks->invoke(app->id, skill->id, args, false, &result, error, sizeof error, hooks->user) : -ENOSYS;
                o.ok = irc == 0;
                if (o.ok) { const char *r = result ? mc_json_compact(result, NULL) : "ok"; set_summary(&o, r); read_result_flags(result, &o); }
                else set_summary(&o, error[0] ? error : strerror(-irc));
                if (result) json_object_put(result);
                mb_lane_result_line(&o, line, sizeof line);
            }
            json_object_put(args);
            o.finished = now_seconds();
            if (hooks->run_finished) hooks->run_finished(&o, hooks->user);
            json_object_array_add(history, mb_lane_tool_message(call_id, name, line));
            if (out->outcome_count < MB_LANE_OUTCOMES) out->outcomes[out->outcome_count++] = o;
        }
        json_object_put(reply);
        /* an ASKED result ends the lane: the question is the reply */
        bool asked = false;
        for (int i = 0; i < out->outcome_count; i++) asked = asked || (out->outcomes[i].requested && out->outcomes[i].asks_the_person);
        if (asked) break;
    }
    json_object_put(history);
    json_object_put(tools);
    return 0;
}
