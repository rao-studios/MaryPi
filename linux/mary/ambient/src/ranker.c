#include "ambient/ranker.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambient/classify.h"
#include "ambient/realm.h"

static const char *const STOP_WORDS[] = {
    "the", "a", "an", "and", "or", "but", "of", "to", "in", "on", "at", "for", "with", "about", "is", "are", "was", "were", "it", "this",
    "that", "these", "those", "my", "your", "our", "their", "me", "you", "i", "we", "they", "he", "she", "what", "which", "who", "how", "do",
    "does", "did", "can", "could", "would", "should", "please", "read", "tell", "say", "says", "said", "show", "give", "again", "just", "so", NULL,
};

static const char *const MODE_NAMES[] = { "relevance", "focusedWorld", "transformUnfocused" };
const char *ma_ranking_mode_name(ma_ranking_mode m) { return (unsigned)m < 3 ? MODE_NAMES[m] : NULL; }
bool ma_rendering_is_empty(const ma_rendering *r) { return !r->surface_count && !r->block_count && !r->mention_count; }

bool ma_is_stop_word(const char *word) {
    for (int i = 0; STOP_WORDS[i]; i++) if (strcmp(STOP_WORDS[i], word) == 0) return true;
    return false;
}

int ma_tokens(const char *text, char out[][32], int max) {
    char words[256][32];
    int n = ma_words(text, words, 256, false), count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (strlen(words[i]) <= 2 || ma_is_stop_word(words[i])) continue;
        bool dup = false;
        for (int k = 0; k < count && !dup; k++) dup = strcmp(out[k], words[i]) == 0;
        if (!dup) snprintf(out[count++], 32, "%s", words[i]);
    }
    return count;
}

static int overlap(char a[][32], int na, char b[][32], int nb) {
    int hits = 0;
    for (int i = 0; i < na; i++) for (int k = 0; k < nb; k++) if (strcmp(a[i], b[k]) == 0) { hits++; break; }
    return hits;
}

/* The text's words padded with spaces, so phrases match on word boundaries: " what s on my screen ". */
static void padded(const char *utterance, char *out, size_t n) {
    char words[256][32];
    int count = ma_words(utterance, words, 256, true);
    snprintf(out, n, " ");
    for (int i = 0; i < count; i++) { strncat(out, words[i], n - strlen(out) - 1); strncat(out, " ", n - strlen(out) - 1); }
}

bool ma_references_selection(const char *utterance) {
    char words[256][32];
    int n = ma_words(utterance, words, 256, true);
    for (int i = 0; i < n; i++)
        if (!strcmp(words[i], "highlight") || !strcmp(words[i], "highlighted") || !strcmp(words[i], "selection") || !strcmp(words[i], "selected")) return true;
    char text[4096];
    padded(utterance, text, sizeof text);
    static const char *const PHRASES[] = { " the part ", " that part ", " this part ", " the passage ", " that passage ", " this passage ", NULL };
    for (int i = 0; PHRASES[i]; i++) if (strstr(text, PHRASES[i])) return true;
    return false;
}

bool ma_is_deictic(const char *utterance) {
    char text[4096];
    padded(utterance, text, sizeof text);
    static const char *const PHRASES[] = {
        " this ", " these ", " here ", " right here ", " on screen", " on my screen", " on the screen", " in front of me", " looking at",
        " i'm reading", " im reading", " what i just ", " currently open", " open right now", " what i highlighted", " what i've highlighted",
        " what i have highlighted", " what i selected", " what i've selected", " what i have selected", NULL,
    };
    for (int i = 0; PHRASES[i]; i++) if (strstr(text, PHRASES[i])) return true;
    return ma_references_selection(utterance);
}

