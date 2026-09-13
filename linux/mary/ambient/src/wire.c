#include "ambient/wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "ambient/render.h"

static double ms_or(struct json_object *o, const char *key, double fallback_seconds) {
    double ms;
    return mc_json_double(o, key, &ms) ? ms / 1000.0 : fallback_seconds;
}

static void element_parse(struct json_object *o, ma_element *e) {
    memset(e, 0, sizeof *e);
    const char *identity = mc_json_string(o, "identity"), *role = mc_json_string(o, "role"), *kind = mc_json_string(o, "kind"), *label = mc_json_string(o, "label");
    int64_t ordinal = 0;
    mc_json_int64(o, "ordinal", &ordinal);
    e->ordinal = (int)ordinal;
    snprintf(e->role, sizeof e->role, "%s", role ? role : "");
    snprintf(e->kind, sizeof e->kind, "%s", kind ? kind : "");
    if (label) { size_t i = 0, points = 0; while (label[i] && points < MA_LABEL_CAP) { i++; while ((label[i] & 0xC0) == 0x80) i++; points++; } memcpy(e->label, label, i); }
    if (identity) snprintf(e->identity, sizeof e->identity, "%s", identity);
    else snprintf(e->identity, sizeof e->identity, "%s|%s", e->role, e->label);
    for (char *c = e->identity; *c && c < e->identity + strlen(e->role); c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    bool b;
    e->focused = mc_json_bool(o, "focused", &b) && b;
    e->enabled = !mc_json_bool(o, "enabled", &b) || b;
}

int ma_surface_parse(struct json_object *entry, double now, ma_surface *out) {
    if (!entry) return -EINVAL;
    const char *token = mc_json_string(entry, "place");
    ma_place place;
    if (!token || !ma_place_from_token(token, &place)) return -EINVAL;
    struct json_object *surface = mc_json_object(entry, "surface");
    if (!surface) surface = entry;
    struct json_object *app = mc_json_object(surface, "application");
    const char *name = app ? mc_json_string(app, "name") : NULL, *id = app ? mc_json_string(app, "id") : NULL;
    ma_surface_init(out, &place, name ? name : ma_place_memory_token(&place), id ? id : ma_place_memory_token(&place), ms_or(entry, "capturedAt", now));
    int64_t pid = 0;
    if (app && mc_json_int64(app, "pid", &pid)) out->pid = (int)pid;
    struct json_object *window = mc_json_object(surface, "activeWindow");
    if (window) { out->has_window = true; const char *title = mc_json_string(window, "title"); snprintf(out->window_title, sizeof out->window_title, "%s", title ? title : ""); }
    int64_t n = 0;
    if (mc_json_int64(surface, "windowCount", &n)) out->window_count = (int)n;
    if (mc_json_int64(surface, "minimizedCount", &n)) out->minimized_count = (int)n;
    struct json_object *elements = mc_json_array(surface, "elements");
    if (elements) {
        size_t count = json_object_array_length(elements);
        for (size_t i = 0; i < count && out->element_count < MA_ELEMENT_CAP; i++) {
            ma_element *e = &out->elements[out->element_count];
            element_parse(json_object_array_get_idx(elements, i), e);
            if (!e->ordinal) e->ordinal = out->element_count;
            out->element_count++;
            if (e->focused && !out->has_focused) { out->has_focused = true; out->focused = *e; }
        }
    }
    struct json_object *focused = mc_json_object(surface, "focused");
    if (focused) { out->has_focused = true; element_parse(focused, &out->focused); }
    bool b;
    out->page_not_yet_read = mc_json_bool(surface, "pageNotYetRead", &b) && b;
    struct json_object *document = mc_json_object(surface, "document");
    const char *path = document ? mc_json_string(document, "path") : NULL;
    if (path) snprintf(out->document_path, sizeof out->document_path, "%s", path);
    double fresh;
    if (mc_json_double(surface, "freshFor", &fresh) && fresh > 0) out->fresh_for = fresh;
    return 0;
}

int ma_surface_document_fact(struct json_object *entry, const ma_surface *surface, ma_fact *out) {
    struct json_object *s = mc_json_object(entry, "surface");
    struct json_object *document = mc_json_object(s ? s : entry, "document");
    if (!document) return 0;
    const char *text = mc_json_string(document, "text"), *name = mc_json_string(document, "name"), *path = mc_json_string(document, "path");
    if (!text || !*text) return 0;
    ma_slot slot = ma_slot_of(MA_SLOT_FILE);
    ma_fact_init(out, &surface->place, &slot, text, MA_PROVENANCE_LIVE_AX, surface->captured_at);
    snprintf(out->subject, sizeof out->subject, "%s", name ? name : path ? path : "");
    snprintf(out->application_id, sizeof out->application_id, "%s", surface->app_id);
    int64_t v;
    if (mc_json_int64(document, "total", &v)) out->document_total = (int)v;
    if (mc_json_int64(document, "lower", &v)) { out->lower = (int)v; out->has_bounds = true; }
    if (mc_json_int64(document, "upper", &v)) out->upper = (int)v;
    if (out->has_bounds && out->upper <= out->lower) out->has_bounds = false;
    /* The surface's window is what it was seen through: fresh as long as the surface is. */
    out->fresh_for = surface->fresh_for;
    return 1;
}

int ma_selection_parse(struct json_object *msg, double now, ma_selection *out) {
    if (!msg) return -EINVAL;
    const char *text = mc_json_string(msg, "text"), *token = mc_json_string(msg, "place"), *app = mc_json_string(msg, "applicationID");
    if (!text || !*text) return -EINVAL;
    memset(out, 0, sizeof *out);
    if (!token || !ma_place_from_token(token, &out->place)) {
        if (!app) return -EINVAL;
        out->place = ma_place_application(app);
    }
    snprintf(out->application_id, sizeof out->application_id, "%s", app ? app : ma_place_memory_token(&out->place));
    const char *id = mc_json_string(msg, "id");
    if (id) snprintf(out->id, sizeof out->id, "%s", id);
    snprintf(out->text, sizeof out->text, "%s", text);
    const char *surrounding = mc_json_string(msg, "surrounding"), *subject = mc_json_string(msg, "subject");
    if (!subject) subject = mc_json_string(msg, "document");
    if (surrounding) snprintf(out->surrounding, sizeof out->surrounding, "%s", surrounding);
    if (subject) snprintf(out->subject, sizeof out->subject, "%s", subject);
    int64_t v;
    if (mc_json_int64(msg, "lower", &v)) { out->lower = (int)v; out->has_range = true; }
    if (mc_json_int64(msg, "upper", &v)) out->upper = (int)v;
    if (out->has_range && out->upper <= out->lower) out->has_range = false;
    if (mc_json_int64(msg, "total", &v)) out->document_total = (int)v;
    bool b;
    out->editable = mc_json_bool(msg, "editable", &b) && b;
    out->captured_at = ms_or(msg, "capturedAt", now);
    return 0;
}

/* ---- out ---- */

static struct json_object *str(const char *s) { return json_object_new_string(s ? s : ""); }
static struct json_object *ms(double seconds) { return json_object_new_int64((int64_t)(seconds * 1000.0)); }

struct json_object *ma_place_json(const ma_place *p, const ma_roster *r) {
    struct json_object *o = json_object_new_object();
    char token[MA_TOKEN_MAX];
    ma_place_token(p, token, sizeof token);
    json_object_object_add(o, "token", str(token));
    json_object_object_add(o, "memoryToken", str(ma_place_memory_token(p)));
    json_object_object_add(o, "attention", str(ma_attention_name(p->attention)));
    if (ma_place_is_application(p)) json_object_object_add(o, "application", str(p->application));
    json_object_object_add(o, "name", str(ma_place_display_name(p, r)));
    json_object_object_add(o, "class", str(ma_place_class_name(p, r)));
    json_object_object_add(o, "hasEyes", json_object_new_boolean(ma_place_has_eyes(p, r)));
    const char *focus = ma_place_focus(p, r);
    if (focus) json_object_object_add(o, "focus", str(focus));
    json_object_object_add(o, "order", json_object_new_int(ma_place_order(p, r)));
    return o;
}

static struct json_object *element_json(const ma_element *e) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "identity", str(e->identity));
    json_object_object_add(o, "ordinal", json_object_new_int(e->ordinal));
    json_object_object_add(o, "role", str(e->role));
    json_object_object_add(o, "kind", str(e->kind));
    json_object_object_add(o, "label", str(e->label));
    json_object_object_add(o, "focused", json_object_new_boolean(e->focused));
    json_object_object_add(o, "enabled", json_object_new_boolean(e->enabled));
    return o;
}

