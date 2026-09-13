/* Fleet/Sources/FleetCore/Gate/JSONGate.swift and TokenVocabulary.swift in C:
 * a schema turned into per-step token decisions. Declared; the automaton
 * (SchemaAutomaton.swift) and token trie are not ported yet. */
#ifndef MARY_FLEET_GATE_H
#define MARY_FLEET_GATE_H

#include <stddef.h>

typedef enum fleet_gate_decision_kind {
    FLEET_GATE_FORCED,      /* the schema fixes this text: emit token_id, no sampling */
    FLEET_GATE_FREE,        /* a value position: sample, but only from `allowed` */
    FLEET_GATE_COMPLETE,    /* the document is complete: emit EOS and stop */
    FLEET_GATE_STUCK,       /* no token can continue at `path` — an error, never invalid JSON */
} fleet_gate_decision_kind;

typedef struct fleet_gate_decision {
    fleet_gate_decision_kind kind;
    int token_id;               /* FORCED */
    const char *text;           /* FORCED */
    const int *allowed;         /* FREE */
    size_t allowed_count;
    const char *path;           /* STUCK */
} fleet_gate_decision;

/* TokenVocabulary: `count` is one past the highest id; text_of returns NULL for
 * a token that can never appear in gated output (specials, partial UTF-8). */
typedef struct fleet_vocabulary {
    int count;
    const char *(*text_of)(void *context, int token_id);
    void *context;
} fleet_vocabulary;

typedef struct fleet_gate fleet_gate;
typedef struct fleet_gate_state { unsigned long opaque[4]; } fleet_gate_state;

/* The SchemaTemplate as JSON. NULL with errno ENOSYS until the gate is ported. */
fleet_gate *fleet_gate_new(const char *schema_template_json, const fleet_vocabulary *vocabulary);
fleet_gate_state fleet_gate_initial_state(const fleet_gate *gate);
int fleet_gate_decide(const fleet_gate *gate, const fleet_gate_state *state, fleet_gate_decision *out);
void fleet_gate_free(fleet_gate *gate);

/* "forced", "free", "complete", "stuck" — GateDecision's case names. */
const char *fleet_gate_decision_name(fleet_gate_decision_kind kind);

#endif
