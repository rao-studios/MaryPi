#include "skills/registry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "foundation/hash.h"

void sk_registry_init(sk_registry *r) { memset(r, 0, sizeof *r); }

static bool parse_effect(const char *s, sk_effect *out) {
    if (!s) return false;
    if (strcmp(s, "read") == 0) *out = SK_EFFECT_READ;
    else if (strcmp(s, "act") == 0) *out = SK_EFFECT_ACT;
    else if (strcmp(s, "destructive") == 0) *out = SK_EFFECT_DESTRUCTIVE;
    else return false;
    return true;
}

static bool parse_ask(const char *s, sk_ask *out) {
    if (!s) {
        *out = SK_ASK_CHANGES;   /* Settings' default */
        return true;
    }
    if (strcmp(s, "never") == 0) *out = SK_ASK_NEVER;
    else if (strcmp(s, "changes") == 0) *out = SK_ASK_CHANGES;
    else if (strcmp(s, "always") == 0) *out = SK_ASK_ALWAYS;
    else return false;
    return true;
}

static void free_strings(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void free_apps(sk_app *apps, size_t count) {
    for (size_t a = 0; a < count; a++) {
        for (size_t s = 0; s < apps[a].skill_count; s++) {
            sk_skill *k = &apps[a].skills[s];
            free(k->id);
            free(k->title);
            free(k->summary);
            json_object_put(k->params);
            json_object_put(k->spoken);
            free_strings(k->triggers, k->trigger_count);
            free_strings(k->phrases, k->phrase_count);
            free_strings(k->target_classes, k->target_class_count);
        }
        free(apps[a].skills);
        free(apps[a].id);
        free(apps[a].name);
        free(apps[a].title);
        free(apps[a].summary);
        free(apps[a].paradigm);
        free(apps[a].discipline);
        free_strings(apps[a].aliases, apps[a].alias_count);
        free_strings(apps[a].perception, apps[a].perception_count);
    }
    free(apps);
}

static char *copy(const char *s) { return strdup(s ? s : ""); }

/* An array of strings from JSON, copied. */
static char **copy_strings(struct json_object *arr, size_t *count) {
    *count = 0;
    if (!arr || !json_object_is_type(arr, json_type_array)) return NULL;
    size_t n = json_object_array_length(arr);
    char **out = n ? calloc(n, sizeof *out) : NULL;
    for (size_t i = 0; out && i < n; i++) {
        struct json_object *item = json_object_array_get_idx(arr, i);
        if (json_object_is_type(item, json_type_string)) out[(*count)++] = copy(json_object_get_string(item));
    }
    return out;
}

static bool parse_kind(const char *s, sk_kind *out) {
    if (!s) { *out = SK_KIND_EFFECTFUL; return true; }
    if (strcmp(s, "cognitive") == 0) *out = SK_KIND_COGNITIVE;
    else if (strcmp(s, "effectful") == 0) *out = SK_KIND_EFFECTFUL;
    else if (strcmp(s, "workflow") == 0) *out = SK_KIND_WORKFLOW;
    else return false;
    return true;
}

static bool parse_access(const char *s, sk_effect effect, sk_access *out) {
    if (!s) { *out = effect == SK_EFFECT_READ ? SK_ACCESS_SEAMLESS : effect == SK_EFFECT_ACT ? SK_ACCESS_REVERSIBLE : SK_ACCESS_CONFIRM; return true; }
    if (strcmp(s, "seamless") == 0) *out = SK_ACCESS_SEAMLESS;
    else if (strcmp(s, "confirm") == 0) *out = SK_ACCESS_CONFIRM;
    else if (strcmp(s, "reversible") == 0) *out = SK_ACCESS_REVERSIBLE;
    else return false;
    return true;
}

int sk_registry_load(sk_registry *r, struct json_object *message) {
    struct json_object *list = mc_json_array(message, "apps");
    if (!list) return -EINVAL;
    size_t n = json_object_array_length(list);
    sk_app *apps = n ? calloc(n, sizeof *apps) : NULL;
    if (n && !apps) return -ENOMEM;
    int rc = 0;
    for (size_t a = 0; rc == 0 && a < n; a++) {
        struct json_object *app = json_object_array_get_idx(list, a);
        const char *id = mc_json_string(app, "id");
        struct json_object *skills = mc_json_array(app, "skills");
        bool enabled = true;
        mc_json_bool(app, "enabled", &enabled);
        if (!id || !*id || !skills || !parse_ask(mc_json_string(app, "ask"), &apps[a].ask)) {
            rc = -EINVAL;
            break;
        }
        apps[a].id = copy(id);
        apps[a].name = copy(mc_json_string(app, "name") ? mc_json_string(app, "name") : id);
        apps[a].enabled = enabled;
        apps[a].title = copy(mc_json_string(app, "title") ? mc_json_string(app, "title") : apps[a].name);
        apps[a].summary = copy(mc_json_string(app, "summary"));
        apps[a].paradigm = copy(mc_json_string(app, "paradigm") ? mc_json_string(app, "paradigm") : "applicationExpertise");
        apps[a].discipline = copy(mc_json_string(app, "discipline"));
        apps[a].aliases = copy_strings(mc_json_array(app, "aliases"), &apps[a].alias_count);
        apps[a].perception = copy_strings(mc_json_array(app, "perception"), &apps[a].perception_count);
        size_t m = json_object_array_length(skills);
        apps[a].skills = m ? calloc(m, sizeof *apps[a].skills) : NULL;
        for (size_t s = 0; s < m; s++) {
            struct json_object *skill = json_object_array_get_idx(skills, s);
            sk_skill *out = &apps[a].skills[s];
            const char *skill_id = mc_json_string(skill, "id");
            if (!skill_id || !*skill_id || !parse_effect(mc_json_string(skill, "effect"), &out->effect)) {
                rc = -EINVAL;
                break;
            }
            out->id = copy(skill_id);
            out->title = copy(mc_json_string(skill, "title"));
            out->summary = copy(mc_json_string(skill, "summary"));
            struct json_object *params = mc_json_object(skill, "params");
            out->params = params ? json_object_get(params) : NULL;
            out->enabled = true;
            mc_json_bool(skill, "enabled", &out->enabled);
            if (!parse_kind(mc_json_string(skill, "kind"), &out->kind) || !parse_access(mc_json_string(skill, "access"), out->effect, &out->access)) {
                rc = -EINVAL;
                break;
            }
            struct json_object *triggers = mc_json_object(skill, "triggers"), *spoken = mc_json_object(skill, "spoken");
            out->triggers = copy_strings(mc_json_array(triggers, "tokens"), &out->trigger_count);
            out->phrases = copy_strings(mc_json_array(triggers, "phrases"), &out->phrase_count);
            out->target_classes = copy_strings(mc_json_array(skill, "target_classes"), &out->target_class_count);
            out->spoken = spoken ? json_object_get(spoken) : NULL;
            sk_tool_name(&apps[a], out, out->invocation, sizeof out->invocation);
            apps[a].skill_count++;
        }
    }
    if (rc) {
        free_apps(apps, n);
        return rc;
    }
    free_apps(r->apps, r->app_count);
    r->apps = apps;
    r->app_count = n;
    return 0;
}

void sk_registry_free(sk_registry *r) {
    free_apps(r->apps, r->app_count);
    memset(r, 0, sizeof *r);
}

const sk_app *sk_registry_app(const sk_registry *r, const char *app_id) {
    for (size_t a = 0; app_id && a < r->app_count; a++)
        if (strcmp(r->apps[a].id, app_id) == 0) return &r->apps[a];
    return NULL;
}

const sk_skill *sk_registry_skill(const sk_registry *r, const char *app_id, const char *skill_id) {
    const sk_app *app = sk_registry_app(r, app_id);
    for (size_t s = 0; app && skill_id && s < app->skill_count; s++)
        if (strcmp(app->skills[s].id, skill_id) == 0) return &app->skills[s];
    return NULL;
}

sk_decision sk_registry_decide(const sk_registry *r, const char *app_id, const char *skill_id) {
    const sk_app *app = sk_registry_app(r, app_id);
    const sk_skill *skill = sk_registry_skill(r, app_id, skill_id);
    if (!app || !skill) return SK_UNKNOWN;
    if (!app->enabled || !skill->enabled) return SK_DENIED;
    if (skill->effect == SK_EFFECT_DESTRUCTIVE || app->ask == SK_ASK_ALWAYS ||
        (app->ask == SK_ASK_CHANGES && skill->effect == SK_EFFECT_ACT))
        return SK_NEEDS_CONFIRMATION;
    return SK_ALLOWED;
}

const char *sk_decision_name(sk_decision d) {
    static const char *const names[] = { "allowed", "unknown", "denied", "needs_confirmation" };
    return (unsigned)d < 4 ? names[d] : NULL;
}

const char *sk_effect_name(sk_effect e) {
    static const char *const names[] = { "read", "act", "destructive" };
    return (unsigned)e < 3 ? names[e] : NULL;
}

const char *sk_ask_name(sk_ask a) {
    static const char *const names[] = { "never", "changes", "always" };
    return (unsigned)a < 3 ? names[a] : NULL;
}

const char *sk_kind_name(sk_kind k) { return k == SK_KIND_COGNITIVE ? "cognitive" : k == SK_KIND_WORKFLOW ? "workflow" : "effectful"; }
const char *sk_access_name(sk_access a) { return a == SK_ACCESS_CONFIRM ? "confirm" : a == SK_ACCESS_REVERSIBLE ? "reversible" : "seamless"; }

void sk_tool_name(const sk_app *app, const sk_skill *skill, char *out, size_t cap) {
    snprintf(out, cap, "%s__%s", app->id, skill->id);
    for (char *p = out; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) *p = '_';
    }
}