struct json_object *ma_surface_json(const ma_surface *s, double now, const ma_roster *r) {
    struct json_object *o = json_object_new_object(), *app = json_object_new_object();
    json_object_object_add(o, "place", ma_place_json(&s->place, r));
    json_object_object_add(app, "name", str(s->app_name));
    json_object_object_add(app, "id", str(s->app_id));
    json_object_object_add(app, "pid", json_object_new_int(s->pid));
    json_object_object_add(o, "application", app);
    if (s->has_window) { struct json_object *w = json_object_new_object(); json_object_object_add(w, "title", str(s->window_title)); json_object_object_add(o, "activeWindow", w); }
    json_object_object_add(o, "windowCount", json_object_new_int(s->window_count));
    json_object_object_add(o, "minimizedCount", json_object_new_int(s->minimized_count));
    struct json_object *elements = json_object_new_array();
    for (int i = 0; i < s->element_count; i++) json_object_array_add(elements, element_json(&s->elements[i]));
    json_object_object_add(o, "elements", elements);
    if (s->has_focused) json_object_object_add(o, "focused", element_json(&s->focused));
    json_object_object_add(o, "pageNotYetRead", json_object_new_boolean(s->page_not_yet_read));
    if (s->document_path[0]) json_object_object_add(o, "document", str(s->document_path));
    json_object_object_add(o, "capturedAt", ms(s->captured_at));
    json_object_object_add(o, "age", json_object_new_double(ma_surface_age(s, now)));
    json_object_object_add(o, "fresh", json_object_new_boolean(ma_surface_is_fresh(s, now)));
    char line[MA_SURFACE_LINE_MAX];
    ma_surface_line(s, now, line, sizeof line);
    json_object_object_add(o, "surfaceLine", str(line));
    return o;
}

