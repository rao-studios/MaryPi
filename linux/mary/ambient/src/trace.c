#include "ambient/trace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/buf.h"

struct ma_trace_log {
    ma_trace_record *records;   /* newest first */
    int count, capacity;
    int voice_without_mutation;
};

ma_trace_log *ma_trace_log_new(int capacity) {
    ma_trace_log *log = calloc(1, sizeof *log);
    if (!log) return NULL;
    log->capacity = capacity > 0 ? capacity : MA_TRACE_CAPACITY;
    log->records = calloc((size_t)log->capacity, sizeof *log->records);
    if (!log->records) { free(log); return NULL; }
    return log;
}

void ma_trace_log_free(ma_trace_log *log) {
    if (!log) return;
    free(log->records);
    free(log);
}

void ma_trace_push(ma_trace_log *log, const ma_trace_record *record) {
    int n = log->count < log->capacity ? log->count : log->capacity - 1;
    memmove(&log->records[1], &log->records[0], (size_t)n * sizeof *log->records);
    log->records[0] = *record;
    if (log->count < log->capacity) log->count++;
}

static ma_trace_record *find(ma_trace_log *log, const char *turn_id) {
    if (!turn_id) return NULL;
    for (int i = 0; i < log->count; i++) if (strcmp(log->records[i].id, turn_id) == 0) return &log->records[i];
    return NULL;
}

void ma_trace_note_skill_invocation(ma_trace_log *log, const char *turn_id, const char *app, const char *skill, const char *effect, const char *args, double now) {
    ma_trace_record *r = find(log, turn_id);
    if (!r || r->run_count >= MA_TRACE_RUNS) return;
    ma_skill_run *run = &r->runs[r->run_count++];
    memset(run, 0, sizeof *run);
    snprintf(run->app, sizeof run->app, "%s", app ? app : "");
    snprintf(run->skill, sizeof run->skill, "%s", skill ? skill : "");
    snprintf(run->invocation, sizeof run->invocation, "%s.%s", app ? app : "", skill ? skill : "");
    snprintf(run->status, sizeof run->status, "running");
    snprintf(run->effect, sizeof run->effect, "%s", effect ? effect : "");
    snprintf(run->args, sizeof run->args, "%s", args ? args : "");
    run->started = now;
}

void ma_trace_note_skill_result(ma_trace_log *log, const char *turn_id, const char *app, const char *skill, const char *status, bool found_nothing, const char *result, double now) {
    ma_trace_record *r = find(log, turn_id);
    if (!r) return;
    for (int i = r->run_count - 1; i >= 0; i--) {
        ma_skill_run *run = &r->runs[i];
        if (strcmp(run->status, "running") != 0 || strcmp(run->app, app ? app : "") != 0 || strcmp(run->skill, skill ? skill : "") != 0) continue;
        snprintf(run->status, sizeof run->status, "%s", status ? status : "completed");
        run->finished = now;
        run->found_nothing = found_nothing;
        snprintf(run->result, sizeof run->result, "%s", result ? result : "");
        return;
    }
}

void ma_trace_note_retrieval(ma_trace_log *log, const char *turn_id, const ma_retrieval_purpose *purpose) {
    ma_trace_record *r = find(log, turn_id);
    if (!r) return;
    for (int i = 0; i < r->purpose_count; i++) if (strcmp(r->purposes[i].name, purpose->name) == 0) { r->purposes[i] = *purpose; return; }
    if (r->purpose_count < MA_TRACE_PURPOSES) r->purposes[r->purpose_count++] = *purpose;
}

void ma_trace_note_contribution(ma_trace_log *log, const char *turn_id, const char *summary) {
    ma_trace_record *r = find(log, turn_id);
    if (r) snprintf(r->contribution, sizeof r->contribution, "%s", summary ? summary : "");
}

void ma_trace_note_confirmation(ma_trace_log *log, const char *turn_id, const char *state) {
    ma_trace_record *r = find(log, turn_id);
    if (r) snprintf(r->confirmation, sizeof r->confirmation, "%s", state ? state : "");
}

int ma_trace_count(const ma_trace_log *log) { return log->count; }
const ma_trace_record *ma_trace_entry(const ma_trace_log *log, int index) { return index >= 0 && index < log->count ? &log->records[index] : NULL; }
const ma_trace_record *ma_trace_find(const ma_trace_log *log, const char *turn_id) { return find((ma_trace_log *)log, turn_id); }
void ma_trace_clear(ma_trace_log *log) { log->count = 0; }
void ma_trace_note_voice_without_mutation(ma_trace_log *log) { log->voice_without_mutation++; }
int ma_trace_voice_without_mutation(const ma_trace_log *log) { return log->voice_without_mutation; }

/* ---- the report ---- */