static void tool_name(const sk_app *app, const sk_skill *skill, char *out, size_t cap) { sk_tool_name(app, skill, out, cap); }

struct json_object *sk_tools_json(const sk_registry *r) {
    struct json_object *tools = json_object_new_array();
    for (size_t a = 0; a < r->app_count; a++) {
        const sk_app *app = &r->apps[a];
        for (size_t s = 0; s < app->skill_count; s++) {
            const sk_skill *skill = &app->skills[s];
            sk_decision decision = sk_registry_decide(r, app->id, skill->id);
            if (decision != SK_ALLOWED && decision != SK_NEEDS_CONFIRMATION) continue;
            char name[65], description[512];
            tool_name(app, skill, name, sizeof name);
            snprintf(description, sizeof description, "%s (%s): %s", skill->title, app->name, skill->summary);
            struct json_object *function = json_object_new_object(), *tool = json_object_new_object();
            json_object_object_add(function, "name", json_object_new_string(name));
            json_object_object_add(function, "description", json_object_new_string(description));
            struct json_object *parameters = skill->params ? json_object_get(skill->params) : NULL;
            if (!parameters) {
                parameters = json_object_new_object();
                json_object_object_add(parameters, "type", json_object_new_string("object"));
                json_object_object_add(parameters, "properties", json_object_new_object());
            }
            json_object_object_add(function, "parameters", parameters);
            json_object_object_add(tool, "type", json_object_new_string("function"));
            json_object_object_add(tool, "function", function);
            json_object_array_add(tools, tool);
        }
    }
    return tools;
}