struct json_object *ma_fact_json(const ma_fact *f, double now, const ma_roster *r) {
    struct json_object *o = json_object_new_object();
    char key[MA_KEY_MAX], slot[2 * MA_PHRASE_MAX + 8], line[MA_MENTION_MAX];
    ma_fact_key(f, key, sizeof key);
    ma_slot_token(&f->slot, slot, sizeof slot);
    json_object_object_add(o, "key", str(key));
    json_object_object_add(o, "place", ma_place_json(&f->place, r));
    json_object_object_add(o, "slot", str(slot));
    json_object_object_add(o, "content", str(f->content));
    if (f->subject[0]) json_object_object_add(o, "subject", str(f->subject));
    if (f->has_bounds) { json_object_object_add(o, "lower", json_object_new_int(f->lower)); json_object_object_add(o, "upper", json_object_new_int(f->upper)); }
    if (f->document_total) json_object_object_add(o, "total", json_object_new_int(f->document_total));
    json_object_object_add(o, "provenance", str(ma_provenance_name(f->provenance)));
    json_object_object_add(o, "registration", str(ma_registered_name(f->registration)));
    json_object_object_add(o, "capturedAt", ms(f->captured_at));
    json_object_object_add(o, "age", json_object_new_double(ma_fact_age(f, now)));
    json_object_object_add(o, "fresh", json_object_new_boolean(ma_fact_is_fresh(f, now)));
    if (f->spoken_at > 0) json_object_object_add(o, "spokenNote", str(f->spoken_note));
    ma_fact_mention_line(f, now, r, line, sizeof line);
    json_object_object_add(o, "mention", str(line));
    return o;
}

struct json_object *ma_world_json(const ma_world *w, double now) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "sense", str(ma_sense_name(w->sense)));
    json_object_object_add(o, "attention", str(ma_attention_name(w->attention)));
    if (w->subject[0]) json_object_object_add(o, "subject", str(w->subject));
    if (w->application_id[0]) json_object_object_add(o, "applicationID", str(w->application_id));
    if (w->selected_text[0]) json_object_object_add(o, "selectedText", str(w->selected_text));
    json_object_object_add(o, "capturedAt", ms(w->captured_at));
    json_object_object_add(o, "fresh", json_object_new_boolean(ma_world_is_fresh(w, now)));
    return o;
}

