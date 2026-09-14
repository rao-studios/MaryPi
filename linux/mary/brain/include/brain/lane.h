/* Lane B, the silent orchestrator (MaryBrain+Turn.swift runOrchestratorLane, MaryBrain+RepeatGuard.swift):
 * the skills loop with the voice removed. Rounds of `complete{messages, tools, instructions, scope}`
 * through sewnd; every tool call is decided by the registry, parked for the person when it needs
 * confirmation, refused when it repeats a call that just failed or ran unproven, and dispatched
 * through the desktop's pipes; its result rides back as a tool message so the tool_use →
 * tool_result pairing survives. At most ten rounds; a turn may be nudged once when everything it
 * ran only read or prepared and a change was asked for. The hooks block: the lane runs on a
 * thread of its own, and maryd's hooks bridge to its main loop. */
#ifndef MARY_BRAIN_LANE_H
#define MARY_BRAIN_LANE_H

#include <stdbool.h>
#include <stddef.h>

#include "skills/registry.h"

#define MB_LANE_MAX_ROUNDS 10
#define MB_LANE_OUTCOMES 32
#define MB_LANE_MAX_TOKENS 800

struct json_object;

typedef struct mb_lane_outcome {
    char call_id[64];
    char invocation[128];
    char app[64], skill[64];
    char arguments[512];            /* canonical */
    bool ok, requested, deferred, landed, found_nothing, asks_the_person, is_read;
    char summary[240];
    double started, finished;
} mb_lane_outcome;

typedef struct mb_lane_hooks {
    /* sewnd's complete: the request object is borrowed; *reply is a new reference ({text, tool_calls[]}). 0, or -errno. */
    int (*complete)(struct json_object *request, struct json_object **reply, char *message, size_t cap, void *user);
    /* Runs a skill through the desktop; *result is a new reference (may be NULL). 0, or -errno with `error` (the wire's word).
     * confirmed is true when the person just allowed this call on the card: the desktop's gate lets it through then. */
    int (*invoke)(const char *app, const char *skill, struct json_object *args, bool confirmed, struct json_object **result, char *error, size_t cap, void *user);
    /* Asks the person (the confirmation card). 1 allowed, 0 refused, -ETIMEDOUT. */
    int (*confirm)(const char *call_id, const sk_app *app, const sk_skill *skill, struct json_object *args, const char *summary, void *user);
    /* A run began, and ended (for the trace and the chips). Optional. */
    void (*run_started)(const mb_lane_outcome *outcome, void *user);
    void (*run_finished)(const mb_lane_outcome *outcome, void *user);
    /* The lane should stop (a newer turn). Optional. */
    bool (*cancelled)(void *user);
    void *user;
} mb_lane_hooks;

typedef struct mb_lane_request {
    const char *system;             /* the orchestration prompt (mb_system_prompt), with the executor addendum appended here */
    struct json_object *messages;   /* [{role, content}], borrowed: the history and the user's turn */
    const sk_registry *registry;
    struct json_object *scope;      /* {lanes, groups, entities, request_id}, borrowed, or NULL */
    const char *provider;           /* "mistral" */
    const char *request_id;
    bool implies_action;            /* the turn asked for a change: the continuation nudge may fire */
    int max_rounds;                 /* 0: MB_LANE_MAX_ROUNDS */
} mb_lane_request;

typedef struct mb_lane_result {
    char *text;                     /* the model's last prose (heap), "" when it only acted */
    mb_lane_outcome outcomes[MB_LANE_OUTCOMES];
    int outcome_count;
    int rounds;
    bool nudged;
    bool cancelled;
    char error[240];                /* why the lane stopped early, or "" */
} mb_lane_result;

/* Runs the lane to its end. 0, or -errno when the first completion could not be had. */
int mb_lane_run(const mb_lane_request *req, const mb_lane_hooks *hooks, mb_lane_result *out);
void mb_lane_result_free(mb_lane_result *r);

/* The repeat guard: an earlier outcome of this lane for the same call that did not go through, and the
 * most recent outcome being the same unproven act. */
const mb_lane_outcome *mb_lane_already_failed(const mb_lane_outcome *outcomes, int n, const char *invocation, const char *canonical_args);
const mb_lane_outcome *mb_lane_already_ran_unproven(const mb_lane_outcome *outcomes, int n, const char *invocation, const char *canonical_args);
/* The Mistral messages a round appends: the assistant's tool calls, and one tool result. */
struct json_object *mb_lane_assistant_message(const char *text, struct json_object *tool_calls);
struct json_object *mb_lane_tool_message(const char *call_id, const char *name, const char *content);
/* One line the model reads in place of a result. */
void mb_lane_result_line(const mb_lane_outcome *outcome, char *out, size_t n);

#endif
