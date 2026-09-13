#include "ambient/place.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/json.h"

static const char *const ATTENTION_NAMES[MA_ATTENTION_COUNT] = { "applications", "mac", "system", "window-management", "typer" };
static const char *const ATTENTION_DISPLAY[MA_ATTENTION_COUNT] = { "Applications", "Mac", "System", "Window Management", "Typer" };

const char *ma_attention_name(ma_attention a) { return (unsigned)a < MA_ATTENTION_COUNT ? ATTENTION_NAMES[a] : NULL; }
const char *ma_attention_display_name(ma_attention a) { return (unsigned)a < MA_ATTENTION_COUNT ? ATTENTION_DISPLAY[a] : NULL; }
bool ma_attention_is_invocable(ma_attention a) { return a != MA_ATTENTION_APPLICATIONS && (unsigned)a < MA_ATTENTION_COUNT; }

bool ma_attention_from_name(const char *name, ma_attention *out) {
    if (!name) return false;
    for (int i = 0; i < MA_ATTENTION_COUNT; i++) {
        if (strcasecmp(name, ATTENTION_NAMES[i]) == 0) {
            if (out) *out = (ma_attention)i;
            return true;
        }
    }
    return false;
}

ma_place ma_place_lane(ma_attention a) {
    ma_place p = { .attention = a };
    return p;
}

ma_place ma_place_application(const char *id) {
    ma_place p = { .attention = MA_ATTENTION_APPLICATIONS };
    if (id) snprintf(p.application, sizeof p.application, "%s", id);
    return p;
}

bool ma_place_is_application(const ma_place *p) { return p && p->application[0]; }

bool ma_place_equal(const ma_place *a, const ma_place *b) {
    if (!a || !b) return false;
    if (ma_place_is_application(a) || ma_place_is_application(b)) return strcmp(a->application, b->application) == 0;
    return a->attention == b->attention;
}

void ma_place_token(const ma_place *p, char *out, size_t n) {
    if (ma_place_is_application(p)) snprintf(out, n, "%s:%s", ATTENTION_NAMES[MA_ATTENTION_APPLICATIONS], p->application);
    else snprintf(out, n, "%s", ma_attention_name(p->attention) ? ma_attention_name(p->attention) : "");
}

const char *ma_place_memory_token(const ma_place *p) {
    return ma_place_is_application(p) ? p->application : ma_attention_name(p->attention);
}

bool ma_place_from_token(const char *token, ma_place *out) {
    if (!token || !*token) return false;
    const char *colon = strchr(token, ':');
    if (colon) {
        size_t head = (size_t)(colon - token);
        if (head != strlen(ATTENTION_NAMES[MA_ATTENTION_APPLICATIONS]) || strncmp(token, ATTENTION_NAMES[MA_ATTENTION_APPLICATIONS], head) != 0) return false;
        if (!colon[1]) return false;
        if (out) *out = ma_place_application(colon + 1);
        return true;
    }
    ma_attention a;
    if (!ma_attention_from_name(token, &a)) return false;
    if (out) *out = ma_place_lane(a);
    return true;
}

int ma_place_compare(const ma_place *a, const ma_place *b) {
    char ta[MA_TOKEN_MAX], tb[MA_TOKEN_MAX];
    ma_place_token(a, ta, sizeof ta);
    ma_place_token(b, tb, sizeof tb);
    return strcmp(ta, tb);
}

/* ---- words ---- */

int ma_words(const char *text, char out[][32], int max, bool keep_apostrophe) {
    int n = 0, len = 0;
    if (!text) return 0;
    for (const unsigned char *c = (const unsigned char *)text;; c++) {
        bool in = *c && (isalnum(*c) || *c >= 0x80 || (keep_apostrophe && (*c == '\'' || *c == 0xE2)));
        /* U+2019 (E2 80 99) is an apostrophe too; its continuation bytes are ≥ 0x80 and ride along. */
        if (in) {
            if (n < max && len < 31) out[n][len++] = (char)tolower(*c);
            continue;
        }
        if (len) {
            if (n < max) out[n][len] = 0, n++;
            len = 0;
        }
        if (!*c) break;
    }
    return n;
}

/* ---- the roster ---- */

static const char *const PARADIGM_NAMES[] = { "discipline", "applicationExpertise", "systemControl" };
const char *ma_paradigm_name(ma_paradigm p) { return (unsigned)p < 3 ? PARADIGM_NAMES[p] : NULL; }

void ma_roster_clear(ma_roster *r) { memset(r, 0, sizeof *r); }

