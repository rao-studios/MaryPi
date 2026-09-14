#include "brain/triage.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ambient/classify.h"
#include "ambient/place.h"
#include "common/base64.h"
#include "common/json.h"
#include "common/service.h"

/* ---- the seam ---- */

int mb_embed_sewn(const char *const *texts, size_t n, float **out, size_t *dim, char *message, size_t cap, void *user) {
    const char *path = user ? (const char *)user : mc_service_default_socket("SEWN_SOCKET", "/run/sewn/sewn.sock");
    int fd = mc_service_connect(path);
    if (fd < 0) { snprintf(message, cap, "sewnd could not be reached: %s", strerror(-fd)); return fd; }
    struct json_object *req = json_object_new_object(), *arr = json_object_new_array(), *reply = NULL;
    json_object_object_add(req, "type", json_object_new_string("embed"));
    for (size_t i = 0; i < n; i++) json_object_array_add(arr, json_object_new_string(texts[i]));
    json_object_object_add(req, "texts", arr);
    json_object_object_add(req, "purpose", json_object_new_string("route"));
    int rc = mc_service_call(fd, req, &reply);
    close(fd);
    json_object_put(req);
    if (rc < 0) { snprintf(message, cap, "sewnd: %s", strerror(-rc)); return rc; }
    const char *type = mc_json_type(reply);
    if (!type || strcmp(type, "embed.result") != 0) {
        const char *why = mc_json_string(reply, "message");
        snprintf(message, cap, "%s", why ? why : "sewnd could not embed");
        json_object_put(reply);
        return -EIO;
    }
    int64_t d = 0;
    struct json_object *vectors = mc_json_array(reply, "vectors_b64");
    if (!mc_json_int64(reply, "dim", &d) || d <= 0 || !vectors || json_object_array_length(vectors) != n) {
        snprintf(message, cap, "sewnd's embedding could not be read");
        json_object_put(reply);
        return -EBADMSG;
    }
    float *all = calloc(n * (size_t)d, sizeof *all);
    if (!all) { json_object_put(reply); return -ENOMEM; }
    for (size_t i = 0; i < n; i++) {
        const char *b64 = json_object_get_string(json_object_array_get_idx(vectors, i));
        size_t len = 0;
        unsigned char *bytes = b64 ? mc_base64_decode_alloc(b64, strlen(b64), &len) : NULL;
        if (bytes && len == (size_t)d * sizeof(float)) memcpy(all + i * (size_t)d, bytes, len);
        free(bytes);
    }
    json_object_put(reply);
    *out = all;
    *dim = (size_t)d;
    return 0;
}

/* ---- the index ---- */

void mb_skill_index_text(const sk_app *app, const sk_skill *skill, char *out, size_t n) {
    snprintf(out, n, "%s. %s", skill->title, skill->summary);
    for (size_t i = 0; i < skill->phrase_count; i++) { strncat(out, " ", n - strlen(out) - 1); strncat(out, skill->phrases[i], n - strlen(out) - 1); strncat(out, ".", n - strlen(out) - 1); }
    if (skill->trigger_count) {
        strncat(out, " ", n - strlen(out) - 1);
        for (size_t i = 0; i < skill->trigger_count; i++) { if (i) strncat(out, ", ", n - strlen(out) - 1); strncat(out, skill->triggers[i], n - strlen(out) - 1); }
        strncat(out, ".", n - strlen(out) - 1);
    }
    (void)app;
}

void mb_skill_index_free(mb_skill_index *index) {
    for (size_t i = 0; i < index->n; i++) free(index->items[i].vector);
    free(index->items);
    memset(index, 0, sizeof *index);
}