bool ma_references_application_anaphorically(const char *utterance) {
    char words[256][32];
    int n = ma_words(utterance, words, 256, true);
    char text[4096];
    padded(utterance, text, sizeof text);
    static const char *const PHRASES[] = { " that app ", " the app ", " same app ", " that application ", " the application ", " same application ", NULL };
    for (int i = 0; PHRASES[i]; i++) if (strstr(text, PHRASES[i])) return true;
    if (n && strcmp(words[0], "where") == 0) for (int i = 0; i < n; i++) if (strcmp(words[i], "it") == 0) return true;
    int i = 0;
    static const char *const LEADERS[] = { "and", "then", "now", "please", "just", "yeah", "yes", "yep", "ok", "okay", "sure", "so", "also", NULL };
    for (;;) {
        bool leader = false;
        for (int k = 0; i < n && LEADERS[k]; k++) if (strcmp(words[i], LEADERS[k]) == 0) leader = true;
        if (!leader) break;
        i++;
    }
    if (n - i >= 2 && ma_is_request_frame(words[i], words[i + 1])) {
        i += 2;
        if (i < n && strcmp(words[i], MA_POLITE_TAIL) == 0) i++;
    }
    static const char *const VERBS[] = {
        "add", "create", "do", "draw", "make", "move", "place", "put", "resize", "rename", "show",
        "rewrite", "revise", "edit", "change", "fix", "tighten", "shorten", "expand", "polish", "reword", "rephrase", "proofread", "correct",
        "refactor", "replace", "delete", "remove", "insert", "append", "translate", "reformat", "format", "tidy", "update",
        "write", "type", "compose", "draft", "jot", "note", "dictate", "title", "caption", "describe", "summarize", "summarise", NULL,
    };
    if (i >= n) return false;
    bool verb = false;
    for (int k = 0; VERBS[k]; k++) if (strcmp(words[i], VERBS[k]) == 0) verb = true;
    if (!verb) return false;
    for (int k = i + 1; k < n; k++)
        if (!strcmp(words[k], "it") || !strcmp(words[k], "there") || !strcmp(words[k], "another") || !strcmp(words[k], "here") || !strcmp(words[k], "this") || !strcmp(words[k], "that")) return true;
    return false;
}

bool ma_names_transform(const char *utterance, const ma_roster *r) { return ma_roster_names_transform(r, utterance); }
const char *ma_named_discipline(const char *utterance, const ma_roster *r) { return ma_roster_discipline_in(r, utterance); }

static void add_place(ma_place *out, int *n, int max, const ma_place *p) {
    for (int i = 0; i < *n; i++) if (ma_place_equal(&out[i], p)) return;
    if (*n < max) out[(*n)++] = *p;
}

int ma_explicitly_named_places(const char *utterance, const ma_roster *r, ma_place *out, int max) {
    int n = 0;
    for (int i = 0; i < r->app_count; i++) {
        if (!ma_registration_is_mentioned(&r->apps[i], utterance)) continue;
        ma_place p = ma_registration_place(&r->apps[i]);
        add_place(out, &n, max, &p);
    }
    return n;
}

int ma_named_places(const char *utterance, const ma_roster *r, ma_place *out, int max) {
    int n = 0;
    const char *discipline = ma_named_discipline(utterance, r);
    if (discipline) {
        /* The discipline cue, resolved by the realm resolver: every place with eyes that realizes it. */
        ma_realm_inputs in = { .utterance = utterance, .discipline = discipline, .roster = r };
        ma_need need;
        ma_realm_need(&in, &need);
        need.ability_count = 0;         /* AmbientNeed(discipline:) alone */
        ma_candidate candidates[MA_CANDIDATES_MAX];
        int count = ma_realm_candidates(&need, &in, candidates, MA_CANDIDATES_MAX);
        for (int i = 0; i < count; i++) if (candidates[i].has_eyes && candidates[i].conforms_by_discipline) add_place(out, &n, max, &candidates[i].place);
    }
    ma_place mentioned[MA_REGISTRATIONS_MAX];
    int m = ma_explicitly_named_places(utterance, r, mentioned, MA_REGISTRATIONS_MAX);
    for (int i = 0; i < m; i++) add_place(out, &n, max, &mentioned[i]);
    return n;
}

