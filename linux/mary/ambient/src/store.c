#include "ambient/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambient/engine.h"

/* ---- slots ---- */

static const char *const SLOT_TOKENS[MA_SLOT_COUNT] = {
    "file", "viewport", "selection", "object-selection", "cursor", "git", "project", "read", "digest", "heard", "glimpsed",
};
static const int SLOT_ORDER[MA_SLOT_COUNT] = { 5, 3, 1, 2, 4, 7, 6, 0, 8, 10, 9 };

ma_slot ma_slot_of(ma_slot_kind kind) {
    ma_slot s = { .kind = kind };
    return s;
}

ma_slot ma_slot_read(const char *phrase, const char *document) {
    ma_slot s = { .kind = MA_SLOT_NAMED_READ };
    snprintf(s.phrase, sizeof s.phrase, "%s", phrase ? phrase : "");
    if (document) snprintf(s.document, sizeof s.document, "%s", document);
    return s;
}

void ma_slot_token(const ma_slot *s, char *out, size_t n) {
    if (s->kind == MA_SLOT_NAMED_READ) {
        if (s->document[0]) snprintf(out, n, "read:%s#%s", s->document, s->phrase);
        else snprintf(out, n, "read:%s", s->phrase);
    } else {
        snprintf(out, n, "%s", (unsigned)s->kind < MA_SLOT_COUNT ? SLOT_TOKENS[s->kind] : "");
    }
}

bool ma_slot_equal(const ma_slot *a, const ma_slot *b) {
    if (a->kind != b->kind) return false;
    if (a->kind != MA_SLOT_NAMED_READ) return true;
    return strcmp(a->phrase, b->phrase) == 0 && strcmp(a->document, b->document) == 0;
}

bool ma_slot_is_perceived(const ma_slot *s) {
    switch (s->kind) {
    case MA_SLOT_NAMED_READ: case MA_SLOT_DIGEST: case MA_SLOT_HEARD: case MA_SLOT_GLIMPSED: case MA_SLOT_CURSOR: return false;
    default: return true;
    }
}

bool ma_slot_is_read(const ma_slot *s) { return s->kind == MA_SLOT_NAMED_READ; }
int ma_slot_order(const ma_slot *s) { return (unsigned)s->kind < MA_SLOT_COUNT ? SLOT_ORDER[s->kind] : 99; }

/* ---- facts ---- */

static const char *const PROVENANCE_NAMES[] = { "liveAX", "cachedBody", "recipeRead", "derived" };
static const char *const PROVENANCE_DISPLAY[] = { "live accessibility read", "cached document body", "binding read", "derived from watcher state" };
const char *ma_provenance_name(ma_provenance p) { return (unsigned)p < 4 ? PROVENANCE_NAMES[p] : NULL; }
const char *ma_provenance_display_name(ma_provenance p) { return (unsigned)p < 4 ? PROVENANCE_DISPLAY[p] : NULL; }
const char *ma_registered_name(ma_registered r) { return r == MA_REGISTERED_ASKED_FOR ? "askedFor" : "perceived"; }
const char *ma_registered_verb(ma_registered r) { return r == MA_REGISTERED_ASKED_FOR ? "read" : "seen"; }

/* AmbientSamplingCadence: a 2.5 s active interval, a body refreshed every 8 ticks, fresh for 1.5 × that. */
#define ACTIVE_INTERVAL 2.5
#define BODY_FRESH_WINDOW (ACTIVE_INTERVAL * 8 * 1.5)
#define DIGEST_REFRESH_FLOOR 180.0
#define CURSOR_REFRESH_FLOOR 5.0

double ma_fact_default_fresh_for(ma_provenance p, const ma_slot *slot) {
    if (slot->kind == MA_SLOT_DIGEST) return DIGEST_REFRESH_FLOOR * 3;
    if (slot->kind == MA_SLOT_CURSOR) return CURSOR_REFRESH_FLOOR * 2 + 2;
    switch (p) {
    case MA_PROVENANCE_LIVE_AX: return ACTIVE_INTERVAL * 2;
    case MA_PROVENANCE_CACHED_BODY: case MA_PROVENANCE_RECIPE_READ: return BODY_FRESH_WINDOW;
    default: return 60;
    }
}