int mb_skill_index_build(mb_skill_index *index, const sk_registry *registry, mb_embed_fn embed, void *user, char *message, size_t cap) {
    mb_skill_index_free(index);
    size_t total = 0;
    for (size_t a = 0; a < registry->app_count; a++) total += registry->apps[a].skill_count;
    if (!total) return 0;
    char **texts = calloc(total, sizeof *texts);
    mb_skill_vector *items = calloc(total, sizeof *items);
    if (!texts || !items) { free(texts); free(items); return -ENOMEM; }
    size_t n = 0;
    for (size_t a = 0; a < registry->app_count; a++) {
        const sk_app *app = &registry->apps[a];
        for (size_t s = 0; s < app->skill_count; s++) {
            const sk_skill *skill = &app->skills[s];
            char text[2048];
            mb_skill_index_text(app, skill, text, sizeof text);
            texts[n] = strdup(text);
            snprintf(items[n].app, sizeof items[n].app, "%s", app->id);
            snprintf(items[n].skill, sizeof items[n].skill, "%s", skill->id);
            snprintf(items[n].invocation, sizeof items[n].invocation, "%s", skill->invocation);
            n++;
        }
    }
    float *vectors = NULL;
    size_t dim = 0;
    int rc = embed((const char *const *)texts, n, &vectors, &dim, message, cap, user);
    for (size_t i = 0; i < n; i++) free(texts[i]);
    free(texts);
    if (rc < 0) { free(items); return rc; }
    for (size_t i = 0; i < n; i++) {
        items[i].vector = malloc(dim * sizeof(float));
        if (items[i].vector) memcpy(items[i].vector, vectors + i * dim, dim * sizeof(float));
    }
    free(vectors);
    index->items = items;
    index->n = n;
    index->dim = dim;
    return 0;
}

static float cosine(const float *a, const float *b, size_t dim) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < dim; i++) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    return na > 0 && nb > 0 ? (float)(dot / (sqrt(na) * sqrt(nb))) : 0;
}

int mb_affinities(const mb_skill_index *index, const float *query, mb_affinity *out, int max) {
    int n = 0;
    for (size_t i = 0; i < index->n && n < max; i++) {
        if (!index->items[i].vector) continue;
        mb_affinity a = { .skill = &index->items[i], .score = cosine(query, index->items[i].vector, index->dim) };
        int pos = n;
        while (pos > 0 && out[pos - 1].score < a.score) { out[pos] = out[pos - 1]; pos--; }
        out[pos] = a;
        n++;
    }
    return n;
}

const mb_skill_vector *mb_unique_winner(const mb_affinity *affinities, int n, const sk_registry *registry, float floor, float margin) {
    const mb_skill_vector *first = NULL;
    float first_score = 0, second_score = -1;
    for (int i = 0; i < n; i++) {
        if (affinities[i].score < floor) continue;
        sk_decision decision = sk_registry_decide(registry, affinities[i].skill->app, affinities[i].skill->skill);
        if (decision != SK_ALLOWED && decision != SK_NEEDS_CONFIRMATION) continue;
        if (!first) { first = affinities[i].skill; first_score = affinities[i].score; }
        else { second_score = affinities[i].score; break; }
    }
    if (!first) return NULL;
    if (second_score >= 0 && first_score - second_score < margin) return NULL;
    return first;
}

/* ---- the shape ---- */

static const char *const SHAPE_NAMES[] = { "none", "noRequiredArguments", "singleString", "singleEnum" };
const char *mb_confidence_shape_name(mb_confidence_shape s) { return (unsigned)s < 4 ? SHAPE_NAMES[s] : "none"; }

/* The parameters of a skill's JSON Schema: properties, required. */
static struct json_object *properties_of(const sk_skill *skill) { return skill->params ? mc_json_object(skill->params, "properties") : NULL; }

static bool is_required(const sk_skill *skill, const char *name) {
    struct json_object *required = skill->params ? mc_json_array(skill->params, "required") : NULL;
    for (size_t i = 0; required && i < json_object_array_length(required); i++) {
        const char *r = json_object_get_string(json_object_array_get_idx(required, i));
        if (r && strcmp(r, name) == 0) return true;
    }
    return false;
}

static bool requires_composition(struct json_object *property) {
    bool b = false;
    return mc_json_bool(property, "x-composition", &b) && b;
}