bool sk_tool_lookup(const sk_registry *r, const char *tool_name_in, const sk_app **app_out, const sk_skill **skill_out) {
    for (size_t a = 0; tool_name_in && a < r->app_count; a++) {
        for (size_t s = 0; s < r->apps[a].skill_count; s++) {
            char name[65];
            tool_name(&r->apps[a], &r->apps[a].skills[s], name, sizeof name);
            if (strcmp(name, tool_name_in) == 0) {
                if (app_out) *app_out = &r->apps[a];
                if (skill_out) *skill_out = &r->apps[a].skills[s];
                return true;
            }
        }
    }
    return false;
}

/* ---- the Thread's ability records (ThreadMemoryTopology, AbilitySchema's knowledge document) ---- */

static void canonical(const char *in, char *out, size_t n) {
    size_t o = 0;
    bool space = false;
    for (const char *c = in ? in : ""; *c && o + 1 < n; c++) {
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') { space = o > 0; continue; }
        if (space) { out[o++] = ' '; space = false; if (o + 1 >= n) break; }
        out[o++] = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
    }
    out[o] = 0;
}

static void keyed(const char *owner, const char *ability_id, const char *paradigm, const char *extra, char hex[17]) {
    char key[512], a[128], b[128], c[64], d[128];
    canonical(owner, a, sizeof a);
    canonical(ability_id, b, sizeof b);
    canonical(paradigm, c, sizeof c);
    canonical(extra, d, sizeof d);
    if (extra) snprintf(key, sizeof key, "%s|%s|%s|%s", a, b, c, d);
    else snprintf(key, sizeof key, "%s|%s|%s", a, b, c);
    mf_fnv1a64_hex(key, hex);
}