/* namedPlacesForRanking: the explicitly named places with eyes; failing any, the cue's places with eyes. */
static int named_for_ranking(const char *utterance, const ma_roster *r, ma_place *out, int max) {
    ma_place found[MA_REGISTRATIONS_MAX];
    int n = ma_explicitly_named_places(utterance, r, found, MA_REGISTRATIONS_MAX), kept = 0;
    for (int i = 0; i < n; i++) if (ma_place_has_eyes(&found[i], r)) add_place(out, &kept, max, &found[i]);
    if (kept) return kept;
    n = ma_named_places(utterance, r, found, MA_REGISTRATIONS_MAX);
    for (int i = 0; i < n; i++) if (ma_place_has_eyes(&found[i], r)) add_place(out, &kept, max, &found[i]);
    return kept;
}

static bool contains_place(const ma_place *places, int n, const ma_place *p) {
    for (int i = 0; i < n; i++) if (ma_place_equal(&places[i], p)) return true;
    return false;
}

bool ma_concerns_focused_place(const char *utterance, const ma_place *focused, const ma_roster *r) {
    ma_place named[MA_REGISTRATIONS_MAX];
    int n = named_for_ranking(utterance, r, named, MA_REGISTRATIONS_MAX);
    if (contains_place(named, n, focused)) return true;
    if (n) return false;
    return ma_is_deictic(utterance);
}

ma_ranking_mode ma_ranker_mode(const char *utterance, const ma_place *focused, const ma_roster *r) {
    if (!focused) return MA_RANKING_RELEVANCE;
    ma_place named[MA_REGISTRATIONS_MAX];
    int n = named_for_ranking(utterance, r, named, MA_REGISTRATIONS_MAX);
    if (ma_names_transform(utterance, r) && n && !contains_place(named, n, focused)) return MA_RANKING_TRANSFORM_UNFOCUSED;
    return ma_concerns_focused_place(utterance, focused, r) ? MA_RANKING_FOCUSED_WORLD : MA_RANKING_RELEVANCE;
}

/* ---- relevance ---- */

bool ma_concerns_eyeless(const ma_fact *f, const char *utterance, const ma_roster *r) {
    if (ma_slot_is_perceived(&f->slot) && ma_is_deictic(utterance)) return true;
    char wanted[64][32];
    int nw = ma_tokens(utterance, wanted, 64);
    if (!nw) return false;
    char searchable[MA_CONTENT_CAP + MA_SUBJECT_MAX + MA_TOKEN_MAX + MA_NAME_MAX + MA_PHRASE_MAX + 8], token[MA_TOKEN_MAX];
    ma_place_token(&f->place, token, sizeof token);
    snprintf(searchable, sizeof searchable, "%s %s %s %s %s", f->content, f->subject, token, ma_place_display_name(&f->place, r),
             f->slot.kind == MA_SLOT_NAMED_READ ? f->slot.phrase : "");
    char have[256][32];
    int nh = ma_tokens(searchable, have, 256);
    return overlap(wanted, nw, have, nh) > 0;
}

double ma_relevance(const ma_fact *f, const char *utterance, double now) {
    char wanted[64][32];
    int nw = ma_tokens(utterance, wanted, 64);
    double score = 0;
    if (nw) {
        char searchable[MA_CONTENT_CAP + MA_SUBJECT_MAX + 2 * MA_PHRASE_MAX + 8];
        snprintf(searchable, sizeof searchable, "%s %s %s %s", f->content, f->subject,
                 f->slot.kind == MA_SLOT_NAMED_READ ? f->slot.phrase : "", f->slot.kind == MA_SLOT_NAMED_READ ? f->slot.phrase : "");
        char have[256][32];
        int nh = ma_tokens(searchable, have, 256);
        score += 3.0 * overlap(wanted, nw, have, nh);
    }
    if (f->registration == MA_REGISTERED_ASKED_FOR) score += 1.5;
    switch (f->provenance) {
    case MA_PROVENANCE_LIVE_AX: score += 0.75; break;
    case MA_PROVENANCE_RECIPE_READ: score += 0.5; break;
    case MA_PROVENANCE_CACHED_BODY: score += 0.25; break;
    default: break;
    }
    score += 1.0 / (1.0 + ma_fact_age(f, now) / 60.0);
    if (!ma_fact_is_fresh(f, now)) score -= 0.5;
    return score;
}