double ma_fact_default_retain_for(const ma_slot *slot) {
    switch (slot->kind) {
    case MA_SLOT_DIGEST: return DIGEST_REFRESH_FLOOR * 5;
    case MA_SLOT_CURSOR: return 60;
    case MA_SLOT_NAMED_READ: return 1200;
    default: return 300;
    }
}

/* The first n code points of a UTF-8 string, never splitting a sequence. */
static size_t clip_utf8(const char *text, size_t max_points) {
    size_t i = 0, points = 0;
    while (text[i] && points < max_points) {
        i++;
        while ((text[i] & 0xC0) == 0x80) i++;
        points++;
    }
    return i;
}

void ma_fact_init(ma_fact *f, const ma_place *place, const ma_slot *slot, const char *content, ma_provenance provenance, double captured_at) {
    memset(f, 0, sizeof *f);
    f->place = *place;
    f->slot = *slot;
    if (content) {
        size_t n = clip_utf8(content, MA_CONTENT_CAP);
        memcpy(f->content, content, n);
    }
    f->provenance = provenance;
    f->registration = MA_REGISTERED_PERCEIVED;
    f->captured_at = captured_at;
    f->fresh_for = ma_fact_default_fresh_for(provenance, slot);
    f->retain_for = ma_fact_default_retain_for(slot);
}

double ma_fact_age(const ma_fact *f, double now) { return now > f->captured_at ? now - f->captured_at : 0; }
bool ma_fact_is_fresh(const ma_fact *f, double now) { double age = now - f->captured_at; return age >= 0 && age <= f->fresh_for; }
bool ma_fact_is_expired(const ma_fact *f, double now) { return now - f->captured_at > f->retain_for; }

void ma_fact_key(const ma_fact *f, char *out, size_t n) {
    char place[MA_TOKEN_MAX], slot[2 * MA_PHRASE_MAX + 8];
    ma_place_token(&f->place, place, sizeof place);
    ma_slot_token(&f->slot, slot, sizeof slot);
    snprintf(out, n, "%s/%s", place, slot);
}

int ma_fact_ordered(const ma_fact *a, const ma_fact *b, const ma_roster *r) {
    int oa = ma_place_order(&a->place, r), ob = ma_place_order(&b->place, r);
    if (oa != ob) return oa < ob ? -1 : 1;
    int sa = ma_slot_order(&a->slot), sb = ma_slot_order(&b->slot);
    if (sa != sb) return sa < sb ? -1 : 1;
    char ka[512], kb[512];
    ma_fact_key(a, ka, sizeof ka);
    ma_fact_key(b, kb, sizeof kb);
    return strcmp(ka, kb);
}

/* ---- surfaces ---- */

void ma_surface_init(ma_surface *s, const ma_place *place, const char *app_name, const char *app_id, double captured_at) {
    memset(s, 0, sizeof *s);
    s->place = *place;
    snprintf(s->app_name, sizeof s->app_name, "%s", app_name ? app_name : "");
    snprintf(s->app_id, sizeof s->app_id, "%s", app_id ? app_id : "");
    s->captured_at = captured_at;
    s->fresh_for = MA_SURFACE_FRESH_FOR;
}

double ma_surface_age(const ma_surface *s, double now) { return now > s->captured_at ? now - s->captured_at : 0; }
bool ma_surface_is_fresh(const ma_surface *s, double now) { double age = now - s->captured_at; return age >= 0 && age <= s->fresh_for; }
const char *ma_element_descriptor(const ma_element *e) { return e->label[0] ? e->label : e->kind; }

/* ---- selection, world ---- */

bool ma_selection_is_fresh(const ma_selection *s, double now) { double age = now - s->captured_at; return age >= 0 && age <= MA_HANDOFF_FRESH_FOR; }

static const char *const SENSE_NAMES[] = { "workspace", "selection", "hover" };
const char *ma_sense_name(ma_sense s) { return (unsigned)s < 3 ? SENSE_NAMES[s] : NULL; }
double ma_sense_fresh_for(ma_sense s) { return s == MA_SENSE_WORKSPACE ? MA_WORKSPACE_FRESH_FOR : s == MA_SENSE_SELECTION ? MA_HANDOFF_FRESH_FOR : 3; }

ma_place ma_world_place(const ma_world *w) {
    if (w->application_id[0]) return ma_place_application(w->application_id);
    return ma_place_lane(w->attention);
}