static struct json_object *strings(const char (*items)[32], int n) {
    struct json_object *a = json_object_new_array();
    for (int i = 0; i < n; i++) json_object_array_add(a, str(items[i]));
    return a;
}

struct json_object *ma_rendering_json(const ma_rendering *rendering) {
    struct json_object *o = json_object_new_object(), *a;
    json_object_object_add(o, "mode", str(ma_ranking_mode_name(rendering->mode)));
    a = json_object_new_array();
    for (int i = 0; i < rendering->surface_count; i++) json_object_array_add(a, str(rendering->surface_lines[i]));
    json_object_object_add(o, "surfaceLines", a);
    a = json_object_new_array();
    for (int i = 0; i < rendering->block_count; i++) json_object_array_add(a, str(rendering->blocks[i]));
    json_object_object_add(o, "blocks", a);
    a = json_object_new_array();
    for (int i = 0; i < rendering->mention_count; i++) json_object_array_add(a, str(rendering->mentions[i]));
    json_object_object_add(o, "mentions", a);
    a = json_object_new_array();
    for (int i = 0; i < rendering->key_count; i++) json_object_array_add(a, str(rendering->keys[i]));
    json_object_object_add(o, "keys", a);
    return o;
}

static struct json_object *places_json(const ma_place *places, int n, const ma_roster *r) {
    struct json_object *a = json_object_new_array();
    for (int i = 0; i < n; i++) json_object_array_add(a, ma_place_json(&places[i], r));
    return a;
}

