#include "ambient/engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- lanes ---- */

const char *ma_lane_name(ma_lane l) { return l == MA_LANE_ABILITY ? "ability" : l == MA_LANE_PERSONAL ? "personal" : NULL; }

int ma_lane_storage_lanes(ma_lane l, const char **out, int max) {
    const char *lane = l == MA_LANE_ABILITY ? "behavioral" : "personal";   /* one storage lane behind each thread now */
    if (max < 1) return 0;
    out[0] = lane;
    return 1;
}

static const char *const WRITING_TARGETS[] = { NULL, "selection", "passage" };
const char *ma_writing_target_name(ma_writing_target t) { return (unsigned)t < 3 ? WRITING_TARGETS[t] : NULL; }

/* ---- the gate ---- */

static int compare_strings(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void add_hint(ma_memory_plan *plan, const char *hint) {
    for (int i = 0; i < plan->hint_count; i++) if (strcmp(plan->relationship_hints[i], hint) == 0) return;
    if (plan->hint_count < MA_HINTS_MAX) snprintf(plan->relationship_hints[plan->hint_count++], 32, "%s", hint);
}

static void add_family(ma_signature *s, const char *family) {
    for (int i = 0; i < s->family_count; i++) if (strcmp(s->predicate_families[i], family) == 0) return;
    if (s->family_count < MA_HINTS_MAX) snprintf(s->predicate_families[s->family_count++], 32, "%s", family);
}

static void question_signature(const char *utterance, unsigned questions, ma_signature *out) {
    memset(out, 0, sizeof *out);
    out->questions = questions;
    static const struct { unsigned q; const char *families[3]; } FAMILIES[] = {
        { MA_Q_WHO, { "person", "owns", "works on" } }, { MA_Q_HOW, { "supports", "operates", "workflow" } },
        { MA_Q_WHERE, { "contains", "part of", "location" } }, { MA_Q_WHY, { "reason", "decided", "rationale" } },
        { MA_Q_WHEN, { "before", "after", "occurred" } }, { MA_Q_WHAT, { "is", "contains", "describes" } },
        { MA_Q_WHICH, { "contains", "selects", "belongs to" } },
    };
    for (size_t i = 0; i < sizeof FAMILIES / sizeof *FAMILIES; i++)
        if (questions & FAMILIES[i].q) for (int k = 0; k < 3; k++) add_family(out, FAMILIES[i].families[k]);
    qsort(out->predicate_families, (size_t)out->family_count, 32, compare_strings);
    if (questions & (MA_Q_HOW | MA_Q_WHERE)) { out->lane_priority[0] = MA_LANE_ABILITY; out->lane_priority[1] = MA_LANE_PERSONAL; }
    else if (questions & (MA_Q_WHO | MA_Q_WHY | MA_Q_WHEN)) { out->lane_priority[0] = MA_LANE_PERSONAL; out->lane_priority[1] = MA_LANE_ABILITY; }
    else { out->lane_priority[0] = MA_LANE_ABILITY; out->lane_priority[1] = MA_LANE_PERSONAL; }
    out->priority_count = 2;
    if (!out->family_count) { snprintf(out->semantic_projection, sizeof out->semantic_projection, "%s", utterance); return; }
    char cue[MA_HINTS_MAX * 34] = "";
    for (int i = 0; i < out->family_count; i++) { if (i) strncat(cue, ", ", sizeof cue - strlen(cue) - 1); strncat(cue, out->predicate_families[i], sizeof cue - strlen(cue) - 1); }
    snprintf(out->semantic_projection, sizeof out->semantic_projection, "%s [relationships: %s]", utterance, cue);
}

/* ThreadMemoryPlan.init: the priority filtered to the lanes, the default when that leaves nothing. */
static void plan_init(ma_memory_plan *plan, unsigned lanes, const ma_lane *priority, int priority_count) {
    plan->lanes = lanes ? lanes : MA_LANE_PERSONAL;
    ma_lane defaults[2] = { MA_LANE_ABILITY, MA_LANE_PERSONAL };
    plan->priority_count = 0;
    for (int i = 0; i < priority_count; i++) if (plan->lanes & priority[i]) plan->lane_priority[plan->priority_count++] = priority[i];
    if (!plan->priority_count) for (int i = 0; i < 2; i++) if (plan->lanes & defaults[i]) plan->lane_priority[plan->priority_count++] = defaults[i];
}

static bool has_ability(const char abilities[][32], int n, const char *id) {
    for (int i = 0; i < n; i++) if (strcmp(abilities[i], id) == 0) return true;
    return false;
}

void ma_gate_resolve(const char *utterance, const char *routing_query, const char *lead_application_id, const ma_roster *r, ma_gate *out) {
    memset(out, 0, sizeof *out);
    out->questions = ma_question_forms(utterance);
    question_signature(utterance, out->questions, &out->signature);
    out->requested_count = ma_roster_requested_abilities(r, routing_query ? routing_query : utterance, out->requested_abilities, MA_NEED_ABILITIES);
    /* The applications named by their aliases, and the abilities those name. */
    char named_abilities[MA_REGISTRATIONS_MAX * MA_PER_APP_ABILITIES][32];
    int named_ability_count = 0;
    for (int i = 0; i < r->app_count; i++) {
        const ma_registration *reg = &r->apps[i];
        if (!ma_registration_is_mentioned(reg, utterance)) continue;
        if (out->application_count < MA_GATE_APPLICATIONS) snprintf(out->applications[out->application_count++], MA_ID_MAX, "%s", reg->id);
        for (int k = 0; k < reg->ability_count; k++)
            if (!has_ability(named_abilities, named_ability_count, reg->abilities[k])) snprintf(named_abilities[named_ability_count++], 32, "%s", reg->abilities[k]);
    }
    qsort(out->applications, (size_t)out->application_count, MA_ID_MAX, compare_strings);
    const ma_registration *lead = lead_application_id ? ma_roster_registration(r, lead_application_id) : NULL;
    bool is_architecture = has_ability(out->requested_abilities, out->requested_count, "architect");
    bool lead_writes = lead && has_ability(lead->abilities, lead->ability_count, "writing");
    bool is_writing = has_ability(out->requested_abilities, out->requested_count, "writing") && (has_ability(named_abilities, named_ability_count, "writing") || lead_writes);
    bool inherits_application = lead_application_id && ma_references_application_anaphorically(utterance);
    bool prefers_ability = out->application_count > 0 || is_architecture || ((out->questions & MA_Q_HOW) && named_ability_count > 0) || inherits_application || out->requested_count > 0;
    /* abilityIDs = requested ∪ named; empty and preferring an ability, the lead's own. */
    char ids[MA_TARGETS_MAX + MA_REGISTRATIONS_MAX * MA_PER_APP_ABILITIES][32];
    int id_count = 0;
    for (int i = 0; i < out->requested_count; i++) snprintf(ids[id_count++], 32, "%s", out->requested_abilities[i]);
    for (int i = 0; i < named_ability_count; i++) if (!has_ability(ids, id_count, named_abilities[i])) snprintf(ids[id_count++], 32, "%s", named_abilities[i]);
    if (!id_count && prefers_ability && lead) for (int i = 0; i < lead->ability_count; i++) snprintf(ids[id_count++], 32, "%s", lead->abilities[i]);
    qsort(ids, (size_t)id_count, 32, compare_strings);
    ma_memory_plan *plan = &out->memory;
    for (int i = 0; i < id_count && plan->target_count < MA_TARGETS_MAX; i++) {
        snprintf(plan->targets[plan->target_count].ability_id, 32, "%s", ids[i]);
        plan->targets[plan->target_count].paradigm = ma_roster_paradigm(r, ids[i]);
        plan->target_count++;
    }
    bool has_target = plan->target_count > 0, expand = false;
    for (int i = 0; i < plan->target_count; i++) if (plan->targets[i].paradigm == MA_PARADIGM_DISCIPLINE) expand = true;
    unsigned lanes;
    if (has_target && (is_architecture || is_writing || (prefers_ability && (out->questions & (MA_Q_WHAT | MA_Q_WHICH | MA_Q_WHERE | MA_Q_WHY))))) lanes = MA_LANE_ABILITY | MA_LANE_PERSONAL;
    else if (has_target && prefers_ability) lanes = MA_LANE_ABILITY;
    else lanes = MA_LANE_PERSONAL;
    plan_init(plan, lanes, out->signature.lane_priority, out->signature.priority_count);
    for (int i = 0; i < out->signature.family_count; i++) add_hint(plan, out->signature.predicate_families[i]);
    if (expand) {
        add_hint(plan, "practices");
        for (int i = 0; i < plan->target_count; i++) if (plan->targets[i].paradigm == MA_PARADIGM_DISCIPLINE) add_hint(plan, plan->targets[i].ability_id);
    }
    qsort(plan->relationship_hints, (size_t)plan->hint_count, 32, compare_strings);
    plan->expand_discipline_usage = expand && (plan->lanes & MA_LANE_ABILITY);
}

int ma_gate_named_places(const ma_gate *g, const ma_roster *r, ma_place *out, int max) {
    int n = 0;
    for (int i = 0; i < g->application_count && n < max; i++) {
        const ma_registration *reg = ma_roster_registration(r, g->applications[i]);
        if (reg) out[n++] = ma_registration_place(reg);
    }
    return n;
}

/* ---- the route ---- */

bool ma_route_lead_place(const ma_route *route, ma_place *out) {
    if (!route->lead_application_id[0]) return false;
    if (out) *out = ma_place_application(route->lead_application_id);
    return true;
}

bool ma_route_spawn_place(const ma_route *route, ma_place *out) {
    if (!route->realm.has_spawn) return false;
    *out = route->realm.spawn;
    return true;
}

bool ma_route_is_action_turn(const ma_route *route) {
    return route->intent == MA_INTENT_OPERATE || route->intent == MA_INTENT_COMPOSE || route->verdicts.has_edit;
}

const ma_world *ma_route_routed_world(const ma_route *route) {
    if (!route->has_world) return NULL;
    if (!ma_world_is_direct_reference(&route->world)) return &route->world;
    return route->selection_defines_turn ? &route->world : NULL;
}

unsigned ma_engine_candidate_attentions(ma_intent intent, const ma_place *lead, const ma_place *named, int named_count, const ma_roster *r) {
    unsigned candidates = 0;
    for (int a = 0; a < MA_ATTENTION_COUNT; a++) candidates |= 1u << a;      /* no lane has eyes of its own */
    for (int i = 0; i < named_count; i++) if (ma_place_has_eyes(&named[i], r)) candidates |= 1u << named[i].attention;
    if (intent == MA_INTENT_CONVERSE) return candidates;
    if (lead && ma_place_has_eyes(lead, r)) candidates |= 1u << lead->attention;
    return candidates;
}

/* applicationProfile(_:represents:): a registration's logical identity against a packet's process identity. */
static bool represents(const ma_registration *reg, const ma_world *world) {
    ma_attention lane;
    if (ma_attention_from_name(reg->id, &lane) && lane == MA_ATTENTION_APPLICATIONS) return false;
    ma_place place = ma_world_place(world);
    if (ma_place_is_application(&place) && strcasecmp(place.application, reg->id) == 0) return true;
    if (!world->application_id[0]) return false;
    if (strcasecmp(reg->id, world->application_id) == 0) return true;
    if (strcasecmp(reg->name, world->application_id) == 0) return true;
    for (int i = 0; i < reg->alias_count; i++) if (strcasecmp(reg->aliases[i], world->application_id) == 0) return true;
    return world->attention == MA_ATTENTION_APPLICATIONS && world->subject[0] && ma_registration_is_mentioned(reg, world->subject);
}

static bool contains(const ma_place *places, int n, const ma_place *p) {
    for (int i = 0; i < n; i++) if (ma_place_equal(&places[i], p)) return true;
    return false;
}

static void classify(const ma_engine_inputs *in, const ma_verdicts *v, const ma_place *named, int named_count, const ma_gate *gate,
                     const char *lead_application_id, const ma_world *world, ma_intent *intent, ma_signal *signal) {
    if (in->pending_confirmation && v->bare_decision >= 0) { *intent = MA_INTENT_DECIDE; *signal = MA_SIGNAL_PENDING_DECISION; return; }
    if (in->active_routines > 0 && v->bare_decision == 0) { *intent = MA_INTENT_HALT; *signal = MA_SIGNAL_ROUTINE_STOP; return; }
    if (v->has_edit) { *intent = MA_INTENT_REVISE; *signal = MA_SIGNAL_EDIT_INTENT; return; }
    if (has_ability(gate->requested_abilities, gate->requested_count, "architect")) { *intent = MA_INTENT_ARCHITECT; *signal = MA_SIGNAL_ARCHITECT_ABILITY; return; }
    if (in->has_embedding_intent) { *intent = in->embedding_intent; *signal = MA_SIGNAL_EMBEDDING; return; }
    if (in->action_turn) {
        const ma_registration *lead = lead_application_id ? ma_roster_registration(in->roster, lead_application_id) : NULL;
        bool writing = lead && has_ability(lead->abilities, lead->ability_count, "writing");
        for (int i = 0; i < gate->application_count && !writing; i++) {
            const ma_registration *reg = ma_roster_registration(in->roster, gate->applications[i]);
            writing = reg && has_ability(reg->abilities, reg->ability_count, "writing");
        }
        /* nothing leads and nothing was named: the words themselves ask for writing ("write … in a new note") */
        if (!lead && !gate->application_count) writing = writing || has_ability(gate->requested_abilities, gate->requested_count, "writing");
        *intent = writing ? MA_INTENT_COMPOSE : MA_INTENT_OPERATE;
        *signal = writing ? MA_SIGNAL_WRITING_REGISTER : MA_SIGNAL_ACTION_COMMAND;
        return;
    }
    if (world && ma_world_is_direct_reference(world) && v->is_deictic) { *intent = MA_INTENT_PERCEIVE; *signal = MA_SIGNAL_WORLD; return; }
    if (v->is_deictic) { *intent = MA_INTENT_PERCEIVE; *signal = MA_SIGNAL_DEIXIS; return; }
    if (in->lead_application_id && in->lead_application_id[0]) {
        for (int i = 0; i < named_count; i++)
            if (ma_place_is_application(&named[i]) && strcasecmp(named[i].application, in->lead_application_id) == 0) {
                *intent = MA_INTENT_PERCEIVE; *signal = MA_SIGNAL_NAMED_LEAD_ATTENTION; return;
            }
    }
    if (v->names_ambient_source) { *intent = MA_INTENT_ASK; *signal = MA_SIGNAL_AMBIENT_SOURCE; return; }
    if (v->named_part[0]) { *intent = MA_INTENT_ASK; *signal = MA_SIGNAL_NAMED_PART; return; }
    *intent = MA_INTENT_CONVERSE;
    *signal = MA_SIGNAL_NONE;
}

void ma_engine_resolve(const ma_engine_inputs *in, ma_route *out) {
    memset(out, 0, sizeof *out);
    const ma_roster *r = in->roster;
    const char *utterance = in->utterance ? in->utterance : "";
    const ma_world *attention = in->world && ma_world_is_fresh(in->world, in->now) ? in->world : NULL;

    ma_place explicit[MA_REGISTRATIONS_MAX];
    int explicit_count = ma_explicitly_named_places(utterance, r, explicit, MA_REGISTRATIONS_MAX);
    /* The named profiles are the explicitly named places' registrations. */
    const ma_registration *named_regs[MA_REGISTRATIONS_MAX];
    int named_reg_count = 0;
    for (int i = 0; i < explicit_count; i++) {
        const ma_registration *reg = ma_roster_registration(r, explicit[i].application);
        if (reg) named_regs[named_reg_count++] = reg;
    }
    ma_place named[MA_REGISTRATIONS_MAX];
    int named_count = named_reg_count ? 0 : ma_named_places(utterance, r, named, MA_REGISTRATIONS_MAX);
    if (named_reg_count) { named_count = explicit_count; memcpy(named, explicit, (size_t)explicit_count * sizeof *named); }
    bool is_deictic = ma_is_deictic(utterance);

    const ma_place *conflict = explicit_count == 0 && named_count == 1 ? named : explicit;
    int conflict_count = explicit_count == 0 && named_count == 1 ? 1 : explicit_count;
    bool conflicts_with_place = false, conflicts_with_application = false;
    if (attention) {
        ma_place attention_place = ma_world_place(attention);
        for (int i = 0; i < conflict_count && !conflicts_with_place; i++) {
            const ma_registration *reg = ma_place_is_application(&conflict[i]) ? ma_roster_registration(r, conflict[i].application) : NULL;
            conflicts_with_place = reg ? !represents(reg, attention) : !ma_place_equal(&conflict[i], &attention_place);
        }
        for (int i = 0; i < named_reg_count && !conflicts_with_application; i++) conflicts_with_application = !represents(named_regs[i], attention);
    }
    ma_edit_intent classified;
    const ma_edit_intent *edit = in->edit_intent;
    if (!edit && in->classify_edit) {
        const char *aliases[MA_REGISTRATIONS_MAX * 2];
        int alias_count = 0;
        for (int i = 0; i < r->app_count && alias_count < MA_REGISTRATIONS_MAX * 2; i++) aliases[alias_count++] = r->apps[i].id;
        if (ma_edit_intent_in(utterance, aliases, alias_count, &classified)) edit = &classified;
    }
    bool selection_defines_turn = attention && ma_world_is_direct_reference(attention) && (is_deictic || edit) && !conflicts_with_place && !conflicts_with_application;
    const ma_world *routed_world = attention && ma_world_is_direct_reference(attention) ? (selection_defines_turn ? attention : NULL) : attention;

    const ma_place *explicit_lead = explicit_count == 1 ? &explicit[0] : NULL;
    const char *explicit_application = named_reg_count == 1 ? named_regs[0]->id : NULL;
    const char *attention_application = NULL;
    if (attention) for (int i = 0; i < r->app_count && !attention_application; i++) if (represents(&r->apps[i], attention)) attention_application = r->apps[i].id;
    const char *lead_id = selection_defines_turn
                              ? (attention_application ? attention_application : explicit_lead ? explicit_lead->application : NULL)
                              : (explicit_application ? explicit_application : explicit_lead ? explicit_lead->application
                                 : in->lead_application_id && in->lead_application_id[0] ? in->lead_application_id : NULL);
    if (lead_id) snprintf(out->lead_application_id, sizeof out->lead_application_id, "%s", lead_id);

    ma_gate_resolve(utterance, in->routing_query, lead_id, r, &out->gate);

    ma_verdicts *v = &out->verdicts;
    v->action_turn = in->action_turn;
    v->has_edit = edit != NULL;
    if (edit) v->edit = *edit;
    ma_named_part(utterance, v->named_part, sizeof v->named_part);
    v->names_ambient_source = ma_names_ambient_source(utterance);
    v->is_deictic = is_deictic;
    v->names_transform = ma_names_transform(utterance, r);
    const char *discipline = ma_named_discipline(utterance, r);
    if (discipline) snprintf(v->focus_override, sizeof v->focus_override, "%s", discipline);
    v->bare_decision = in->bare_decision == -2 ? ma_bare_decision(utterance) : in->bare_decision;

    classify(in, v, named, named_count, &out->gate, lead_id, routed_world, &out->intent, &out->decided_by);

    if (edit) {
        if (selection_defines_turn && attention && ma_world_is_direct_reference(attention) && attention->editable && attention->application_id[0]) out->writing_target = MA_WRITING_SELECTION;
        else out->writing_target = MA_WRITING_PASSAGE;
    }
    if (out->writing_target == MA_WRITING_SELECTION) snprintf(out->supporting_context, sizeof out->supporting_context, "%s", v->named_part);

    /* All the named places: the utterance's, plus the gate's by name. */
    for (int i = 0; i < named_count && out->named_count < MA_NAMED_MAX; i++) out->named_places[out->named_count++] = named[i];
    ma_place from_gate[MA_GATE_APPLICATIONS];
    int gate_count = ma_gate_named_places(&out->gate, r, from_gate, MA_GATE_APPLICATIONS);
    for (int i = 0; i < gate_count && out->named_count < MA_NAMED_MAX; i++) if (!contains(out->named_places, out->named_count, &from_gate[i])) out->named_places[out->named_count++] = from_gate[i];

    if (attention) { out->has_world = true; out->world = *attention; }
    out->selection_defines_turn = selection_defines_turn;

    ma_realm_inputs realm_in = {
        .utterance = utterance, .ability_query = in->routing_query, .named = out->named_places, .named_count = out->named_count,
        .discipline = v->focus_override[0] ? v->focus_override : NULL, .decided_by = out->decided_by, .focus = in->focus,
        .evidence = in->evidence, .evidence_count = in->evidence_count, .roster = r, .now = in->now,
    };
    ma_realm_resolve(&realm_in, &out->realm);

    ma_place lead_place;
    bool has_lead = ma_route_lead_place(out, &lead_place);
    out->candidate_attentions = ma_engine_candidate_attentions(out->intent, has_lead ? &lead_place : NULL, named, named_count, r);
    out->needs_locate = out->writing_target == MA_WRITING_PASSAGE;
    out->needs_pre_read = !edit && !in->action_turn && v->named_part[0];
    out->needs_execution = out->intent != MA_INTENT_ARCHITECT && out->intent != MA_INTENT_CONVERSE;
    out->ranking_mode = ma_ranker_mode(utterance, has_lead ? &lead_place : NULL, r);
}