struct scored { ma_fact fact; double score; bool attended; };

ma_ranking_mode ma_rank(ma_fact *facts, int n, const char *utterance, const ma_place *focused, const ma_world *world, double now, const ma_roster *r) {
    const ma_world *attention = world && ma_world_is_fresh(world, now) && ma_world_is_direct_reference(world) ? world : NULL;
    ma_ranking_mode mode = ma_ranker_mode(utterance, focused, r);
    struct scored *s = malloc((size_t)(n > 0 ? n : 1) * sizeof *s);
    if (!s) return mode;
    for (int i = 0; i < n; i++) {
        s[i].fact = facts[i];
        s[i].score = ma_relevance(&facts[i], utterance, now);
        s[i].attended = attention && ma_world_matches(attention, &facts[i]);
    }
    for (int i = 1; i < n; i++) {
        struct scored e = s[i];
        int j = i - 1;
        for (; j >= 0; j--) {
            const struct scored *l = &s[j];
            bool before;   /* does e sort before l? */
            if (l->attended != e.attended) before = e.attended;
            else if (mode == MA_RANKING_FOCUSED_WORLD && focused && ma_place_equal(&l->fact.place, focused) != ma_place_equal(&e.fact.place, focused)) before = ma_place_equal(&e.fact.place, focused);
            else if (l->score != e.score) before = e.score > l->score;
            else if (l->fact.captured_at != e.fact.captured_at) before = e.fact.captured_at > l->fact.captured_at;
            else before = ma_fact_ordered(&e.fact, &l->fact, r) < 0;
            if (!before) break;
            s[j + 1] = s[j];
        }
        s[j + 1] = e;
    }
    for (int i = 0; i < n; i++) facts[i] = s[i].fact;
    free(s);
    return mode;
}

/* ---- rendering ---- */

void ma_render(const ma_render_inputs *in, ma_rendering *out) {
    memset(out, 0, sizeof *out);
    int budget = in->budget > 0 ? in->budget : MA_VOICE_BUDGET, max_blocks = in->max_blocks > 0 ? in->max_blocks : MA_MAX_BLOCKS;
    const ma_world *attention = in->world && ma_world_is_fresh(in->world, in->now) && ma_world_is_direct_reference(in->world) ? in->world : NULL;
    ma_fact *candidates = malloc((size_t)(in->fact_count > 0 ? in->fact_count : 1) * sizeof *candidates);
    if (!candidates) return;
    int n = 0;
    for (int i = 0; i < in->fact_count; i++) {
        const ma_fact *f = &in->facts[i];
        bool attended = attention && ma_world_matches(attention, f);
        char key[MA_KEY_MAX];
        ma_fact_key(f, key, sizeof key);
        bool already = false;
        for (int k = 0; k < in->already_count && !already; k++) already = in->already_rendered[k] && strcmp(in->already_rendered[k], key) == 0;
        if (!attended && already) continue;
        if (!f->content[0]) continue;
        bool suppressed = false;
        for (int k = 0; k < in->suppressed_count && !suppressed; k++) suppressed = in->suppressed[k] && strstr(in->suppressed[k], f->content) != NULL;
        if (suppressed) continue;
        candidates[n++] = *f;
    }
    out->mode = ma_rank(candidates, n, in->utterance, in->focused, attention, in->now, in->roster);
    size_t spent = 0;
    for (int i = 0; i < in->surface_count && out->surface_count < MA_RENDER_SURFACES; i++) {
        const ma_surface *s = in->surfaces[i];
        if (!ma_surface_is_fresh(s, in->now)) continue;
        char line[MA_SURFACE_LINE_MAX];
        ma_surface_line(s, in->now, line, sizeof line);
        size_t len = ma_utf8_count(line);
        if (spent + len > (size_t)budget) continue;
        snprintf(out->surface_lines[out->surface_count++], MA_SURFACE_LINE_MAX, "%s", line);
        spent += len;
    }
    for (int i = 0; i < n; i++) {
        const ma_fact *f = &candidates[i];
        if (out->key_count < MA_RENDER_KEYS) ma_fact_key(f, out->keys[out->key_count++], MA_KEY_MAX);
        bool attended = attention && ma_world_matches(attention, f);
        bool focusable = ma_place_focus(&f->place, in->roster) != NULL;
        bool eyeless_aside = !focusable && f->registration != MA_REGISTERED_ASKED_FOR && !ma_concerns_eyeless(f, in->utterance, in->roster);
        bool mention_only = !attended && ((ma_slot_is_perceived(&f->slot) && focusable && !(in->focused && ma_place_equal(&f->place, in->focused))) || eyeless_aside);
        char block[MA_BLOCK_MAX];
        size_t room = spent < (size_t)budget ? (size_t)budget - spent : 0;
        ma_fact_block(f, in->now, in->roster, room, block, sizeof block);
        size_t bl = ma_utf8_count(block);
        bool room_for_block = !mention_only && (out->block_count == 0 || (out->block_count < max_blocks && spent + bl <= (size_t)budget));
        if (room_for_block && out->block_count < MA_MAX_BLOCKS) {
            snprintf(out->blocks[out->block_count++], MA_BLOCK_MAX, "%s", block);
            spent += bl;
        } else if (out->mention_count < MA_RENDER_MENTIONS) {
            ma_fact_mention_line(f, in->now, in->roster, out->mentions[out->mention_count++], MA_MENTION_MAX);
        }
    }
    free(candidates);
}