mb_confidence_shape mb_confidence_shape_of(const sk_skill *skill, const char *utterance) {
    struct json_object *props = properties_of(skill);
    int required = 0;
    const char *only = NULL;
    struct json_object *only_prop = NULL;
    if (props) {
        json_object_object_foreach(props, name, prop) {
            if (!is_required(skill, name)) continue;
            required++;
            only = name;
            only_prop = prop;
        }
    }
    if (!required) return MB_SHAPE_NO_REQUIRED_ARGUMENTS;
    if (required != 1) return MB_SHAPE_NONE;
    const char *type = mc_json_string(only_prop, "type");
    if (!type || strcmp(type, "string") != 0 || requires_composition(only_prop)) return MB_SHAPE_NONE;
    if (mc_json_array(only_prop, "enum")) {
        char value[64], spoken[64];
        return mb_spoken_enum(skill, only, utterance ? utterance : "", value, sizeof value, spoken, sizeof spoken) ? MB_SHAPE_SINGLE_ENUM : MB_SHAPE_NONE;
    }
    return MB_SHAPE_SINGLE_STRING;
}

bool mb_is_single_clause(const char *utterance) {
    char trimmed[2048];
    snprintf(trimmed, sizeof trimmed, "%s", utterance ? utterance : "");
    size_t len = strlen(trimmed);
    while (len && isspace((unsigned char)trimmed[len - 1])) trimmed[--len] = 0;
    if (len && trimmed[len - 1] == '?') { trimmed[--len] = 0; while (len && isspace((unsigned char)trimmed[len - 1])) trimmed[--len] = 0; }
    const char *start = trimmed;
    while (isspace((unsigned char)*start)) start++;
    if (!*start) return false;
    int words = 0;
    bool in = false;
    for (const char *c = start; *c; c++) { if (isspace((unsigned char)*c)) in = false; else if (!in) { in = true; words++; } }
    if (words > MB_SINGLE_CLAUSE_WORDS) return false;
    if (strchr(start, ';') || strchr(start, ',') || strchr(start, '?')) return false;
    char padded[2100] = " ";
    for (const char *c = start; *c; c++) { char w[2] = { (char)tolower((unsigned char)*c), 0 }; if (isspace((unsigned char)*c)) w[0] = ' '; strncat(padded, w, sizeof padded - strlen(padded) - 1); }
    strncat(padded, " ", sizeof padded - strlen(padded) - 1);
    static const char *const JOINERS[] = { " and ", " then ", " also ", " after that ", " plus ", NULL };
    for (int i = 0; JOINERS[i]; i++) if (strstr(padded, JOINERS[i])) return false;
    return true;
}

/* ---- the spoken extractors ---- */

/* " normalized " — lowercase words separated by single spaces, padded. */
static void normalized(const char *text, char *out, size_t n) {
    char words[128][32];
    int count = ma_words(text, words, 128, true);
    snprintf(out, n, " ");
    for (int i = 0; i < count; i++) { strncat(out, words[i], n - strlen(out) - 1); strncat(out, " ", n - strlen(out) - 1); }
}

static void replace_once(char *text, const char *needle, size_t cap) {
    char *at = strstr(text, needle);
    if (!at) return;
    size_t nl = strlen(needle);
    memmove(at + 1, at + nl, strlen(at + nl) + 1);
    at[0] = ' ';
    (void)cap;
}