void ma_trace_age_string(double seconds, char *out, size_t n) {
    if (!isfinite(seconds) || seconds < 0) seconds = 0;
    if (seconds < 60) snprintf(out, n, "%.1fs", seconds);
    else if (seconds < 3600) snprintf(out, n, "%dm %ds", (int)(seconds / 60), (int)fmod(seconds, 60));
    else snprintf(out, n, "%dh %dm", (int)(seconds / 3600), (int)(fmod(seconds, 3600) / 60));
}

static void timestamp(double seconds, char *out, size_t n) {
    time_t t = (time_t)seconds;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int line(mc_buf *b, const char *key, const char *value) {
    int rc = mc_buf_append_str(b, key);
    rc |= mc_buf_append_str(b, ": ");
    rc |= mc_buf_append_str(b, value && *value ? value : "none");
    return rc | mc_buf_append_str(b, "\n");
}

static int line_int(mc_buf *b, const char *key, long value) {
    char text[32];
    snprintf(text, sizeof text, "%ld", value);
    return line(b, key, text);
}

static const char *yes_no(bool v) { return v ? "yes" : "no"; }

static void one_line(const char *text, char *out, size_t n) {
    snprintf(out, n, "%s", text);
    for (char *c = out; *c; c++) if (*c == '\n') *c = ' ';
}

static void places_list(const ma_place *places, int n, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        char token[MA_TOKEN_MAX];
        ma_place_token(&places[i], token, sizeof token);
        if (i) strncat(out, ", ", cap - strlen(out) - 1);
        strncat(out, token, cap - strlen(out) - 1);
    }
}