void sk_ability_group(const char *owner, const char *ability_id, const char *paradigm, char *out, size_t cap) {
    char hex[17];
    keyed(owner, ability_id, paradigm, NULL, hex);
    snprintf(out, cap, "mary-ability-%s", hex);
}

void sk_ability_document_id(const char *owner, const char *ability_id, const char *paradigm, const char *skill_id, char *out, size_t cap) {
    char hex[17];
    keyed(owner, ability_id, paradigm, skill_id, hex);
    snprintf(out, cap, "mary-ability-schema-%s", hex);
}

static struct json_object *str(const char *s) { return json_object_new_string(s ? s : ""); }

static struct json_object *entity(const char *name, const char *kind) {
    struct json_object *e = json_object_new_object();
    json_object_object_add(e, "name", str(name));
    json_object_object_add(e, "kind", str(kind));
    return e;
}

static struct json_object *relationship(const char *subject, const char *predicate, const char *object) {
    struct json_object *r = json_object_new_object();
    json_object_object_add(r, "subject", str(subject));
    json_object_object_add(r, "predicate", str(predicate));
    json_object_object_add(r, "object", str(object));
    return r;
}

static void append_list(char *text, size_t cap, const char *label, char *const *items, size_t n) {
    if (!n) return;
    strncat(text, label, cap - strlen(text) - 1);
    for (size_t i = 0; i < n; i++) {
        if (i) strncat(text, ", ", cap - strlen(text) - 1);
        strncat(text, items[i], cap - strlen(text) - 1);
    }
    strncat(text, "\n", cap - strlen(text) - 1);
}

static struct json_object *deposit(const char *owner, const char *document_id, const char *group, const char *label, const char *family, const char *name, const char *text) {
    struct json_object *d = json_object_new_object();
    json_object_object_add(d, "type", str("deposit"));
    json_object_object_add(d, "source", str("maryd"));
    json_object_object_add(d, "document_id", str(document_id));
    json_object_object_add(d, "group", str(group));
    json_object_object_add(d, "label", str(label));
    json_object_object_add(d, "family", str(family));
    json_object_object_add(d, "name", str(name));
    struct json_object *texts = json_object_new_array();
    json_object_array_add(texts, str(text));
    json_object_object_add(d, "texts", texts);
    (void)owner;
    return d;
}