bool mb_spoken_enum(const sk_skill *skill, const char *parameter, const char *utterance, char *value, size_t vn, char *spoken_as, size_t sn) {
    struct json_object *props = properties_of(skill), *prop = props ? mc_json_object(props, parameter) : NULL, *values = prop ? mc_json_array(prop, "enum") : NULL;
    if (!values || !json_object_array_length(values)) return false;
    const char *aliases[1] = { NULL };
    char stripped[2048], text[2200];
    ma_edit_strip_preamble(utterance, aliases, 0, stripped, sizeof stripped);
    normalized(stripped, text, sizeof text);
    if (strlen(text) <= 2) return false;
    /* every (phrase → value): the value's own name, then its spoken words; longest first */
    struct { char phrase[96]; char value[64]; } candidates[128];
    int n = 0;
    struct json_object *spoken = skill->spoken ? mc_json_object(skill->spoken, parameter) : NULL;
    for (size_t i = 0; i < json_object_array_length(values) && n < 128; i++) {
        const char *v = json_object_get_string(json_object_array_get_idx(values, i));
        if (!v) continue;
        char spelled[96];
        normalized(v, spelled, sizeof spelled);
        if (strlen(spelled) > 2) { snprintf(candidates[n].phrase, 96, "%s", spelled); snprintf(candidates[n].value, 64, "%s", v); n++; }
        struct json_object *words = spoken ? mc_json_array(spoken, v) : NULL;
        for (size_t k = 0; words && k < json_object_array_length(words) && n < 128; k++) {
            char phrase[96];
            normalized(json_object_get_string(json_object_array_get_idx(words, k)), phrase, sizeof phrase);
            if (strlen(phrase) <= 2 || strcmp(phrase, spelled) == 0) continue;
            snprintf(candidates[n].phrase, 96, "%s", phrase);
            snprintf(candidates[n].value, 64, "%s", v);
            n++;
        }
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0; j--) {
            size_t a = strlen(candidates[j - 1].phrase), b = strlen(candidates[j].phrase);
            if (a > b || (a == b && strcmp(candidates[j - 1].phrase, candidates[j].phrase) <= 0)) break;
            char tp[96], tv[64];
            memcpy(tp, candidates[j - 1].phrase, 96); memcpy(tv, candidates[j - 1].value, 64);
            memcpy(candidates[j - 1].phrase, candidates[j].phrase, 96); memcpy(candidates[j - 1].value, candidates[j].value, 64);
            memcpy(candidates[j].phrase, tp, 96); memcpy(candidates[j].value, tv, 64);
        }
    }
    char hits_value[4][64], hits_spoken[4][96];
    int hits = 0;
    for (int i = 0; i < n; i++) {
        if (!strstr(text, candidates[i].phrase)) continue;
        replace_once(text, candidates[i].phrase, sizeof text);
        bool same = false;
        for (int h = 0; h < hits && !same; h++) same = strcmp(hits_value[h], candidates[i].value) == 0;
        if (same) continue;
        if (hits < 4) { snprintf(hits_value[hits], 64, "%s", candidates[i].value); snprintf(hits_spoken[hits], 96, "%s", candidates[i].phrase); hits++; }
    }
    if (hits != 1) return false;
    snprintf(value, vn, "%s", hits_value[0]);
    /* the phrase without its padding */
    const char *p = hits_spoken[0];
    while (*p == ' ') p++;
    snprintf(spoken_as, sn, "%s", p);
    size_t sl = strlen(spoken_as);
    while (sl && spoken_as[sl - 1] == ' ') spoken_as[--sl] = 0;
    return true;
}

/* Word tokens with their byte ranges, so a peel slices the original text. */
struct token { size_t start, end; char lower[32]; };

static int tokenize(const char *text, struct token *out, int max) {
    int n = 0;
    size_t i = 0;
    while (text[i]) {
        while (text[i] && !(isalpha((unsigned char)text[i]) || (unsigned char)text[i] >= 0x80)) i++;
        if (!text[i]) break;
        size_t start = i;
        while (text[i] && (isalpha((unsigned char)text[i]) || (unsigned char)text[i] >= 0x80)) i++;
        if (n < max) {
            out[n].start = start;
            out[n].end = i;
            size_t len = i - start < 31 ? i - start : 31;
            for (size_t k = 0; k < len; k++) out[n].lower[k] = (char)tolower((unsigned char)text[start + k]);
            out[n].lower[len] = 0;
            n++;
        }
    }
    return n;
}

/* One of the skill's own single-word triggers. */
static bool skill_word(const sk_skill *skill, const char *word) {
    for (size_t t = 0; skill && t < skill->trigger_count; t++) {
        char words[16][32];
        if (ma_words(skill->triggers[t], words, 16, false) == 1 && strcmp(words[0], word) == 0) return true;
    }
    return false;
}