static ma_ability *add_ability(ma_roster *r, const char *id, ma_paradigm paradigm) {
    if (r->ability_count >= MA_ABILITIES_MAX) return NULL;
    ma_ability *a = &r->abilities[r->ability_count++];
    memset(a, 0, sizeof *a);
    snprintf(a->id, sizeof a->id, "%s", id);
    a->paradigm = paradigm;
    return a;
}

static void set_words(const char **slots, const char *const *words) {
    int i = 0;
    for (; words[i] && i < MA_ABILITY_TRIGGERS_MAX - 1; i++) slots[i] = words[i];
    slots[i] = NULL;
}

/* The transform family every craft that revises authors for itself (AmbientRanker's continuation verbs). */
static const char *const TRANSFORMS[] = {
    "rewrite", "revise", "edit", "change", "fix", "tighten", "shorten", "expand", "polish", "reword", "rephrase",
    "proofread", "correct", "refactor", "replace", "delete", "remove", "insert", "append", "translate", "reformat",
    "format", "tidy", "update", "condense", "add", NULL,
};

ma_registration *ma_roster_add(ma_roster *r, const char *id, const char *name) {
    ma_registration *reg = (ma_registration *)ma_roster_registration(r, id);
    if (!reg) {
        if (r->app_count >= MA_REGISTRATIONS_MAX) return NULL;
        reg = &r->apps[r->app_count++];
        memset(reg, 0, sizeof *reg);
        snprintf(reg->id, sizeof reg->id, "%s", id);
        snprintf(reg->document_noun, sizeof reg->document_noun, "document");
    }
    if (name) snprintf(reg->name, sizeof reg->name, "%s", name);
    return reg;
}

static void seed(ma_roster *r, const char *id, const char *name, const char *aliases, const char *abilities, bool eyes, int poll) {
    ma_registration *reg = ma_roster_add(r, id, name);
    if (!reg) return;
    reg->has_eyes = eyes;
    reg->poll_seconds = poll;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", aliases ? aliases : "");
    reg->alias_count = 0;
    for (char *tok = strtok(copy, ","); tok && reg->alias_count < MA_ALIASES_MAX; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        snprintf(reg->aliases[reg->alias_count++], 32, "%s", tok);
    }
    char list[256];
    snprintf(list, sizeof list, "%s", abilities ? abilities : "");
    reg->ability_count = 0;
    for (char *tok = strtok(list, ", "); tok && reg->ability_count < MA_PER_APP_ABILITIES; tok = strtok(NULL, ", "))
        snprintf(reg->abilities[reg->ability_count++], 32, "%s", tok);
}

