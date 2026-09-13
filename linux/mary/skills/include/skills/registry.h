/* What Mary can do with each application, as the desktop tells maryd: the
 * `skills{apps: [...]}` message, sent on connect and whenever System Settings
 * changes a policy. On macOS Mary learns applications from `.mary` ability packages
 * and drives them through the accessibility tree; on MaryOS each app declares its
 * skills in code (MaryUI's lp_skill) and Settings holds the per-app policy
 * (PORTING.md deviations 9 and 10).
 *
 *   {id, name, enabled, ask: "never"|"changes"|"always",
 *    [title, summary, aliases[], paradigm: "applicationExpertise"|"systemControl"|"discipline",
 *     discipline, perception: [field names]],
 *    skills: [{id, title, summary, params: JSON Schema or null,
 *              effect: "read"|"act"|"destructive", enabled,
 *              [kind: "cognitive"|"effectful"|"workflow", access: "seamless"|"confirm"|"reversible",
 *               triggers: {tokens[], phrases[]}, target_classes[], spoken: {param: {value: [words]}}]}]}
 *
 * The richer fields are the Mac's SkillSchema and AbilitySchema (kind, access, triggers, target
 * classes, spoken values; the package's title, summary, aliases, paradigm); absent, they take their
 * defaults from the effect. The desktop decides every call; this copy lets maryd say in advance
 * why a call would be refused, render the skills Mary may use as Mistral tools, and write one
 * `ability` record per skill into the Thread (sk_ability_records). */
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

typedef enum sk_kind { SK_KIND_COGNITIVE, SK_KIND_EFFECTFUL, SK_KIND_WORKFLOW } sk_kind;
typedef enum sk_access { SK_ACCESS_SEAMLESS, SK_ACCESS_CONFIRM, SK_ACCESS_REVERSIBLE } sk_access;

typedef struct sk_skill {
    char *id;
    char *title;
    char *summary;
    struct json_object *params;     /* a JSON Schema, or NULL */
    sk_effect effect;
    bool enabled;
    char invocation[128];           /* "<app>__<skill>", the tool's name */
    sk_kind kind;                   /* effectful unless the desktop says */
    sk_access access;               /* from the effect: read → seamless, act → reversible, destructive → confirm */
    char **triggers;                /* tokens */
    size_t trigger_count;
    char **phrases;
    size_t phrase_count;
    char **target_classes;
    size_t target_class_count;
    struct json_object *spoken;     /* {param: {value: [words]}} or NULL */
} sk_skill;

typedef struct sk_app {
    char *id;
    char *name;
    bool enabled;
    sk_ask ask;
    sk_skill *skills;
    size_t skill_count;
    char *title;                    /* the name unless the desktop says */
    char *summary;
    char **aliases;
    size_t alias_count;
    char *paradigm;                 /* applicationExpertise unless the desktop says */
    char *discipline;               /* the craft it realizes: writing, multimedia, awareness … or "" */
    char **perception;              /* the surface fields it publishes */
    size_t perception_count;
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
const char *sk_kind_name(sk_kind k);            /* cognitive, effectful, workflow */
const char *sk_access_name(sk_access a);        /* seamless, confirm, reversible */
/* The skill a tool name stands for: "<app>__<skill>" with anything outside [A-Za-z0-9_-] as '_'. */
void sk_tool_name(const sk_app *app, const sk_skill *skill, char *out, size_t cap);

/* The Thread's `ability` records for this registry (ThreadMemoryTopology): a JSON array of threadd
 * `deposit` requests — one document per skill (its title, summary, invocation, parameters, effect,
 * triggers and target classes, with the app `offers` the skill and the skill `effects` its effect in
 * the graph), one `ability-schema` manifest per app (what it publishes), and one document per
 * discipline naming the apps that realize it. Ids are minted from the owner, the app and the skill
 * (mary-ability-schema-<fnv>), so a second deposit replaces the first. A new reference. */
struct json_object *sk_ability_records(const sk_registry *r, const char *owner);
/* The group id an app's (or a discipline's) ability records live in: mary-ability-<fnv(owner|id|paradigm)>. */
void sk_ability_group(const char *owner, const char *ability_id, const char *paradigm, char *out, size_t cap);
void sk_ability_document_id(const char *owner, const char *ability_id, const char *paradigm, const char *skill_id, char *out, size_t cap);

/* The skills Mary may use (enabled app and skill; those that need confirmation too, since
 * the skills lane parks them for the person) as Mistral tools: [{type: "function",
 * function: {name, description, parameters}}]. */
struct json_object *sk_tools_json(const sk_registry *r);
/* Finds the skill a tool name stands for. false when none does. */
bool sk_tool_lookup(const sk_registry *r, const char *tool_name, const sk_app **app, const sk_skill **skill);

#endif
