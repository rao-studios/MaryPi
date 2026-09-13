#include "skills/registry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"

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

static void free_apps(sk_app *apps, size_t count) {
    for (size_t a = 0; a < count; a++) {
        for (size_t s = 0; s < apps[a].skill_count; s++) {
            free(apps[a].skills[s].id);
            free(apps[a].skills[s].title);
            free(apps[a].skills[s].summary);
            json_object_put(apps[a].skills[s].params);
        }
        free(apps[a].skills);
        free(apps[a].id);
        free(apps[a].name);
    }
    free(apps);
}

static char *copy(const char *s) { return strdup(s ? s : ""); }

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

static void tool_name(const sk_app *app, const sk_skill *skill, char *out, size_t cap) {
    snprintf(out, cap, "%s__%s", app->id, skill->id);
    for (char *p = out; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) *p = '_';
    }
}

struct json_object *sk_tools_json(const sk_registry *r) {
    struct json_object *tools = json_object_new_array();
    for (size_t a = 0; a < r->app_count; a++) {
        const sk_app *app = &r->apps[a];
        for (size_t s = 0; s < app->skill_count; s++) {
            const sk_skill *skill = &app->skills[s];
            if (sk_registry_decide(r, app->id, skill->id) != SK_ALLOWED) continue;
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
