#include "ambient/realm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const EVIDENCE_NAMES[] = { "glance", "activation", "activity" };
const char *ma_evidence_name(ma_evidence_kind k) { return (unsigned)k < 3 ? EVIDENCE_NAMES[k] : NULL; }
double ma_focus_horizon(ma_evidence_kind kind) { return kind == MA_EVIDENCE_GLANCE ? MA_GLANCE_HORIZON : MA_CO_ACTIVE_HORIZON; }

void ma_focus_init(ma_focus_ledger *l) { memset(l, 0, sizeof *l); }

void ma_focus_note(ma_focus_ledger *l, const ma_place *place, ma_evidence_kind kind, double now) {
    int i = 0;
    for (; i < l->count; i++) if (ma_place_equal(&l->entries[i].place, place)) break;
    if (i == l->count) {
        if (l->count < MA_FOCUS_LEDGER_MAX) l->count++;
        else {
            i = 0;
            for (int j = 1; j < l->count; j++) if (l->entries[j].at < l->entries[i].at) i = j;
        }
    }
    l->entries[i] = (ma_focus_evidence){ .place = *place, .kind = kind, .at = now };
    if (kind != MA_EVIDENCE_GLANCE) { l->has_lead = true; l->lead = *place; l->lead_at = now; }
}

int ma_focus_fresh_evidence(const ma_focus_ledger *l, double now, ma_focus_evidence *out, int max) {
    int n = 0;
    for (int i = 0; i < l->count && n < max; i++)
        if (now - l->entries[i].at <= ma_focus_horizon(l->entries[i].kind)) out[n++] = l->entries[i];
    return n;
}

void ma_focus_project(const ma_focus_ledger *l, double now, ma_focus_signal *out) {
    memset(out, 0, sizeof *out);
    if (l->has_lead && now - l->lead_at <= MA_SIGNAL_HORIZON) { out->has_lead = true; out->lead = l->lead; }
    ma_focus_evidence fresh[MA_FOCUS_LEDGER_MAX];
    int n = ma_focus_fresh_evidence(l, now, fresh, MA_FOCUS_LEDGER_MAX);
    /* Strongest evidence first, then the most recent. */
    for (int i = 1; i < n; i++) {
        ma_focus_evidence e = fresh[i];
        int j = i - 1;
        while (j >= 0 && (fresh[j].kind < e.kind || (fresh[j].kind == e.kind && fresh[j].at < e.at))) { fresh[j + 1] = fresh[j]; j--; }
        fresh[j + 1] = e;
    }
    for (int i = 0; i < n && out->co_active_count < MA_CO_ACTIVE_MAX; i++) {
        if (out->has_lead && ma_place_equal(&fresh[i].place, &out->lead)) continue;
        out->co_active[out->co_active_count++] = fresh[i].place;
        if (fresh[i].kind == MA_EVIDENCE_GLANCE) out->glanced[out->glanced_count++] = fresh[i].place;
    }
}

/* ---- the realm ---- */

bool ma_need_is_empty(const ma_need *n) { return n->ability_count == 0 && !n->discipline[0]; }
bool ma_candidate_conforms(const ma_candidate *c) { return c->conforms_count > 0 || c->conforms_by_discipline; }

void ma_realm_need(const ma_realm_inputs *in, ma_need *out) {
    memset(out, 0, sizeof *out);
    out->ability_count = ma_roster_requested_abilities(in->roster, in->ability_query ? in->ability_query : in->utterance, out->abilities, MA_NEED_ABILITIES);
    if (in->discipline) snprintf(out->discipline, sizeof out->discipline, "%s", in->discipline);
}