bool ma_world_is_fresh(const ma_world *w, double now) { double age = now - w->captured_at; return age >= 0 && age <= w->fresh_for; }

bool ma_world_matches(const ma_world *w, const ma_fact *f) {
    if (w->has_key) return ma_place_equal(&w->key_place, &f->place) && ma_slot_equal(&w->key_slot, &f->slot);
    if (f->place.attention != w->attention) return false;
    return !w->subject[0] || strcmp(f->subject, w->subject) == 0;
}

bool ma_world_is_direct_reference(const ma_world *w) { return w->sense == MA_SENSE_SELECTION; }

/* ---- the store ---- */

#define FACTS_MAX 64
#define SURFACES_MAX 16
#define TOMBSTONES_MAX 16

struct ma_store {
    const ma_roster *roster;
    ma_fact facts[FACTS_MAX];
    int fact_count;
    ma_surface surfaces[SURFACES_MAX];
    int surface_count;
    bool has_lead;
    ma_place lead;
    double lead_at;
    char utterance[2048];
    ma_route *route;
    bool has_world;
    ma_world world;
    bool has_selection;
    ma_selection selection;
    char claimed_id[40];                        /* the packet a turn already took */
    struct { char application_id[MA_ID_MAX]; double at; } cleared[TOMBSTONES_MAX];
    int cleared_count;
};

ma_store *ma_store_new(void) { return calloc(1, sizeof(ma_store)); }

void ma_store_free(ma_store *s) {
    if (!s) return;
    free(s->route);
    free(s);
}

void ma_store_set_roster(ma_store *s, const ma_roster *r) { s->roster = r; }

static void prune(ma_store *s, double now) {
    int kept = 0;
    for (int i = 0; i < s->fact_count; i++) {
        if (!ma_fact_is_expired(&s->facts[i], now)) s->facts[kept++] = s->facts[i];
    }
    s->fact_count = kept;
}

static int find_fact(const ma_store *s, const ma_place *place, const ma_slot *slot) {
    for (int i = 0; i < s->fact_count; i++)
        if (ma_place_equal(&s->facts[i].place, place) && ma_slot_equal(&s->facts[i].slot, slot)) return i;
    return -1;
}

static void remove_fact(ma_store *s, int i) {
    memmove(&s->facts[i], &s->facts[i + 1], (size_t)(s->fact_count - i - 1) * sizeof *s->facts);
    s->fact_count--;
}

/* Newest MA_NAMED_READ_CAP reads per place survive. */
static void cap_named_reads(ma_store *s, const ma_place *place) {
    for (;;) {
        int reads = 0, oldest = -1;
        for (int i = 0; i < s->fact_count; i++) {
            if (!ma_place_equal(&s->facts[i].place, place) || !ma_slot_is_read(&s->facts[i].slot)) continue;
            reads++;
            if (oldest < 0 || s->facts[i].captured_at < s->facts[oldest].captured_at) oldest = i;
        }
        if (reads <= MA_NAMED_READ_CAP) return;
        remove_fact(s, oldest);
    }
}

static void put_fact(ma_store *s, const ma_fact *f) {
    int i = find_fact(s, &f->place, &f->slot);
    if (i >= 0) { s->facts[i] = *f; return; }
    if (s->fact_count >= FACTS_MAX) {
        /* Full: the oldest perceived fact makes room. */
        int oldest = 0;
        for (int j = 1; j < s->fact_count; j++) if (s->facts[j].captured_at < s->facts[oldest].captured_at) oldest = j;
        remove_fact(s, oldest);
    }
    s->facts[s->fact_count++] = *f;
}

void ma_store_register(ma_store *s, const ma_fact *f, double now) {
    if (f->slot.kind == MA_SLOT_SELECTION) return;
    prune(s, now);
    put_fact(s, f);
    cap_named_reads(s, &f->place);
}

void ma_store_replace_perceived(ma_store *s, const ma_place *place, const ma_fact *facts, int n, double now) {
    prune(s, now);
    for (int i = s->fact_count - 1; i >= 0; i--)
        if (ma_place_equal(&s->facts[i].place, place) && ma_slot_is_perceived(&s->facts[i].slot)) remove_fact(s, i);
    for (int i = 0; i < n; i++) {
        if (facts[i].slot.kind == MA_SLOT_SELECTION || !ma_slot_is_perceived(&facts[i].slot)) continue;
        put_fact(s, &facts[i]);
    }
}