void ma_roster_maryos(ma_roster *r) {
    ma_roster_clear(r);
    /* The disciplines: the crafts a place can host. Their triggers are the words that ask for them. */
    static const char *const WRITING[] = { "write", "writing", "draft", "compose", "essay", "note", "notes", "letter", "paragraph",
        "sentence", "prose", "wording", "rewrite", "revise", "tighten", "shorten", "expand", "polish", "reword", "rephrase",
        "proofread", "spelling", "typo", "typos", NULL };
    static const char *const MULTIMEDIA[] = { "play", "pause", "resume", "song", "songs", "music", "video", "videos", "album",
        "track", "tracks", "playlist", "volume", "playback", "skip", "mute", "unmute", "louder", "quieter", NULL };
    static const char *const AWARENESS[] = { "file", "files", "folder", "folders", "directory", "where is", "find", "locate",
        "recent", "downloads", "desktop", "trash", "process", "processes", "cpu", "memory usage", NULL };
    static const char *const ARCHITECT[] = { "teach you", "new ability", "ability package", "skill package", "make an ability", NULL };
    static const char *const WINDOWS[] = { "window", "windows", "minimize", "minimise", "arrange", "raise", "tile", "bring up",
        "close the window", "focus", NULL };
    static const char *const SYSTEM[] = { "settings", "preferences", "brightness", "wallpaper", "display", "displays", "sound",
        "speaker", "microphone", "network", "wifi", "wi-fi", "keyboard", "disk", "drive", "partition", "unmount", "eject", NULL };
    static const char *const SCHEDULING[] = { "calendar", "event", "events", "appointment", "appointments", "schedule", "meeting",
        "agenda", "tomorrow", "next week", NULL };
    static const char *const ARITHMETIC[] = { "calculate", "compute", "sum", "multiply", "divide", "percent", "square root",
        "plus", "minus", "times", NULL };
    static const char *const READING[] = { "image", "picture", "photo", "photos", "pdf", "preview", "zoom", "rotate", "page", NULL };
    static const char *const SHELL[] = { "terminal", "shell", "command", "commands", "run", "script", "console", NULL };
    static const char *const NONE[] = { NULL };
    ma_ability *a;
    if ((a = add_ability(r, "writing", MA_PARADIGM_DISCIPLINE))) { set_words(a->triggers, WRITING); set_words(a->transforms, TRANSFORMS); }
    if ((a = add_ability(r, "multimedia", MA_PARADIGM_DISCIPLINE))) { set_words(a->triggers, MULTIMEDIA); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "awareness", MA_PARADIGM_DISCIPLINE))) { set_words(a->triggers, AWARENESS); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "architect", MA_PARADIGM_APPLICATION_EXPERTISE))) { set_words(a->triggers, ARCHITECT); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "window-management", MA_PARADIGM_SYSTEM_CONTROL))) { set_words(a->triggers, WINDOWS); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "system", MA_PARADIGM_SYSTEM_CONTROL))) { set_words(a->triggers, SYSTEM); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "scheduling", MA_PARADIGM_APPLICATION_EXPERTISE))) { set_words(a->triggers, SCHEDULING); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "arithmetic", MA_PARADIGM_APPLICATION_EXPERTISE))) { set_words(a->triggers, ARITHMETIC); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "reading", MA_PARADIGM_APPLICATION_EXPERTISE))) { set_words(a->triggers, READING); set_words(a->transforms, NONE); }
    if ((a = add_ability(r, "shell", MA_PARADIGM_APPLICATION_EXPERTISE))) { set_words(a->triggers, SHELL); set_words(a->transforms, NONE); }

    /* The desktop's applications (lp_app ids), as registrations: what they are called, what they host, and
     * whether they publish a surface (PARITY D28). */
    seed(r, "textedit", "TextEdit", "text edit, editor", "writing", true, 15);
    seed(r, "finder", "Finder", "files, file browser", "awareness", true, 30);
    seed(r, "preview", "Preview", "viewer", "reading", true, 30);
    seed(r, "calendar", "Calendar", "", "scheduling", true, 60);
    seed(r, "media", "Media Player", "player, music player, media", "multimedia", true, 15);
    seed(r, "terminal", "Terminal", "shell, console", "shell", true, 30);
    seed(r, "settings", "System Settings", "settings, preferences", "system", true, 120);
    seed(r, "calculator", "Calculator", "", "arithmetic", true, 60);
    seed(r, "activity", "Activity Monitor", "activity, monitor", "awareness", true, 60);
    seed(r, "diskutil", "Disk Utility", "disks", "system", true, 120);
    seed(r, "thread", "Threads", "thread, the thread", "awareness", false, 0);
    seed(r, "desktop", "Desktop", "", "window-management", false, 0);
}

const ma_ability *ma_roster_ability(const ma_roster *r, const char *id) {
    if (!r || !id) return NULL;
    for (int i = 0; i < r->ability_count; i++) if (strcmp(r->abilities[i].id, id) == 0) return &r->abilities[i];
    return NULL;
}

const ma_registration *ma_roster_registration(const ma_roster *r, const char *id) {
    if (!r || !id) return NULL;
    for (int i = 0; i < r->app_count; i++) if (strcasecmp(r->apps[i].id, id) == 0) return &r->apps[i];
    return NULL;
}

void ma_roster_merge_skills(ma_roster *r, struct json_object *apps) {
    if (!apps || !json_object_is_type(apps, json_type_array)) return;
    size_t n = json_object_array_length(apps);
    for (size_t i = 0; i < n; i++) {
        struct json_object *app = json_object_array_get_idx(apps, i);
        const char *id = mc_json_string(app, "id"), *name = mc_json_string(app, "name");
        if (id && *id) ma_roster_add(r, id, name && *name ? name : NULL);
    }
}

/* Whether `phrase` (one or more lowercase words) occurs as a run of whole words. */
static bool has_phrase(char words[][32], int n, const char *phrase) {
    char parts[8][32];
    int k = ma_words(phrase, parts, 8, true);
    if (!k) return false;
    for (int i = 0; i + k <= n; i++) {
        int j = 0;
        while (j < k && strcmp(words[i + j], parts[j]) == 0) j++;
        if (j == k) return true;
    }
    return false;
}

static int hits(const ma_ability *a, char words[][32], int n) {
    int count = 0;
    for (int i = 0; a->triggers[i]; i++) count += has_phrase(words, n, a->triggers[i]);
    return count;
}