int ma_realm_candidates(const ma_need *need, const ma_realm_inputs *in, ma_candidate *out, int max) {
    int n = 0;
    const ma_roster *r = in->roster;
    for (int a = 0; a < r->app_count && n < max; a++) {
        const ma_registration *reg = &r->apps[a];
        ma_candidate c;
        memset(&c, 0, sizeof c);
        c.place = ma_registration_place(reg);
        for (int i = 0; i < need->ability_count; i++)
            for (int j = 0; j < reg->ability_count; j++)
                if (strcmp(need->abilities[i], reg->abilities[j]) == 0 && c.conforms_count < MA_NEED_ABILITIES)
                    snprintf(c.conforms_by_abilities[c.conforms_count++], 32, "%s", need->abilities[i]);
        const char *focus = ma_place_focus(&c.place, r);
        c.conforms_by_discipline = need->discipline[0] && focus && strcmp(focus, need->discipline) == 0;
        /* An empty need admits everyone with eyes: nothing was asked for, so nothing can fail to conform. */
        bool conforms = ma_need_is_empty(need) ? reg->has_eyes : ma_candidate_conforms(&c);
        if (!conforms) continue;
        c.target_class_count = reg->target_class_count;
        for (int i = 0; i < reg->target_class_count; i++) snprintf(c.target_classes[i], 32, "%s", reg->target_classes[i]);
        c.has_eyes = reg->has_eyes;
        for (int e = 0; e < in->evidence_count; e++) {
            if (!ma_place_equal(&in->evidence[e].place, &c.place)) continue;
            c.has_evidence = true;
            c.evidence = in->evidence[e].kind;
            c.evidence_age = in->now - in->evidence[e].at;
        }
        /* Sorted by place token: a record, not a ranking. */
        int pos = n;
        while (pos > 0 && ma_place_compare(&out[pos - 1].place, &c.place) > 0) { out[pos] = out[pos - 1]; pos--; }
        out[pos] = c;
        n++;
    }
    return n;
}

static bool among(const ma_candidate *candidates, int n, const ma_need *need, const ma_place *place) {
    for (int i = 0; i < n; i++) {
        if (!ma_place_equal(&candidates[i].place, place)) continue;
        return ma_need_is_empty(need) || ma_candidate_conforms(&candidates[i]);
    }
    return false;
}

bool ma_realm_place(const ma_candidate *candidates, int n, const ma_need *need, const ma_realm_inputs *in, ma_place *out) {
    if (in->named_count > 0) {
        /* Sorted so several named places resolve the same way twice. */
        ma_place named[MA_NAMED_MAX];
        int count = in->named_count < MA_NAMED_MAX ? in->named_count : MA_NAMED_MAX;
        for (int i = 0; i < count; i++) {
            int pos = i;
            while (pos > 0 && ma_place_compare(&named[pos - 1], &in->named[i]) > 0) { named[pos] = named[pos - 1]; pos--; }
            named[pos] = in->named[i];
        }
        for (int i = 0; i < count; i++) if (among(candidates, n, need, &named[i])) { *out = named[i]; return true; }
        *out = named[0];
        return true;
    }
    if (in->focus && in->focus->has_lead && among(candidates, n, need, &in->focus->lead)) { *out = in->focus->lead; return true; }
    if (in->focus)
        for (int i = 0; i < in->focus->co_active_count; i++)
            if (among(candidates, n, need, &in->focus->co_active[i])) { *out = in->focus->co_active[i]; return true; }
    return false;
}

void ma_realm_resolve(const ma_realm_inputs *in, ma_realm *out) {
    memset(out, 0, sizeof *out);
    ma_realm_need(in, &out->need);
    out->candidate_count = ma_realm_candidates(&out->need, in, out->candidates, MA_CANDIDATES_MAX);
    out->has_place = ma_realm_place(out->candidates, out->candidate_count, &out->need, in, &out->place);
    out->decided_by = out->has_place ? in->decided_by : MA_SIGNAL_NONE;
}

const ma_candidate *ma_realm_chosen(const ma_realm *r) {
    if (!r->has_place) return NULL;
    for (int i = 0; i < r->candidate_count; i++) if (ma_place_equal(&r->candidates[i].place, &r->place)) return &r->candidates[i];
    return NULL;
}
