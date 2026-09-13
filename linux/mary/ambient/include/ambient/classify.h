/* MaryAmbient/Ambient/Engine/EditIntentClassifier*.swift, Reference/NamedPartClassifier.swift,
 * Engine/RoutingLexicon.swift and AmbientIntentGate's question forms in C: the structural
 * classifiers a turn is read with. Grammar lives here (question openers, address words,
 * request frames); vocabulary that names a domain lives in the roster's abilities.
 * The Swift matches with regular expressions; this matches the same shapes over words,
 * so a test states the same sentence and gets the same answer. */
#ifndef MARY_AMBIENT_CLASSIFY_H
#define MARY_AMBIENT_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>

/* ---- the edit intent: a revision of something already written ---- */

typedef enum ma_edit_shape { MA_EDIT_REPLACE, MA_EDIT_INSERT, MA_EDIT_DELETE, MA_EDIT_MOVE } ma_edit_shape;
typedef enum ma_edit_anchor { MA_ANCHOR_NONE, MA_ANCHOR_BEFORE, MA_ANCHOR_AFTER, MA_ANCHOR_INTO } ma_edit_anchor;

#define MA_EDIT_CANDIDATES 3
#define MA_EDIT_TARGET_MAX 128
#define MA_EDIT_PAYLOAD_MAX 512

typedef struct ma_edit_intent {
    ma_edit_shape shape;
    char target[MA_EDIT_CANDIDATES][MA_EDIT_TARGET_MAX];    /* ordered guesses, best first */
    int target_count;
    char payload[MA_EDIT_PAYLOAD_MAX];                      /* verbatim, never cleaned; "" when none */
    ma_edit_anchor anchor;
    char destination[MA_EDIT_CANDIDATES][MA_EDIT_TARGET_MAX];
    int destination_count;
    bool anaphoric;                                         /* "tighten it up": the one we were talking about */
} ma_edit_intent;

const char *ma_edit_shape_name(ma_edit_shape s);    /* replace, insert, delete, move */
const char *ma_edit_anchor_name(ma_edit_anchor a);  /* before, after, into; NULL for none */

/* EditIntentClassifier.intent(in:applicationAliases:): the revision the utterance asks for,
 * clause by clause. false (the common answer) when it asks for none. `aliases` are lowercase
 * single words that read as an address when they lead. */
bool ma_edit_intent_in(const char *utterance, const char *const *aliases, int alias_count, ma_edit_intent *out);
/* stripPreamble: address, alias, request frame and confirmation peeled off the front. */
void ma_edit_strip_preamble(const char *text, const char *const *aliases, int alias_count, char *out, size_t n);
/* The target ladder: ordered guesses at which passage they meant. Returns how many (≤ 3). */
int ma_edit_candidates(const char *phrase, char out[][MA_EDIT_TARGET_MAX], int max);
bool ma_edit_is_fresh_surface_phrase(const char *phrase);
bool ma_edit_is_anaphoric_tail(const char *tail);
void ma_edit_trim_courtesy_tail(const char *raw, char *out, size_t n);

/* ---- the named part: does the utterance name a part of the document? ---- */

/* The phrase to hand a targeted read, or false. Case is preserved. */
bool ma_named_part(const char *utterance, char *out, size_t n);
/* The eyeless veto: the words name an ambient data source (a calendar, reminders, mail). */
bool ma_names_ambient_source(const char *utterance);
/* NamedPartClassifier.clean: a spoken capture trimmed to what a document might contain. */
bool ma_named_part_clean(const char *captured, char *out, size_t n);
bool ma_is_part_noun(const char *word);

/* ---- the closed-class grammar ---- */

bool ma_is_question_opener(const char *word);
bool ma_is_address_word(const char *word);      /* hey, mary, ok, okay */
bool ma_is_backchannel_word(const char *word);  /* yeah, yep, right, exactly, sure, alright, so, well */
bool ma_is_request_frame(const char *first, const char *second);     /* can you, could we, … */
#define MA_POLITE_TAIL "please"

/* AmbientQuestion: the forms that decide which durable context to consult, as a bitmask. */
enum { MA_Q_WHAT = 1, MA_Q_WHICH = 2, MA_Q_WHERE = 4, MA_Q_HOW = 8, MA_Q_WHY = 16, MA_Q_WHEN = 32, MA_Q_WHO = 64 };
unsigned ma_question_forms(const char *utterance);
/* The forms' names in the enum's order: what, which, where, how, why, when, who. */
const char *ma_question_name(unsigned bit);

/* A bare yes or no: 1, 0, or -1 when the utterance is neither. */
int ma_bare_decision(const char *utterance);

#endif