struct json_object *ma_route_json(const ma_route *route, const ma_roster *r) {
    struct json_object *o = json_object_new_object(), *v = json_object_new_object(), *g = json_object_new_object(), *m = json_object_new_object(), *a;
    json_object_object_add(o, "intent", str(ma_intent_name(route->intent)));
    json_object_object_add(o, "decidedBy", str(ma_signal_name(route->decided_by)));
    const ma_verdicts *vd = &route->verdicts;
    json_object_object_add(v, "actionTurn", json_object_new_boolean(vd->action_turn));
    if (vd->has_edit) {
        struct json_object *e = json_object_new_object();
        json_object_object_add(e, "shape", str(ma_edit_shape_name(vd->edit.shape)));
        a = json_object_new_array();
        for (int i = 0; i < vd->edit.target_count; i++) json_object_array_add(a, str(vd->edit.target[i]));
        json_object_object_add(e, "target", a);
        if (vd->edit.payload[0]) json_object_object_add(e, "payload", str(vd->edit.payload));
        if (vd->edit.anchor != MA_ANCHOR_NONE) json_object_object_add(e, "anchor", str(ma_edit_anchor_name(vd->edit.anchor)));
        a = json_object_new_array();
        for (int i = 0; i < vd->edit.destination_count; i++) json_object_array_add(a, str(vd->edit.destination[i]));
        json_object_object_add(e, "destination", a);
        json_object_object_add(e, "isAnaphoric", json_object_new_boolean(vd->edit.anaphoric));
        json_object_object_add(v, "editIntent", e);
    }
    if (vd->named_part[0]) json_object_object_add(v, "namedPart", str(vd->named_part));
    json_object_object_add(v, "namesAmbientSource", json_object_new_boolean(vd->names_ambient_source));
    json_object_object_add(v, "isDeictic", json_object_new_boolean(vd->is_deictic));
    json_object_object_add(v, "namesTransform", json_object_new_boolean(vd->names_transform));
    if (vd->focus_override[0]) json_object_object_add(v, "focusOverride", str(vd->focus_override));
    if (vd->bare_decision >= 0) json_object_object_add(v, "bareDecision", json_object_new_boolean(vd->bare_decision == 1));
    json_object_object_add(o, "verdicts", v);
    a = json_object_new_array();
    for (unsigned q = 0; q < 7; q++) if (route->gate.questions & (1u << q)) json_object_array_add(a, str(ma_question_name(1u << q)));
    json_object_object_add(g, "questions", a);
    json_object_object_add(g, "requestedAbilities", strings(route->gate.requested_abilities, route->gate.requested_count));
    a = json_object_new_array();
    for (int i = 0; i < route->gate.application_count; i++) json_object_array_add(a, str(route->gate.applications[i]));
    json_object_object_add(g, "applications", a);
    const ma_memory_plan *plan = &route->gate.memory;
    a = json_object_new_array();
    if (plan->lanes & MA_LANE_ABILITY) json_object_array_add(a, str("ability"));
    if (plan->lanes & MA_LANE_PERSONAL) json_object_array_add(a, str("personal"));
    json_object_object_add(m, "lanes", a);
    a = json_object_new_array();
    for (int i = 0; i < plan->target_count; i++) {
        struct json_object *t = json_object_new_object();
        json_object_object_add(t, "abilityID", str(plan->targets[i].ability_id));
        json_object_object_add(t, "paradigm", str(ma_paradigm_name(plan->targets[i].paradigm)));
        json_object_array_add(a, t);
    }
    json_object_object_add(m, "abilityTargets", a);
    a = json_object_new_array();
    for (int i = 0; i < plan->priority_count; i++) json_object_array_add(a, str(ma_lane_name(plan->lane_priority[i])));
    json_object_object_add(m, "lanePriority", a);
    json_object_object_add(m, "relationshipHints", strings(plan->relationship_hints, plan->hint_count));
    json_object_object_add(m, "expandDisciplineUsage", json_object_new_boolean(plan->expand_discipline_usage));
    a = json_object_new_array();
    for (int l = 0; l < 2; l++) {
        ma_lane lane = l == 0 ? MA_LANE_ABILITY : MA_LANE_PERSONAL;
        if (!(plan->lanes & lane)) continue;
        const char *storage[2];
        int n = ma_lane_storage_lanes(lane, storage, 2);
        for (int i = 0; i < n; i++) json_object_array_add(a, str(storage[i]));
    }
    json_object_object_add(m, "storageLanes", a);
    json_object_object_add(g, "memory", m);
    json_object_object_add(g, "semanticProjection", str(route->gate.signature.semantic_projection));
    json_object_object_add(o, "gate", g);
    if (route->has_world) json_object_object_add(o, "world", ma_world_json(&route->world, route->world.captured_at));
    json_object_object_add(o, "selectionDefinesTurn", json_object_new_boolean(route->selection_defines_turn));
    ma_place lead;
    if (ma_route_lead_place(route, &lead)) {
        json_object_object_add(o, "leadApplicationID", str(route->lead_application_id));
        json_object_object_add(o, "leadPlace", ma_place_json(&lead, r));
    }
    json_object_object_add(o, "namedPlaces", places_json(route->named_places, route->named_count, r));
    struct json_object *realm = json_object_new_object(), *need = json_object_new_object();
    json_object_object_add(need, "abilities", strings(route->realm.need.abilities, route->realm.need.ability_count));
    if (route->realm.need.discipline[0]) json_object_object_add(need, "discipline", str(route->realm.need.discipline));
    json_object_object_add(realm, "need", need);
    a = json_object_new_array();
    for (int i = 0; i < route->realm.candidate_count; i++) {
        const ma_candidate *c = &route->realm.candidates[i];
        struct json_object *cj = json_object_new_object();
        json_object_object_add(cj, "place", ma_place_json(&c->place, r));
        json_object_object_add(cj, "conformsByAbilities", strings(c->conforms_by_abilities, c->conforms_count));
        json_object_object_add(cj, "conformsByDiscipline", json_object_new_boolean(c->conforms_by_discipline));
        json_object_object_add(cj, "targetClasses", strings(c->target_classes, c->target_class_count));
        json_object_object_add(cj, "hasEyes", json_object_new_boolean(c->has_eyes));
        if (c->has_evidence) { json_object_object_add(cj, "evidence", str(ma_evidence_name(c->evidence))); json_object_object_add(cj, "evidenceAgeSeconds", json_object_new_double(c->evidence_age)); }
        json_object_object_add(cj, "conforms", json_object_new_boolean(ma_candidate_conforms(c)));
        json_object_array_add(a, cj);
    }
    json_object_object_add(realm, "candidates", a);
    if (route->realm.has_place) json_object_object_add(realm, "place", ma_place_json(&route->realm.place, r));
    json_object_object_add(realm, "decidedBy", str(ma_signal_name(route->realm.decided_by)));
    json_object_object_add(o, "realm", realm);
    a = json_object_new_array();
    for (int at = 0; at < MA_ATTENTION_COUNT; at++) if (route->candidate_attentions & (1u << at)) json_object_array_add(a, str(ma_attention_name((ma_attention)at)));
    json_object_object_add(o, "candidateAttentions", a);
    if (route->writing_target != MA_WRITING_NONE) json_object_object_add(o, "writingTarget", str(ma_writing_target_name(route->writing_target)));
    if (route->supporting_context[0]) json_object_object_add(o, "supportingContext", str(route->supporting_context));
    json_object_object_add(o, "needsLocate", json_object_new_boolean(route->needs_locate));
    json_object_object_add(o, "needsPreRead", json_object_new_boolean(route->needs_pre_read));
    json_object_object_add(o, "needsExecution", json_object_new_boolean(route->needs_execution));
    json_object_object_add(o, "rankingMode", str(ma_ranking_mode_name(route->ranking_mode)));
    json_object_object_add(o, "isActionTurn", json_object_new_boolean(ma_route_is_action_turn(route)));
    return o;
}