void ma_store_forget_perceived(ma_store *s, const ma_place *place) {
    for (int i = s->fact_count - 1; i >= 0; i--)
        if (ma_place_equal(&s->facts[i].place, place) && ma_slot_is_perceived(&s->facts[i].slot)) remove_fact(s, i);
    if (s->has_world && s->world.attention == place->attention && s->world.sense != MA_SENSE_SELECTION) s->has_world = false;
}

static void discard_selection(ma_store *s, ma_attention a, double now) {
    if (!s->has_selection || s->selection.place.attention != a) return;
    ma_store_clear_selection(s, s->selection.application_id, now);
}

void ma_store_forget_attention(ma_store *s, ma_attention a) {
    for (int i = s->fact_count - 1; i >= 0; i--) if (s->facts[i].place.attention == a) remove_fact(s, i);
    if (s->has_world && s->world.attention == a) s->has_world = false;
    discard_selection(s, a, s->has_selection ? s->selection.captured_at + 1 : 0);
}

void ma_store_forget_key(ma_store *s, const ma_place *place, const ma_slot *slot) {
    int i = find_fact(s, place, slot);
    if (i >= 0) remove_fact(s, i);
    if (s->has_world && s->world.has_key && ma_place_equal(&s->world.key_place, place) && ma_slot_equal(&s->world.key_slot, slot)) s->has_world = false;
}

int ma_store_note_spoken(ma_store *s, const char *const *passages, int n, const char *note, double now) {
    int touched = 0;
    char clipped[MA_NOTE_MAX + 1] = "";
    if (note) memcpy(clipped, note, clip_utf8(note, MA_NOTE_MAX));
    for (int i = 0; i < s->fact_count; i++) {
        ma_fact *f = &s->facts[i];
        if (!f->content[0]) continue;
        bool said = false;
        for (int p = 0; p < n && !said; p++) said = passages[p] && *passages[p] && strstr(passages[p], f->content) != NULL;
        if (!said) continue;
        f->spoken_at = now;
        snprintf(f->spoken_note, sizeof f->spoken_note, "%s", clipped);
        touched++;
    }
    return touched;
}

void ma_store_note_lead(ma_store *s, const ma_place *place, double now) {
    s->has_lead = place != NULL;
    if (place) { s->lead = *place; s->lead_at = now; }
}

bool ma_store_lead_place(const ma_store *s, double now, ma_place *out) {
    if (!s->has_lead || now - s->lead_at > MA_LEAD_HORIZON) return false;
    if (out) *out = s->lead;
    return true;
}

void ma_store_note_utterance(ma_store *s, const char *text) { snprintf(s->utterance, sizeof s->utterance, "%s", text ? text : ""); }
const char *ma_store_utterance(const ma_store *s) { return s->utterance; }

void ma_store_note_route(ma_store *s, const ma_route *route) {
    if (!route) { free(s->route); s->route = NULL; return; }
    if (!s->route) s->route = malloc(sizeof *s->route);
    if (s->route) *s->route = *route;
}

const ma_route *ma_store_route(const ma_store *s) { return s->route; }

/* ---- surfaces ---- */

static void prune_surfaces(ma_store *s, double now) {
    int kept = 0;
    for (int i = 0; i < s->surface_count; i++)
        if (ma_surface_is_fresh(&s->surfaces[i], now)) { if (kept != i) s->surfaces[kept] = s->surfaces[i]; kept++; }
    s->surface_count = kept;
}

static int find_surface(const ma_store *s, const ma_place *place) {
    for (int i = 0; i < s->surface_count; i++) if (ma_place_equal(&s->surfaces[i].place, place)) return i;
    return -1;
}

void ma_store_note_surface(ma_store *s, const ma_surface *surface, double now) {
    prune_surfaces(s, now);
    int i = find_surface(s, &surface->place);
    if (i >= 0) {
        if (ma_surface_is_fresh(&s->surfaces[i], now) && s->surfaces[i].captured_at > surface->captured_at) return;   /* out of order */
        s->surfaces[i] = *surface;
        return;
    }
    if (s->surface_count >= SURFACES_MAX) {
        int oldest = 0;
        for (int j = 1; j < s->surface_count; j++) if (s->surfaces[j].captured_at < s->surfaces[oldest].captured_at) oldest = j;
        s->surfaces[oldest] = *surface;
        return;
    }
    s->surfaces[s->surface_count++] = *surface;
}