struct json_object *sk_ability_records(const sk_registry *r, const char *owner) {
    struct json_object *items = json_object_new_array();
    char disciplines[16][64];
    int discipline_count = 0;
    for (size_t a = 0; a < r->app_count; a++) {
        const sk_app *app = &r->apps[a];
        if (!app->skill_count) continue;
        char group[80], label[160];
        sk_ability_group(owner, app->id, app->paradigm, group, sizeof group);
        snprintf(label, sizeof label, "Ability \xE2\x80\x94 %s", app->title);
        const char *effects[3] = { "read", "act", "destructive" };
        for (size_t s = 0; s < app->skill_count; s++) {
            const sk_skill *k = &app->skills[s];
            char id[96], text[4096], params[1024] = "";
            sk_ability_document_id(owner, app->id, app->paradigm, k->id, id, sizeof id);
            if (k->params) snprintf(params, sizeof params, "%s", json_object_to_json_string_ext(k->params, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
            snprintf(text, sizeof text, "Skill: %s\nApplication: %s\nSummary: %s\nInvocation: %s\nEffect: %s\nKind: %s\nAccess: %s\n",
                     k->title, app->title, k->summary, k->invocation, effects[k->effect], sk_kind_name(k->kind), sk_access_name(k->access));
            if (params[0]) { strncat(text, "Parameters: ", sizeof text - strlen(text) - 1); strncat(text, params, sizeof text - strlen(text) - 1); strncat(text, "\n", sizeof text - strlen(text) - 1); }
            append_list(text, sizeof text, "Listens for: ", k->triggers, k->trigger_count);
            append_list(text, sizeof text, "Phrases: ", k->phrases, k->phrase_count);
            append_list(text, sizeof text, "Target classes: ", k->target_classes, k->target_class_count);
            struct json_object *d = deposit(owner, id, group, label, "ability", k->title, text);
            struct json_object *metadata = json_object_new_object(), *entities = json_object_new_array(), *relationships = json_object_new_array(), *tags = json_object_new_array();
            json_object_object_add(metadata, "family", str("ability"));
            json_object_object_add(metadata, "app", str(app->id));
            json_object_object_add(metadata, "skill", str(k->id));
            json_object_object_add(metadata, "invocation", str(k->invocation));
            json_object_object_add(metadata, "effect", str(effects[k->effect]));
            json_object_object_add(metadata, "kind", str(sk_kind_name(k->kind)));
            json_object_object_add(metadata, "access", str(sk_access_name(k->access)));
            json_object_object_add(metadata, "paradigm", str(app->paradigm));
            if (app->discipline && app->discipline[0]) json_object_object_add(metadata, "discipline", str(app->discipline));
            json_object_object_add(d, "metadata", metadata);
            json_object_array_add(entities, entity(app->title, "app"));
            json_object_array_add(entities, entity(k->title, "skill"));
            char effect_name[32];
            snprintf(effect_name, sizeof effect_name, "%s", effects[k->effect]);
            json_object_array_add(entities, entity(effect_name, "concept"));
            json_object_array_add(relationships, relationship(app->title, "offers", k->title));
            json_object_array_add(relationships, relationship(k->title, "effects", effect_name));
            if (app->discipline && app->discipline[0]) {
                json_object_array_add(entities, entity(app->discipline, "ability"));
                json_object_array_add(relationships, relationship(app->title, "practices", app->discipline));
            }
            json_object_object_add(d, "entities", entities);
            json_object_object_add(d, "relationships", relationships);
            json_object_array_add(tags, str("schema:mary.ability"));
            char tag[160];
            snprintf(tag, sizeof tag, "ability:%s", app->id);
            json_object_array_add(tags, str(tag));
            snprintf(tag, sizeof tag, "skill:%s", k->id);
            json_object_array_add(tags, str(tag));
            json_object_object_add(d, "tags", tags);
            json_object_array_add(items, d);
        }
        /* the manifest: how the app is perceived, and what it offers */
        char id[96], text[4096], name[160];
        sk_ability_document_id(owner, app->id, app->paradigm, "manifest", id, sizeof id);
        snprintf(name, sizeof name, "%s \xE2\x80\x94 schema", app->title);
        snprintf(text, sizeof text, "Application: %s\nPurpose: %s\nParadigm: %s\n", app->title, app->summary, app->paradigm);
        if (app->discipline && app->discipline[0]) { strncat(text, "Realizes: ", sizeof text - strlen(text) - 1); strncat(text, app->discipline, sizeof text - strlen(text) - 1); strncat(text, "\n", sizeof text - strlen(text) - 1); }
        append_list(text, sizeof text, "Aliases: ", app->aliases, app->alias_count);
        append_list(text, sizeof text, "Publishes: ", app->perception, app->perception_count);
        strncat(text, "Skills:\n", sizeof text - strlen(text) - 1);
        for (size_t s = 0; s < app->skill_count; s++) {
            char line[512];
            snprintf(line, sizeof line, "- %s: %s\n", app->skills[s].title, app->skills[s].summary);
            strncat(text, line, sizeof text - strlen(text) - 1);
        }
        struct json_object *d = deposit(owner, id, group, label, "ability-schema", name, text);
        struct json_object *metadata = json_object_new_object(), *entities = json_object_new_array(), *relationships = json_object_new_array();
        json_object_object_add(metadata, "family", str("ability-schema"));
        json_object_object_add(metadata, "app", str(app->id));
        json_object_object_add(metadata, "paradigm", str(app->paradigm));
        json_object_object_add(d, "metadata", metadata);
        json_object_array_add(entities, entity(app->title, "app"));
        for (size_t p = 0; p < app->perception_count; p++) {
            json_object_array_add(entities, entity(app->perception[p], "surface field"));
            json_object_array_add(relationships, relationship(app->title, "perceives", app->perception[p]));
        }
        json_object_object_add(d, "entities", entities);
        json_object_object_add(d, "relationships", relationships);
        json_object_array_add(items, d);
        if (app->discipline && app->discipline[0] && discipline_count < 16) {
            bool known = false;
            for (int i = 0; i < discipline_count && !known; i++) known = strcmp(disciplines[i], app->discipline) == 0;
            if (!known) snprintf(disciplines[discipline_count++], 64, "%s", app->discipline);
        }
    }
    /* the disciplines: one group each, naming the apps that realize it */
    for (int i = 0; i < discipline_count; i++) {
        char group[80], id[96], label[160], text[4096];
        sk_ability_group(owner, disciplines[i], "discipline", group, sizeof group);
        sk_ability_document_id(owner, disciplines[i], "discipline", disciplines[i], id, sizeof id);
        char title[64];
        snprintf(title, sizeof title, "%s", disciplines[i]);
        if (title[0] >= 'a' && title[0] <= 'z') title[0] = (char)(title[0] - 32);
        snprintf(label, sizeof label, "Ability \xE2\x80\x94 %s", title);
        snprintf(text, sizeof text, "Discipline: %s\nRealized by:\n", title);
        struct json_object *d = deposit(owner, id, group, label, "ability", title, "");
        struct json_object *entities = json_object_new_array(), *relationships = json_object_new_array(), *metadata = json_object_new_object();
        json_object_array_add(entities, entity(disciplines[i], "ability"));
        for (size_t a = 0; a < r->app_count; a++) {
            const sk_app *app = &r->apps[a];
            if (!app->discipline || strcmp(app->discipline, disciplines[i]) != 0) continue;
            char line[512];
            snprintf(line, sizeof line, "- %s", app->title);
            strncat(text, line, sizeof text - strlen(text) - 1);
            for (size_t s = 0; s < app->skill_count; s++) {
                snprintf(line, sizeof line, "%s %s", s ? "," : ":", app->skills[s].title);
                strncat(text, line, sizeof text - strlen(text) - 1);
            }
            strncat(text, "\n", sizeof text - strlen(text) - 1);
            json_object_array_add(entities, entity(app->title, "app"));
            json_object_array_add(relationships, relationship(app->title, "practices", disciplines[i]));
        }
        struct json_object *texts = json_object_new_array();
        json_object_array_add(texts, str(text));
        json_object_object_add(d, "texts", texts);
        json_object_object_add(metadata, "family", str("ability"));
        json_object_object_add(metadata, "discipline", str(disciplines[i]));
        json_object_object_add(metadata, "paradigm", str("discipline"));
        json_object_object_add(d, "metadata", metadata);
        json_object_object_add(d, "entities", entities);
        json_object_object_add(d, "relationships", relationships);
        json_object_array_add(items, d);
    }
    return items;
}
