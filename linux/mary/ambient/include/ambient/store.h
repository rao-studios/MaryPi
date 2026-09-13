/* MaryAmbient/Ambient/World/AmbientContextStore.swift (+Surface), Models/AmbientFact.swift,
 * AmbientSlot.swift, AmbientSurface.swift, AmbientSelectionHandoff.swift and
 * AmbientWorld.Snapshot in C: short-term awareness for this machine and conversation,
 * in tiers —
 *
 *   0  SURFACE    what is on screen for one place, as the desktop published it; drops at
 *                 expiry, never degrades (25 s fresh by default)
 *   1  FACTS      one processed detail each, keyed (place, slot); a superseding write replaces
 *                 its slot; a fact past its fresh window keeps its place and loses its
 *                 authority; past retention it is dropped
 *   2  SELECTION  the source-owned highlight, a 30 s interaction packet; never inferred
 *
 * Durable retrieval is the Thread's; this is the memory of the moment. Times are seconds
 * (a double), so a test can state a clock. */
#ifndef MARY_AMBIENT_STORE_H
#define MARY_AMBIENT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "ambient/place.h"

#define MA_CONTENT_CAP 2000
#define MA_SUBJECT_MAX 256
#define MA_PHRASE_MAX 128
#define MA_NOTE_MAX 160
#define MA_ELEMENT_CAP 120
#define MA_LABEL_CAP 120
#define MA_SURFACE_FRESH_FOR 25.0
#define MA_HANDOFF_FRESH_FOR 30.0
#define MA_NAMED_READ_CAP 4
#define MA_LEAD_HORIZON 300.0           /* FocusSignal.coActiveHorizon */
#define MA_WORKSPACE_FRESH_FOR 15.0     /* AmbientSense.workspace */

/* ---- slots ---- */

typedef enum ma_slot_kind {
    MA_SLOT_FILE, MA_SLOT_VIEWPORT, MA_SLOT_SELECTION, MA_SLOT_OBJECT_SELECTION, MA_SLOT_CURSOR, MA_SLOT_GIT,
    MA_SLOT_PROJECT, MA_SLOT_NAMED_READ, MA_SLOT_DIGEST, MA_SLOT_HEARD, MA_SLOT_GLIMPSED, MA_SLOT_COUNT
} ma_slot_kind;

typedef struct ma_slot {
    ma_slot_kind kind;
    char document[MA_PHRASE_MAX];   /* a named read's document, or "" */
    char phrase[MA_PHRASE_MAX];     /* a named read's finding phrase */
} ma_slot;

ma_slot ma_slot_of(ma_slot_kind kind);
ma_slot ma_slot_read(const char *phrase, const char *document);
/* "file", "read:batteries", "read:<document>#<phrase>" … */
void ma_slot_token(const ma_slot *s, char *out, size_t n);
bool ma_slot_equal(const ma_slot *a, const ma_slot *b);
bool ma_slot_is_perceived(const ma_slot *s);    /* filled by a watcher on every poll */
bool ma_slot_is_read(const ma_slot *s);
int ma_slot_order(const ma_slot *s);

/* ---- facts ---- */

typedef enum ma_provenance { MA_PROVENANCE_LIVE_AX, MA_PROVENANCE_CACHED_BODY, MA_PROVENANCE_RECIPE_READ, MA_PROVENANCE_DERIVED } ma_provenance;
typedef enum ma_registered { MA_REGISTERED_PERCEIVED, MA_REGISTERED_ASKED_FOR } ma_registered;

const char *ma_provenance_name(ma_provenance p);        /* liveAX, cachedBody, recipeRead, derived */
const char *ma_provenance_display_name(ma_provenance p);
const char *ma_registered_name(ma_registered r);        /* perceived, askedFor */
const char *ma_registered_verb(ma_registered r);        /* seen, read */

typedef struct ma_fact {
    ma_place place;
    ma_slot slot;
    char content[MA_CONTENT_CAP + 1];
    char surrounding[MA_CONTENT_CAP + 1];
    char subject[MA_SUBJECT_MAX];
    char application_id[MA_ID_MAX];     /* the concrete application, when known */
    bool has_bounds;
    int lower, upper;                   /* code points inside `subject`, never invented */
    int document_total;                 /* 0: unknown */
    char anchor[32];                    /* the perception anchor's display name, or "" */
    ma_provenance provenance;
    ma_registered registration;
    double captured_at;
    double fresh_for, retain_for;       /* 0 at init time: the defaults for the slot and provenance */
    double spoken_at;                   /* 0: never */
    char spoken_note[MA_NOTE_MAX + 1];
    char passage_handle[16];            /* "[S1]"'s S1, or "" */
} ma_fact;

/* A fact with the defaults applied: content clipped, windows from the slot and provenance. */
void ma_fact_init(ma_fact *f, const ma_place *place, const ma_slot *slot, const char *content, ma_provenance provenance, double captured_at);
double ma_fact_default_fresh_for(ma_provenance p, const ma_slot *slot);
double ma_fact_default_retain_for(const ma_slot *slot);
double ma_fact_age(const ma_fact *f, double now);
bool ma_fact_is_fresh(const ma_fact *f, double now);
bool ma_fact_is_expired(const ma_fact *f, double now);
/* "<place token>/<slot token>", the key's id. */
void ma_fact_key(const ma_fact *f, char *out, size_t n);
/* AmbientContextStore.ordered: place order, slot order, key id. */
int ma_fact_ordered(const ma_fact *a, const ma_fact *b, const ma_roster *r);

/* ---- surfaces (tier 0) ---- */

typedef struct ma_element {
    char identity[MA_LABEL_CAP + 40];   /* role|label, the re-finding key */
    int ordinal;
    char role[32];
    char kind[32];                      /* the humanized word: "button" */
    char label[MA_LABEL_CAP + 1];
    bool focused, enabled;
} ma_element;