const ma_surface *ma_store_surface(ma_store *s, const ma_place *place, double now) {
    prune_surfaces(s, now);
    int i = find_surface(s, place);
    return i >= 0 ? &s->surfaces[i] : NULL;
}

int ma_store_surfaces(ma_store *s, double now, const ma_surface **out, int max) {
    prune_surfaces(s, now);
    int n = 0;
    for (int i = 0; i < s->surface_count && n < max; i++) {
        const ma_surface *sf = &s->surfaces[i];
        int pos = n;
        while (pos > 0) {
            int oa = ma_place_order(&out[pos - 1]->place, s->roster), ob = ma_place_order(&sf->place, s->roster);
            if (oa < ob || (oa == ob && out[pos - 1]->captured_at >= sf->captured_at)) break;
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = sf;
        n++;
    }
    return n;
}

void ma_store_forget_surface(ma_store *s, const ma_place *place) {
    int i = find_surface(s, place);
    if (i < 0) return;
    memmove(&s->surfaces[i], &s->surfaces[i + 1], (size_t)(s->surface_count - i - 1) * sizeof *s->surfaces);
    s->surface_count--;
}

/* ---- the selection ---- */

static double cleared_at(const ma_store *s, const char *application_id) {
    for (int i = 0; i < s->cleared_count; i++) if (strcmp(s->cleared[i].application_id, application_id) == 0) return s->cleared[i].at;
    return -1;
}

void ma_store_record_selection(ma_store *s, const ma_selection *sel, double now) {
    if (!sel->text[0]) return;
    if (sel->captured_at <= cleared_at(s, sel->application_id)) return;      /* a late read of a cleared source */
    if (s->has_selection && ma_selection_is_fresh(&s->selection, now) && s->selection.captured_at > sel->captured_at) return;
    s->selection = *sel;
    if (!s->selection.id[0]) snprintf(s->selection.id, sizeof s->selection.id, "sel-%.0f", sel->captured_at * 1000);
    s->has_selection = true;
}

void ma_store_clear_selection(ma_store *s, const char *application_id, double now) {
    if (!application_id) return;
    int i = 0;
    for (; i < s->cleared_count; i++) if (strcmp(s->cleared[i].application_id, application_id) == 0) break;
    if (i == s->cleared_count) {
        if (s->cleared_count == TOMBSTONES_MAX) i = 0; else s->cleared_count++;
        snprintf(s->cleared[i].application_id, MA_ID_MAX, "%s", application_id);
    }
    s->cleared[i].at = now;
    if (s->has_selection && strcmp(s->selection.application_id, application_id) == 0) s->has_selection = false;
    if (s->has_world && s->world.sense == MA_SENSE_SELECTION && strcmp(s->world.application_id, application_id) == 0) s->has_world = false;
}

bool ma_store_selection(const ma_store *s, double now, ma_selection *out) {
    if (!s->has_selection || !ma_selection_is_fresh(&s->selection, now)) return false;
    if (strcmp(s->claimed_id, s->selection.id) == 0) return false;
    if (out) *out = s->selection;
    return true;
}

bool ma_store_claim_selection(ma_store *s, double now, ma_selection *out) {
    if (!ma_store_selection(s, now, out)) return false;
    snprintf(s->claimed_id, sizeof s->claimed_id, "%s", s->selection.id);
    return true;
}

static void selection_fact(const ma_selection *sel, ma_fact *f) {
    ma_slot slot = ma_slot_of(MA_SLOT_SELECTION);
    ma_fact_init(f, &sel->place, &slot, sel->text, MA_PROVENANCE_LIVE_AX, sel->captured_at);
    memcpy(f->surrounding, sel->surrounding, clip_utf8(sel->surrounding, MA_CONTENT_CAP));
    snprintf(f->subject, sizeof f->subject, "%s", sel->subject);
    snprintf(f->application_id, sizeof f->application_id, "%s", sel->application_id);
    f->has_bounds = sel->has_range;
    f->lower = sel->lower;
    f->upper = sel->upper;
    f->document_total = sel->document_total;
    snprintf(f->anchor, sizeof f->anchor, "their highlight");
    f->fresh_for = MA_HANDOFF_FRESH_FOR;
}

static void selection_world(const ma_selection *sel, ma_world *w) {
    memset(w, 0, sizeof *w);
    w->sense = MA_SENSE_SELECTION;
    w->attention = sel->place.attention;
    snprintf(w->subject, sizeof w->subject, "%s", sel->subject);
    snprintf(w->application_id, sizeof w->application_id, "%s", sel->application_id);
    w->has_key = true;
    w->key_place = sel->place;
    w->key_slot = ma_slot_of(MA_SLOT_SELECTION);
    snprintf(w->selected_text, sizeof w->selected_text, "%s", sel->text);
    snprintf(w->surrounding, sizeof w->surrounding, "%s", sel->surrounding);
    w->editable = sel->editable;
    w->captured_at = sel->captured_at;
    w->fresh_for = MA_HANDOFF_FRESH_FOR;
}

void ma_store_note_world(ma_store *s, const ma_world *world, double now) {
    if (world->sense == MA_SENSE_SELECTION || !ma_world_is_fresh(world, now)) return;
    if (s->has_world && ma_world_is_fresh(&s->world, now) && s->world.captured_at > world->captured_at) return;
    s->world = *world;
    s->has_world = true;
}

bool ma_store_world(ma_store *s, double now, ma_world *out) {
    ma_selection sel;
    if (ma_store_selection(s, now, &sel)) {
        if (out) selection_world(&sel, out);
        return true;
    }
    if (!s->has_world || !ma_world_is_fresh(&s->world, now)) { s->has_world = false; return false; }
    if (out) *out = s->world;
    return true;
}

/* ---- readers ---- */

static void sort_facts(ma_fact *facts, int n, const ma_roster *r) {
    for (int i = 1; i < n; i++) {
        ma_fact f = facts[i];
        int j = i - 1;
        while (j >= 0 && ma_fact_ordered(&facts[j], &f, r) > 0) { facts[j + 1] = facts[j]; j--; }
        facts[j + 1] = f;
    }
}

int ma_store_facts(ma_store *s, double now, ma_fact *out, int max) {
    prune(s, now);
    int n = 0;
    for (int i = 0; i < s->fact_count && n < max; i++) {
        if (s->facts[i].slot.kind == MA_SLOT_SELECTION) continue;
        out[n++] = s->facts[i];
    }
    ma_selection sel;
    if (n < max && ma_store_selection(s, now, &sel)) selection_fact(&sel, &out[n++]);
    sort_facts(out, n, s->roster);
    return n;
}

int ma_store_facts_at(ma_store *s, const ma_place *place, double now, ma_fact *out, int max) {
    ma_fact *all = malloc((size_t)(FACTS_MAX + 1) * sizeof *all);
    if (!all) return 0;
    int n = ma_store_facts(s, now, all, FACTS_MAX + 1), kept = 0;
    for (int i = 0; i < n && kept < max; i++) if (ma_place_equal(&all[i].place, place)) out[kept++] = all[i];
    free(all);
    return kept;
}

bool ma_store_fact(ma_store *s, const ma_place *place, const ma_slot *slot, double now, ma_fact *out) {
    if (slot->kind == MA_SLOT_SELECTION) {
        ma_selection sel;
        if (!ma_store_selection(s, now, &sel) || !ma_place_equal(&sel.place, place)) return false;
        if (out) selection_fact(&sel, out);
        return true;
    }
    int i = find_fact(s, place, slot);
    if (i < 0 || ma_fact_is_expired(&s->facts[i], now)) return false;
    if (out) *out = s->facts[i];
    return true;
}

int ma_store_reads(ma_store *s, double now, ma_fact *out, int max) {
    ma_fact *all = malloc((size_t)(FACTS_MAX + 1) * sizeof *all);
    if (!all) return 0;
    int n = ma_store_facts(s, now, all, FACTS_MAX + 1), kept = 0;
    for (int i = 0; i < n && kept < max; i++) if (ma_slot_is_read(&all[i].slot)) out[kept++] = all[i];
    free(all);
    return kept;
}

int ma_store_count(const ma_store *s) { return s->fact_count; }

void ma_store_clear(ma_store *s) {
    const ma_roster *r = s->roster;
    free(s->route);
    memset(s, 0, sizeof *s);
    s->roster = r;
}