struct json_object *ma_trace_record_json(const ma_trace_record *rec, const ma_roster *r, double now) {
    struct json_object *o = json_object_new_object(), *a;
    json_object_object_add(o, "id", str(rec->id));
    json_object_object_add(o, "date", ms(rec->date));
    json_object_object_add(o, "age", json_object_new_double(now - rec->date));
    json_object_object_add(o, "utterance", str(rec->utterance));
    json_object_object_add(o, "route", ma_route_json(&rec->route, r));
    json_object_object_add(o, "systemPromptChars", json_object_new_int(rec->system_prompt_chars));
    json_object_object_add(o, "exposedSkillCount", json_object_new_int(rec->exposed_skill_count));
    a = json_object_new_array();
    for (int i = 0; i < rec->package_count; i++) json_object_array_add(a, str(rec->packages[i]));
    json_object_object_add(o, "packageIDs", a);
    a = json_object_new_array();
    for (int i = 0; i < rec->run_count; i++) {
        const ma_skill_run *run = &rec->runs[i];
        struct json_object *rj = json_object_new_object();
        json_object_object_add(rj, "app", str(run->app));
        json_object_object_add(rj, "skill", str(run->skill));
        json_object_object_add(rj, "invocation", str(run->invocation));
        json_object_object_add(rj, "status", str(run->status));
        json_object_object_add(rj, "effect", str(run->effect));
        json_object_object_add(rj, "started", ms(run->started));
        if (run->finished > 0) json_object_object_add(rj, "finished", ms(run->finished));
        json_object_object_add(rj, "foundNothing", json_object_new_boolean(run->found_nothing));
        json_object_object_add(rj, "args", str(run->args));
        json_object_object_add(rj, "result", str(run->result));
        json_object_array_add(a, rj);
    }
    json_object_object_add(o, "skillRuns", a);
    json_object_object_add(o, "coActivePlaces", places_json(rec->co_active, rec->co_active_count, r));
    json_object_object_add(o, "glancedPlaces", places_json(rec->glanced, rec->glanced_count, r));
    a = json_object_new_array();
    for (int i = 0; i < rec->purpose_count; i++) {
        const ma_retrieval_purpose *p = &rec->purposes[i];
        struct json_object *pj = json_object_new_object(), *lanes = json_object_new_array(), *returned = json_object_new_array();
        json_object_object_add(pj, "name", str(p->name));
        for (int l = 0; l < p->lane_count; l++) json_object_array_add(lanes, str(p->lanes[l]));
        json_object_object_add(pj, "lanes", lanes);
        for (int l = 0; l < p->returned_count; l++) {
            struct json_object *d = json_object_new_object();
            json_object_object_add(d, "document_id", str(p->returned[l].document_id));
            json_object_object_add(d, "group_id", str(p->returned[l].group_id));
            json_object_object_add(d, "family", str(p->returned[l].family));
            json_object_object_add(d, "lane", str(p->returned[l].lane));
            json_object_object_add(d, "score", json_object_new_double(p->returned[l].score));
            json_object_array_add(returned, d);
        }
        json_object_object_add(pj, "returned", returned);
        if (p->warning[0]) json_object_object_add(pj, "warning", str(p->warning));
        json_object_array_add(a, pj);
    }
    json_object_object_add(o, "retrieval", a);
    if (rec->contribution[0]) json_object_object_add(o, "contribution", str(rec->contribution));
    if (rec->confirmation[0]) json_object_object_add(o, "confirmation", str(rec->confirmation));
    return o;
}