static bool alias_word(const sk_app *app, const char *word) {
    if (!app) return false;
    char words[8][32];
    int n = ma_words(app->id, words, 8, false);
    for (int i = 0; i < n; i++) if (strcmp(words[i], word) == 0) return true;
    n = ma_words(app->name ? app->name : "", words, 8, false);
    for (int i = 0; i < n; i++) if (strcmp(words[i], word) == 0) return true;
    for (size_t a = 0; a < app->alias_count; a++) {
        n = ma_words(app->aliases[a], words, 8, false);
        for (int i = 0; i < n; i++) if (strcmp(words[i], word) == 0) return true;
    }
    return false;
}

static void trim_copy(const char *s, char *out, size_t n) {
    while (isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) len--;
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}

static void note(char *stages, size_t cap, const char *label, const char *from, const char *to) {
    if (!stages || strcmp(from, to) == 0) return;
    char line[600];
    snprintf(line, sizeof line, "%s%s: \"%s\"", stages[0] ? "\n" : "", label, to);
    strncat(stages, line, cap - strlen(stages) - 1);
}

void mb_spoken_span(const char *utterance, const sk_skill *skill, const sk_app *app, char *out, size_t n, char *stages, size_t stages_cap) {
    if (stages && stages_cap) stages[0] = 0;
    /* 1. the preamble */
    const char *aliases[MA_ALIASES_MAX + 2];
    int alias_count = 0;
    if (app) {
        aliases[alias_count++] = app->id;
        for (size_t a = 0; a < app->alias_count && alias_count < MA_ALIASES_MAX + 2; a++) aliases[alias_count++] = app->aliases[a];
    }
    char stage1[2048], base[2048];
    ma_edit_strip_preamble(utterance, aliases, alias_count, stage1, sizeof stage1);
    trim_copy(stage1, base, sizeof base);
    if (!base[0]) { snprintf(out, n, "%s", utterance); return; }
    note(stages, stages_cap, "preamble", utterance, base);
    /* 2. "open <app> and" */
    struct token tokens[128];
    int count = tokenize(base, tokens, 128);
    int cursor = 0;
    if (count && (strcmp(tokens[0].lower, "open") == 0 || strcmp(tokens[0].lower, "launch") == 0 || strcmp(tokens[0].lower, "switch") == 0)) {
        cursor = 1;
        if (cursor < count && strcmp(tokens[cursor].lower, "to") == 0) cursor++;
        int alias_start = cursor;
        while (cursor < count && alias_word(app, tokens[cursor].lower)) cursor++;
        if (cursor > alias_start && cursor < count && strcmp(tokens[cursor].lower, "and") == 0 && cursor + 1 < count) {
            char next[2048];
            trim_copy(base + tokens[cursor + 1].start, next, sizeof next);
            if (next[0]) { note(stages, stages_cap, "open clause", base, next); snprintf(base, sizeof base, "%s", next); count = tokenize(base, tokens, 128); }
        }
    }
    /* 3. one leading trigger phrase (longest first), else one token */
    {
        const char *best = NULL;
        int best_words = 0;
        char words[16][32];
        for (size_t p = 0; p < skill->phrase_count; p++) {
            int pw = ma_words(skill->phrases[p], words, 16, false);
            if (pw > count || pw <= best_words) continue;
            bool match = true;
            for (int i = 0; i < pw && match; i++) match = strcmp(tokens[i].lower, words[i]) == 0;
            if (match) { best = skill->phrases[p]; best_words = pw; }
        }
        if (!best) {
            for (size_t t = 0; t < skill->trigger_count && count; t++) {
                int tw = ma_words(skill->triggers[t], words, 16, false);
                if (tw == 1 && strcmp(tokens[0].lower, words[0]) == 0) { best = skill->triggers[t]; best_words = 1; break; }
            }
        }
        if (best && best_words < count) {
            char next[2048];
            trim_copy(base + tokens[best_words].start, next, sizeof next);
            if (next[0]) { note(stages, stages_cap, "trigger", base, next); snprintf(base, sizeof base, "%s", next); count = tokenize(base, tokens, 128); }
        }
    }
    /* 4. a trailing "in/on <app>" */
    if (count >= 2) {
        int i = count - 1;
        while (i > 0 && alias_word(app, tokens[i].lower)) i--;
        if (i < count - 1 && (strcmp(tokens[i].lower, "in") == 0 || strcmp(tokens[i].lower, "on") == 0) && i > 0) {
            char next[2048];
            snprintf(next, sizeof next, "%.*s", (int)tokens[i].start, base);
            trim_copy(next, next, sizeof next);
            if (next[0]) { note(stages, stages_cap, "trailing app", base, next); snprintf(base, sizeof base, "%s", next); }
        }
    }
    /* 5. a trailing "in/into/as a <the skill's own words>": "hello world in a new note" → "hello world" */
    if (count >= 2) {
        int i = count - 1, skill_words = 0;
        while (i > 0 && skill_word(skill, tokens[i].lower)) { i--; skill_words++; }
        if (skill_words) {
            if (i > 0 && (strcmp(tokens[i].lower, "a") == 0 || strcmp(tokens[i].lower, "an") == 0 || strcmp(tokens[i].lower, "the") == 0)) i--;
            if (i > 0 && (strcmp(tokens[i].lower, "in") == 0 || strcmp(tokens[i].lower, "into") == 0 || strcmp(tokens[i].lower, "as") == 0 || strcmp(tokens[i].lower, "to") == 0)) {
                char next[2048];
                snprintf(next, sizeof next, "%.*s", (int)tokens[i].start, base);
                trim_copy(next, next, sizeof next);
                if (next[0]) { note(stages, stages_cap, "trailing words", base, next); snprintf(base, sizeof base, "%s", next); }
            }
        }
    }
    snprintf(out, n, "%s", base);
}