int ma_co_active_lines(const ma_place *places, const bool *glanced, int place_count, const ma_fact *facts, int fact_count,
                       int place_budget, int total_budget, double now, const ma_roster *r,
                       char lines[][MA_MENTION_MAX * 2], int max_lines, char keys[][MA_KEY_MAX], int max_keys, int *key_count) {
    int n = 0, remaining = total_budget;
    if (key_count) *key_count = 0;
    for (int p = 0; p < place_count && n < max_lines; p++) {
        if (remaining <= 0) break;
        /* This place's facts, newest first. */
        const ma_fact *mine[64];
        int mn = 0;
        for (int i = 0; i < fact_count && mn < 64; i++) {
            if (!ma_place_equal(&facts[i].place, &places[p])) continue;
            int pos = mn;
            while (pos > 0 && mine[pos - 1]->captured_at < facts[i].captured_at) { mine[pos] = mine[pos - 1]; pos--; }
            mine[pos] = &facts[i];
            mn++;
        }
        char line[MA_MENTION_MAX * 2];
        snprintf(line, sizeof line, "\xE2\x80\x94 %s%s:", ma_place_display_name(&places[p], r), glanced && glanced[p] ? " (glanced)" : "");
        size_t spent = ma_utf8_count(line);
        int carried = 0;
        for (int i = 0; i < mn; i++) {
            char mention[MA_MENTION_MAX + 1];
            mention[0] = ' ';
            ma_fact_mention_line(mine[i], now, r, mention + 1, sizeof mention - 1);
            size_t ml = ma_utf8_count(mention);
            if (spent + ml > (size_t)place_budget || remaining - (int)(spent + ml) < 0) break;
            strncat(line, mention, sizeof line - strlen(line) - 1);
            spent += ml;
            if (keys && key_count && *key_count < max_keys) ma_fact_key(mine[i], keys[(*key_count)++], MA_KEY_MAX);
            carried++;
        }
        if (!carried) strncat(line, glanced && glanced[p] ? " you just looked at it." : " the user was just working there.", sizeof line - strlen(line) - 1);
        snprintf(lines[n++], MA_MENTION_MAX * 2, "%s", line);
        remaining -= (int)ma_utf8_count(line);
    }
    return n;
}