struct json_object *ma_trace_json(const ma_trace_log *log, const ma_roster *r, double now) {
    struct json_object *a = json_object_new_array();
    for (int i = 0; i < ma_trace_count(log); i++) json_object_array_add(a, ma_trace_record_json(ma_trace_entry(log, i), r, now));
    return a;
}

struct json_object *ma_ambient_state_json(ma_store *store, const ma_roster *r, const ma_focus_signal *focus, double now) {
    struct json_object *o = json_object_new_object(), *places = json_object_new_array();
    const ma_surface *surfaces[16];
    int n = ma_store_surfaces(store, now, surfaces, 16);
    ma_fact *facts = malloc(72 * sizeof *facts);
    int fact_count = facts ? ma_store_facts(store, now, facts, 72) : 0;
    /* One card per place with a surface or a fact. */
    ma_place seen[32];
    int seen_count = 0;
    for (int pass = 0; pass < 2; pass++) {
        int count = pass == 0 ? n : fact_count;
        for (int i = 0; i < count; i++) {
            const ma_place *place = pass == 0 ? &surfaces[i]->place : &facts[i].place;
            bool dup = false;
            for (int k = 0; k < seen_count && !dup; k++) dup = ma_place_equal(&seen[k], place);
            if (dup || seen_count >= 32) continue;
            seen[seen_count++] = *place;
            struct json_object *card = json_object_new_object(), *fl = json_object_new_array();
            json_object_object_add(card, "place", ma_place_json(place, r));
            const ma_surface *surface = pass == 0 ? surfaces[i] : ma_store_surface(store, place, now);
            if (surface) json_object_object_add(card, "surface", ma_surface_json(surface, now, r));
            for (int f = 0; f < fact_count; f++) if (ma_place_equal(&facts[f].place, place)) json_object_array_add(fl, ma_fact_json(&facts[f], now, r));
            json_object_object_add(card, "facts", fl);
            json_object_object_add(card, "isLead", json_object_new_boolean(focus && focus->has_lead && ma_place_equal(&focus->lead, place)));
            bool co = false, glanced = false;
            for (int k = 0; focus && k < focus->co_active_count; k++) if (ma_place_equal(&focus->co_active[k], place)) co = true;
            for (int k = 0; focus && k < focus->glanced_count; k++) if (ma_place_equal(&focus->glanced[k], place)) glanced = true;
            json_object_object_add(card, "isCoActive", json_object_new_boolean(co));
            json_object_object_add(card, "isGlanced", json_object_new_boolean(glanced));
            json_object_array_add(places, card);
        }
    }
    json_object_object_add(o, "places", places);
    if (focus && focus->has_lead) json_object_object_add(o, "lead", ma_place_json(&focus->lead, r));
    ma_selection sel;
    if (ma_store_selection(store, now, &sel)) {
        struct json_object *s = json_object_new_object();
        json_object_object_add(s, "place", ma_place_json(&sel.place, r));
        json_object_object_add(s, "applicationID", str(sel.application_id));
        json_object_object_add(s, "text", str(sel.text));
        if (sel.subject[0]) json_object_object_add(s, "subject", str(sel.subject));
        json_object_object_add(s, "capturedAt", ms(sel.captured_at));
        json_object_object_add(s, "age", json_object_new_double(now - sel.captured_at));
        json_object_object_add(o, "selection", s);
    }
    ma_world world;
    if (ma_store_world(store, now, &world)) json_object_object_add(o, "world", ma_world_json(&world, now));
    json_object_object_add(o, "utterance", str(ma_store_utterance(store)));
    const ma_route *route = ma_store_route(store);
    if (route) json_object_object_add(o, "route", ma_route_json(route, r));
    json_object_object_add(o, "factCount", json_object_new_int(fact_count));
    free(facts);
    return o;
}