struct json_object *mb_confidence_arguments(const sk_skill *skill, const sk_app *app, const char *utterance, char *stages, size_t stages_cap) {
    struct json_object *args = json_object_new_object(), *props = properties_of(skill);
    if (stages && stages_cap) stages[0] = 0;
    if (!props) return args;
    const char *required_name = NULL;
    struct json_object *required_prop = NULL;
    int required = 0;
    json_object_object_foreach(props, name, prop) {
        if (is_required(skill, name)) { required++; required_name = name; required_prop = prop; }
    }
    if (required == 1 && mc_json_string(required_prop, "type") && strcmp(mc_json_string(required_prop, "type"), "string") == 0) {
        if (!mc_json_array(required_prop, "enum")) {
            char span[2048];
            mb_spoken_span(utterance, skill, app, span, sizeof span, stages, stages_cap);
            json_object_object_add(args, required_name, json_object_new_string(span));
        } else {
            char value[64], spoken[64];
            if (mb_spoken_enum(skill, required_name, utterance, value, sizeof value, spoken, sizeof spoken)) {
                json_object_object_add(args, required_name, json_object_new_string(value));
                if (stages) { char line[200]; snprintf(line, sizeof line, "%senum %s: \"%s\" -> %s", stages[0] ? "\n" : "", required_name, spoken, value); strncat(stages, line, stages_cap - strlen(stages) - 1); }
            }
        }
    }
    /* an optional enum the person named is still a thing they said */
    json_object_object_foreach(props, oname, oprop) {
        if (is_required(skill, oname) || !mc_json_array(oprop, "enum") || mc_json_string(args, oname)) continue;
        char value[64], spoken[64];
        if (!mb_spoken_enum(skill, oname, utterance, value, sizeof value, spoken, sizeof spoken)) continue;
        json_object_object_add(args, oname, json_object_new_string(value));
        if (stages) { char line[200]; snprintf(line, sizeof line, "%senum %s: \"%s\" -> %s", stages[0] ? "\n" : "", oname, spoken, value); strncat(stages, line, stages_cap - strlen(stages) - 1); }
    }
    return args;
}