static int compare_ids(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int ma_roster_requested_abilities(const ma_roster *r, const char *utterance, char out[][32], int max) {
    char words[96][32];
    int n = ma_words(utterance, words, 96, true), count = 0;
    for (int i = 0; i < r->ability_count && count < max; i++) {
        if (hits(&r->abilities[i], words, n) > 0) snprintf(out[count++], 32, "%s", r->abilities[i].id);
    }
    qsort(out, (size_t)count, 32, compare_ids);
    return count;
}

ma_paradigm ma_roster_paradigm(const ma_roster *r, const char *ability_id) {
    const ma_ability *a = ma_roster_ability(r, ability_id);
    return a ? a->paradigm : MA_PARADIGM_APPLICATION_EXPERTISE;
}

bool ma_roster_is_discipline(const ma_roster *r, const char *ability_id) {
    const ma_ability *a = ma_roster_ability(r, ability_id);
    return a && a->paradigm == MA_PARADIGM_DISCIPLINE;
}

const char *ma_roster_discipline_in(const ma_roster *r, const char *utterance) {
    char words[96][32];
    int n = ma_words(utterance, words, 96, true), best = 0, ties = 0;
    const char *winner = NULL;
    for (int i = 0; i < r->ability_count; i++) {
        if (r->abilities[i].paradigm != MA_PARADIGM_DISCIPLINE) continue;
        int h = hits(&r->abilities[i], words, n);
        if (h > best) { best = h; winner = r->abilities[i].id; ties = 0; }
        else if (h == best && h > 0) ties++;
    }
    return best > 0 && ties == 0 ? winner : NULL;      /* a contested turn defers to window truth */
}

bool ma_roster_names_transform(const ma_roster *r, const char *text) {
    char words[96][32];
    int n = ma_words(text, words, 96, true);
    for (int i = 0; i < r->ability_count; i++)
        for (int j = 0; r->abilities[i].transforms[j]; j++)
            if (has_phrase(words, n, r->abilities[i].transforms[j])) return true;
    return false;
}

bool ma_registration_is_mentioned(const ma_registration *reg, const char *utterance) {
    char words[96][32];
    int n = ma_words(utterance, words, 96, false);
    if (has_phrase(words, n, reg->id) || (reg->name[0] && has_phrase(words, n, reg->name))) return true;
    for (int i = 0; i < reg->alias_count; i++) if (has_phrase(words, n, reg->aliases[i])) return true;
    return false;
}

ma_place ma_registration_place(const ma_registration *reg) { return ma_place_application(reg->id); }

bool ma_place_has_eyes(const ma_place *p, const ma_roster *r) {
    const ma_registration *reg = ma_place_is_application(p) ? ma_roster_registration(r, p->application) : NULL;
    return reg ? reg->has_eyes : false;      /* a lane is never observed; the apps riding it are */
}

const char *ma_place_ability(const ma_place *p, const ma_roster *r) {
    const ma_registration *reg = ma_place_is_application(p) ? ma_roster_registration(r, p->application) : NULL;
    if (!reg || !reg->ability_count) return NULL;
    /* Precedence is the registry's: the first installed discipline the app declares, else the first by id. */
    for (int i = 0; i < r->ability_count; i++) {
        if (r->abilities[i].paradigm != MA_PARADIGM_DISCIPLINE) continue;
        for (int j = 0; j < reg->ability_count; j++) if (strcmp(reg->abilities[j], r->abilities[i].id) == 0) return r->abilities[i].id;
    }
    const char *first = reg->abilities[0];
    for (int j = 1; j < reg->ability_count; j++) if (strcmp(reg->abilities[j], first) < 0) first = reg->abilities[j];
    return first;
}

const char *ma_place_focus(const ma_place *p, const ma_roster *r) {
    const char *ability = ma_place_ability(p, r);
    return ability && ma_roster_is_discipline(r, ability) ? ability : NULL;
}

const char *ma_place_display_name(const ma_place *p, const ma_roster *r) {
    if (ma_place_is_application(p)) {
        const ma_registration *reg = ma_roster_registration(r, p->application);
        if (reg && reg->name[0]) return reg->name;
        return p->application;
    }
    return ma_attention_display_name(p->attention);
}

int ma_place_order(const ma_place *p, const ma_roster *r) {
    if (!ma_place_is_application(p)) return (int)p->attention;
    for (int i = 0; r && i < r->app_count; i++) if (strcasecmp(r->apps[i].id, p->application) == 0) return 1000 + i;
    return 1000 + (r ? r->app_count : 0);
}

const char *ma_place_class_name(const ma_place *p, const ma_roster *r) {
    if (!ma_place_is_application(p)) return p->attention == MA_ATTENTION_APPLICATIONS ? "perceptionOnly" : "service";
    const ma_registration *reg = ma_roster_registration(r, p->application);
    if (!reg) return "perceptionOnly";
    if (reg->has_eyes) return "workspace";
    for (int i = 0; i < reg->ability_count; i++) if (ma_roster_paradigm(r, reg->abilities[i]) == MA_PARADIGM_SYSTEM_CONTROL) return "service";
    return "perceptionOnly";
}