typedef struct ma_surface {
    ma_place place;
    char app_name[MA_NAME_MAX], app_id[MA_ID_MAX];
    int pid;
    bool has_window;
    char window_title[256];
    int window_count, minimized_count;
    ma_element elements[MA_ELEMENT_CAP];
    int element_count;
    bool has_focused;
    ma_element focused;
    bool page_not_yet_read;
    char document_path[1024];           /* MaryOS: the document the app shows, or "" */
    double captured_at, fresh_for;
} ma_surface;

void ma_surface_init(ma_surface *s, const ma_place *place, const char *app_name, const char *app_id, double captured_at);
double ma_surface_age(const ma_surface *s, double now);
bool ma_surface_is_fresh(const ma_surface *s, double now);
/* How a person would name an element: its label, else its kind. */
const char *ma_element_descriptor(const ma_element *e);

/* ---- the selection handoff (tier 2) ---- */

typedef struct ma_selection {
    char id[40];
    ma_place place;
    char application_id[MA_ID_MAX];
    char text[MA_CONTENT_CAP * 2 + 1];  /* the exact selected value */
    char surrounding[MA_CONTENT_CAP + 1];
    char subject[MA_SUBJECT_MAX];
    bool has_range;
    int lower, upper;                   /* code points inside the document */
    int document_total;
    bool editable;
    double captured_at;
} ma_selection;

bool ma_selection_is_fresh(const ma_selection *s, double now);

/* ---- the world snapshot ---- */

typedef enum ma_sense { MA_SENSE_WORKSPACE, MA_SENSE_SELECTION, MA_SENSE_HOVER } ma_sense;
const char *ma_sense_name(ma_sense s);
double ma_sense_fresh_for(ma_sense s);

typedef struct ma_world {
    ma_sense sense;
    ma_attention attention;
    char subject[MA_SUBJECT_MAX];
    char application_id[MA_ID_MAX];
    bool has_key;
    ma_place key_place;
    ma_slot key_slot;
    char selected_text[MA_CONTENT_CAP * 2 + 1];
    char surrounding[MA_CONTENT_CAP + 1];
    bool editable;
    double captured_at, fresh_for;
} ma_world;

ma_place ma_world_place(const ma_world *w);
bool ma_world_is_fresh(const ma_world *w, double now);
bool ma_world_matches(const ma_world *w, const ma_fact *f);
bool ma_world_is_direct_reference(const ma_world *w);

/* ---- the store ---- */

typedef struct ma_store ma_store;
struct ma_route;

ma_store *ma_store_new(void);
void ma_store_free(ma_store *s);
void ma_store_set_roster(ma_store *s, const ma_roster *r);   /* borrowed: for the sort order */

/* Writers. A selection fact never enters through register; that is the handoff's. */
void ma_store_register(ma_store *s, const ma_fact *f, double now);
/* Replaces one place's perceived slots for one poll; named reads and the selection survive. */
void ma_store_replace_perceived(ma_store *s, const ma_place *place, const ma_fact *facts, int n, double now);
void ma_store_forget_perceived(ma_store *s, const ma_place *place);
void ma_store_forget_attention(ma_store *s, ma_attention a);
void ma_store_forget_key(ma_store *s, const ma_place *place, const ma_slot *slot);
/* The turn loop's write-back: facts whose content a spoken passage contains get the note. Returns how many. */
int ma_store_note_spoken(ma_store *s, const char *const *passages, int n, const char *note, double now);

void ma_store_note_lead(ma_store *s, const ma_place *place, double now);
bool ma_store_lead_place(const ma_store *s, double now, ma_place *out);
void ma_store_note_utterance(ma_store *s, const char *text);
const char *ma_store_utterance(const ma_store *s);
void ma_store_note_route(ma_store *s, const struct ma_route *route);
const struct ma_route *ma_store_route(const ma_store *s);

/* Surfaces: latest wins by capture time, not receipt. */
void ma_store_note_surface(ma_store *s, const ma_surface *surface, double now);
const ma_surface *ma_store_surface(ma_store *s, const ma_place *place, double now);
/* Every fresh surface: place order, then newest first. Returns how many. */
int ma_store_surfaces(ma_store *s, double now, const ma_surface **out, int max);
void ma_store_forget_surface(ma_store *s, const ma_place *place);

/* The selection. A capture older than the source's last clear is refused (a late read must not revive). */
void ma_store_record_selection(ma_store *s, const ma_selection *sel, double now);
void ma_store_clear_selection(ma_store *s, const char *application_id, double now);
/* The fresh handoff, if any; the one a turn already claimed stays out until a newer capture. */
bool ma_store_selection(const ma_store *s, double now, ma_selection *out);
/* The handoff frozen for a turn: returned once, then claimed. */
bool ma_store_claim_selection(ma_store *s, double now, ma_selection *out);

void ma_store_note_world(ma_store *s, const ma_world *world, double now);
/* This turn's machine state: the fresh selection projected as a world, else the noted snapshot. */
bool ma_store_world(ma_store *s, double now, ma_world *out);

/* Readers. Live facts in the store's order, the selection projected from the handoff. */
int ma_store_facts(ma_store *s, double now, ma_fact *out, int max);
int ma_store_facts_at(ma_store *s, const ma_place *place, double now, ma_fact *out, int max);
bool ma_store_fact(ma_store *s, const ma_place *place, const ma_slot *slot, double now, ma_fact *out);
int ma_store_reads(ma_store *s, double now, ma_fact *out, int max);
int ma_store_count(const ma_store *s);
void ma_store_clear(ma_store *s);

#endif