/* ---- the deterministic tier ---- */

/* A read never makes an action turn on its own: "what is on my calendar" is a question with a skill behind it,
 * and the lane runs it when it wins without a nudge to act further. */
static bool acts(const sk_registry *registry, const char *app, const char *skill_id) {
    const sk_skill *skill = registry ? sk_registry_skill(registry, app, skill_id) : NULL;
    return skill && skill->effect != SK_EFFECT_READ;
}

bool mb_question_shaped(const char *utterance) {
    static const char *const ASKS[] = { "what", "who", "whom", "where", "when", "why", "how", "which", "whose",
                                        "is", "are", "was", "were", "do", "does", "did", "have", "has", "had", "am", NULL };
    if (!utterance) return false;
    char stripped[2048], base[2048];
    ma_edit_strip_preamble(utterance, NULL, 0, stripped, sizeof stripped);
    trim_copy(stripped, base, sizeof base);
    struct token tokens[2];
    if (tokenize(base[0] ? base : utterance, tokens, 2) < 1) return false;
    for (int i = 0; ASKS[i]; i++) if (strcmp(tokens[0].lower, ASKS[i]) == 0) return true;
    return false;
}

bool mb_action_shaped(const mb_affinity *affinities, int n, const char *utterance, const sk_registry *registry) {
    if (mb_question_shaped(utterance)) return false;
    if (n > 0 && affinities[0].skill && affinities[0].score >= MB_ACTION_FLOOR && acts(registry, affinities[0].skill->app, affinities[0].skill->skill)) return true;
    if (!utterance || !registry) return false;
    char stripped[2048], base[2048];
    ma_edit_strip_preamble(utterance, NULL, 0, stripped, sizeof stripped);
    trim_copy(stripped, base, sizeof base);
    struct token tokens[128];
    int count = tokenize(base[0] ? base : utterance, tokens, 128);
    if (count < 1) return false;
    for (size_t a = 0; a < registry->app_count; a++) {
        const sk_app *app = &registry->apps[a];
        if (!app->enabled) continue;
        for (size_t k = 0; k < app->skill_count; k++) {
            const sk_skill *skill = &app->skills[k];
            if (skill->effect == SK_EFFECT_READ) continue;
            for (size_t t = 0; t < skill->trigger_count; t++) {
                char words[16][32];
                if (ma_words(skill->triggers[t], words, 16, false) == 1 && strcmp(tokens[0].lower, words[0]) == 0) return true;
            }
        }
    }
    return false;
}

int mb_deterministic_decision(const char *utterance) {
    static const char *const YES[] = { "yes", "yeah", "yep", "yup", "sure", "ok", "okay", "confirm", "proceed", "do it", "go ahead", "go for it", "please do",
        "yes please", "sounds good", "yes go ahead", "okay do it", "yes do it", "yes proceed", "sure go ahead", "okay go ahead", NULL };
    static const char *const NO[] = { "no", "nope", "cancel", "stop", "dont", "do not", "no thanks", "never mind", "nevermind", "leave it", "cancel it",
        "no cancel", "dont do it", "no stop", "cancel that", NULL };
    /* lowercase letters and spaces only, single-spaced */
    char text[512];
    size_t o = 0;
    bool space = false;
    for (const char *c = utterance ? utterance : ""; *c && o + 1 < sizeof text; c++) {
        if (isalpha((unsigned char)*c)) { if (space && o) text[o++] = ' '; space = false; if (o + 1 < sizeof text) text[o++] = (char)tolower((unsigned char)*c); }
        else if (*c == ' ' || *c == '\t' || *c == '\n') space = true;
    }
    text[o] = 0;
    for (int i = 0; YES[i]; i++) if (strcmp(text, YES[i]) == 0) return 1;
    for (int i = 0; NO[i]; i++) if (strcmp(text, NO[i]) == 0) return 0;
    return -1;
}
