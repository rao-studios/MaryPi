/* What Mary can do with each application, as the desktop tells maryd: the
 * `skills{apps: [...]}` message, sent on connect and whenever System Settings
 * changes a policy. On macOS Mary learns applications from `.mary` ability packages
 * and drives them through the accessibility tree; on MaryOS each app declares its
 * skills in code (MaryUI's lp_skill) and Settings holds the per-app policy
 * (PORTING.md deviations 9 and 10).
 *
 *   {id, name, enabled, ask: "never"|"changes"|"always",
 *    skills: [{id, title, summary, params: JSON Schema or null,
 *              effect: "read"|"act"|"destructive", enabled}]}
 *
 * The desktop decides every call; this copy lets maryd say in advance why a call
 * would be refused, and render the skills Mary may use as Mistral tools. */
#ifndef MARY_SKILLS_REGISTRY_H
#define MARY_SKILLS_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

struct json_object;

typedef enum sk_effect {
    SK_EFFECT_READ,         /* looks, changes nothing */
    SK_EFFECT_ACT,          /* changes something that can be changed back */
    SK_EFFECT_DESTRUCTIVE,  /* cannot be undone: always confirmed */
} sk_effect;

typedef enum sk_ask {
    SK_ASK_NEVER,
    SK_ASK_CHANGES,         /* before any skill that acts */
    SK_ASK_ALWAYS,
} sk_ask;

typedef struct sk_skill {
    char *id;
    char *title;
    char *summary;
    struct json_object *params;     /* a JSON Schema, or NULL */
    sk_effect effect;
    bool enabled;
} sk_skill;

typedef struct sk_app {
    char *id;
    char *name;
    bool enabled;
    sk_ask ask;
    sk_skill *skills;
    size_t skill_count;
} sk_app;

typedef struct sk_registry {
    sk_app *apps;
    size_t app_count;
} sk_registry;

void sk_registry_init(sk_registry *r);
/* Replaces the registry with a `skills` message. 0, or -EINVAL (the old contents are kept). */
int sk_registry_load(sk_registry *r, struct json_object *message);
void sk_registry_free(sk_registry *r);

const sk_app *sk_registry_app(const sk_registry *r, const char *app_id);
const sk_skill *sk_registry_skill(const sk_registry *r, const char *app_id, const char *skill_id);

typedef enum sk_decision {
    SK_ALLOWED,
    SK_UNKNOWN,                 /* no such app or skill */
    SK_DENIED,                  /* the app or the skill is turned off */
    SK_NEEDS_CONFIRMATION,      /* destructive, or the app asks first */
} sk_decision;

/* The desktop's rule: unknown, then denied, then needs confirmation, else allowed. */
sk_decision sk_registry_decide(const sk_registry *r, const char *app_id, const char *skill_id);

/* The wire names: "allowed", "unknown", "denied", "needs_confirmation". */
const char *sk_decision_name(sk_decision d);
const char *sk_effect_name(sk_effect e);
const char *sk_ask_name(sk_ask a);

/* The skills Mary may use (enabled app and skill, not needing confirmation) as
 * Mistral tools: [{type: "function", function: {name, description, parameters}}].
 * A tool is named "<app>__<skill>" with anything outside [A-Za-z0-9_-] as '_', as
 * Mistral requires. The conversation sends these in the next milestone. */
struct json_object *sk_tools_json(const sk_registry *r);
/* Finds the skill a tool name stands for. false when none does. */
bool sk_tool_lookup(const sk_registry *r, const char *tool_name, const sk_app **app, const sk_skill **skill);

#endif