int ma_trace_report(const ma_trace_log *log, const ma_roster *roster, double now, mc_buf *b) {
    char stamp[32], text[1024];
    timestamp(now, stamp, sizeof stamp);
    int rc = mc_buf_append_str(b, "=== Mary ABILITY ROUTES ");
    rc |= mc_buf_append_str(b, stamp);
    rc |= mc_buf_append_str(b, " ===\n");
    rc |= line_int(b, "turns", log->count);
    long runs = 0, blocked = 0, intents[MA_INTENT_COUNT] = { 0 };
    for (int i = 0; i < log->count; i++) {
        runs += log->records[i].run_count;
        for (int k = 0; k < log->records[i].run_count; k++) blocked += strcmp(log->records[i].runs[k].status, "blocked") == 0;
        intents[log->records[i].route.intent]++;
    }
    rc |= line_int(b, "ability.runs", runs);
    rc |= line_int(b, "ability.blocked", blocked);
    rc |= line_int(b, "registry.revisions", log->count ? 1 : 0);
    for (int i = 0; i < MA_INTENT_COUNT; i++) {
        if (!intents[i]) continue;
        snprintf(text, sizeof text, "intent.%s", ma_intent_name((ma_intent)i));
        rc |= line_int(b, text, intents[i]);
    }
    for (int i = 0; i < log->count; i++) {
        const ma_trace_record *r = &log->records[i];
        const ma_route *route = &r->route;
        rc |= mc_buf_append_str(b, "\n--- ");
        rc |= mc_buf_append_str(b, ma_intent_name(route->intent));
        rc |= mc_buf_append_str(b, " via ");
        rc |= mc_buf_append_str(b, ma_signal_name(route->decided_by));
        rc |= mc_buf_append_str(b, " ---\n");
        char age[32];
        ma_trace_age_string(now - r->date, age, sizeof age);
        rc |= line(b, "age", age);
        one_line(r->utterance, text, sizeof text);
        rc |= line(b, "utterance", text);
        ma_place lead;
        if (ma_route_lead_place(route, &lead)) {
            char token[MA_TOKEN_MAX];
            ma_place_token(&lead, token, sizeof token);
            rc |= line(b, "lead", token);
            rc |= line(b, "lead.class", ma_place_class_name(&lead, roster));
        } else {
            rc |= line(b, "lead", "none");
            rc |= line(b, "lead.class", "none");
        }
        places_list(route->named_places, route->named_count, text, sizeof text);
        rc |= line(b, "named", text);
        text[0] = 0;
        for (int a = 0; a < MA_ATTENTION_COUNT; a++) {
            if (!(route->candidate_attentions & (1u << a))) continue;
            if (text[0]) strncat(text, ", ", sizeof text - strlen(text) - 1);
            strncat(text, ma_attention_name((ma_attention)a), sizeof text - strlen(text) - 1);
        }
        rc |= line(b, "world.candidates", text);
        rc |= line(b, "ranking", ma_ranking_mode_name(route->ranking_mode));
        if (route->has_world) {
            snprintf(text, sizeof text, "%s@%s%s%s", ma_sense_name(route->world.sense), ma_attention_name(route->world.attention),
                     route->world.subject[0] ? "#" : "", route->world.subject);
            rc |= line(b, "attention", text);
        }
        rc |= line(b, "writing.target", ma_writing_target_name(route->writing_target));
        one_line(route->supporting_context, text, sizeof text);
        rc |= line(b, "writing.context", text);
        text[0] = 0;
        for (unsigned q = 0; q < 7; q++) {
            if (!(route->gate.questions & (1u << q))) continue;
            if (text[0]) strncat(text, ", ", sizeof text - strlen(text) - 1);
            strncat(text, ma_question_name(1u << q), sizeof text - strlen(text) - 1);
        }
        rc |= line(b, "questions", text);
        text[0] = 0;
        for (int k = 0; k < route->gate.requested_count; k++) { if (k) strncat(text, ", ", sizeof text - strlen(text) - 1); strncat(text, route->gate.requested_abilities[k], sizeof text - strlen(text) - 1); }
        rc |= line(b, "abilities", text);
        text[0] = 0;
        for (int k = 0; k < route->gate.application_count; k++) { if (k) strncat(text, ", ", sizeof text - strlen(text) - 1); strncat(text, route->gate.applications[k], sizeof text - strlen(text) - 1); }
        rc |= line(b, "applications", text);
        text[0] = 0;
        if (route->gate.memory.lanes & MA_LANE_ABILITY) strncat(text, "ability", sizeof text - strlen(text) - 1);
        if (route->gate.memory.lanes & MA_LANE_PERSONAL) { if (text[0]) strncat(text, ", ", sizeof text - strlen(text) - 1); strncat(text, "personal", sizeof text - strlen(text) - 1); }
        rc |= line(b, "threads", text);
        rc |= line(b, "needs.locate", yes_no(route->needs_locate));
        rc |= line(b, "needs.pre-read", yes_no(route->needs_pre_read));
        rc |= line(b, "needs.execution", yes_no(route->needs_execution));
        rc |= line_int(b, "prompt.chars", r->system_prompt_chars);
        rc |= line(b, "registry.revision", "maryos");
        text[0] = 0;
        for (int k = 0; k < r->package_count; k++) { if (k) strncat(text, ", ", sizeof text - strlen(text) - 1); strncat(text, r->packages[k], sizeof text - strlen(text) - 1); }
        rc |= line(b, "registry.packages", text);
        rc |= line_int(b, "skills.exposed", r->exposed_skill_count);
        text[0] = 0;
        for (int k = 0; k < r->run_count; k++) { if (k) strncat(text, ", ", sizeof text - strlen(text) - 1); strncat(text, r->runs[k].invocation, sizeof text - strlen(text) - 1); }
        rc |= line(b, "skills.invoked", text);
        for (int k = 0; k < r->run_count; k++) {
            char key[160], value[64];
            snprintf(key, sizeof key, "skill.%s", r->runs[k].invocation);
            snprintf(value, sizeof value, "%s | %s", r->runs[k].status, r->runs[k].effect);
            rc |= line(b, key, value);
            if (r->runs[k].found_nothing) { strncat(key, ".outcome", sizeof key - strlen(key) - 1); rc |= line(b, key, "no-match"); }
        }
        for (int k = 0; k < r->purpose_count; k++) {
            char key[64], value[MA_TRACE_RETURNED * 140];
            snprintf(key, sizeof key, "retrieval.%s.lanes", r->purposes[k].name);
            value[0] = 0;
            for (int l = 0; l < r->purposes[k].lane_count; l++) { if (l) strncat(value, ", ", sizeof value - strlen(value) - 1); strncat(value, r->purposes[k].lanes[l], sizeof value - strlen(value) - 1); }
            rc |= line(b, key, value);
            snprintf(key, sizeof key, "retrieval.%s.returned", r->purposes[k].name);
            value[0] = 0;
            for (int l = 0; l < r->purposes[k].returned_count; l++) {
                char item[160];
                snprintf(item, sizeof item, "%s=%.2f", r->purposes[k].returned[l].document_id, r->purposes[k].returned[l].score);
                if (l) strncat(value, ", ", sizeof value - strlen(value) - 1);
                strncat(value, item, sizeof value - strlen(value) - 1);
            }
            rc |= line(b, key, value);
        }
        const ma_verdicts *v = &route->verdicts;
        rc |= line(b, "verdict.effectful-turn", yes_no(v->action_turn));
        rc |= line(b, "verdict.edit", v->has_edit ? ma_edit_shape_name(v->edit.shape) : "none");
        text[0] = 0;
        for (int k = 0; v->has_edit && k < v->edit.target_count; k++) { if (k) strncat(text, ", ", sizeof text - strlen(text) - 1); strncat(text, v->edit.target[k], sizeof text - strlen(text) - 1); }
        rc |= line(b, "verdict.edit-targets", text);
        one_line(v->named_part, text, sizeof text);
        rc |= line(b, "verdict.named-part", text);
        rc |= line(b, "verdict.ambient-source", yes_no(v->names_ambient_source));
        rc |= line(b, "verdict.deictic", yes_no(v->is_deictic));
        rc |= line(b, "verdict.transform", yes_no(v->names_transform));
        rc |= line(b, "verdict.override", v->focus_override);
        rc |= line(b, "verdict.bare-decision", v->bare_decision < 0 ? "none" : yes_no(v->bare_decision == 1));
    }
    return rc ? -1 : 0;
}
