/* MaryAmbient/Ambient/Realm/AmbientPlace.swift, Models/AmbientAttention.swift and
 * Engine/AmbientCapability.swift + ApplicationRegistration.swift in C: where a fact
 * lives, and the roster of applications a place can be.
 *
 * A place is one of Mary's own lanes (applications, mac, system, window-management,
 * typer — the Swift raw values) or a registered application riding the `applications`
 * host lane. Its token is the store key's place half ("applications:textedit", "mac");
 * its memory token the persisted spelling ("textedit"). On the Mac registrations come
 * from `.mary` packages; on MaryOS the roster is the desktop's own applications, seeded
 * here (ma_roster_maryos) and renamed by the desktop's `skills{apps}` message. */
#ifndef MARY_AMBIENT_PLACE_H
#define MARY_AMBIENT_PLACE_H

#include <stdbool.h>
#include <stddef.h>

#define MA_ID_MAX 64
#define MA_NAME_MAX 64
#define MA_TOKEN_MAX (MA_ID_MAX + 32)

typedef enum ma_attention {
    MA_ATTENTION_APPLICATIONS,          /* the host lane every taught application rides */
    MA_ATTENTION_MAC,
    MA_ATTENTION_SYSTEM,
    MA_ATTENTION_WINDOW_MANAGEMENT,     /* raw value "window-management" */
    MA_ATTENTION_TYPER,
    MA_ATTENTION_COUNT
} ma_attention;

const char *ma_attention_name(ma_attention a);              /* the raw value; NULL out of range */
bool ma_attention_from_name(const char *name, ma_attention *out);
const char *ma_attention_display_name(ma_attention a);      /* "Applications", "Mac", … */
bool ma_attention_is_invocable(ma_attention a);             /* every lane but applications */

/* A place: a lane, or an application on the applications lane (application[0] set). */
typedef struct ma_place {
    ma_attention attention;
    char application[MA_ID_MAX];
} ma_place;

ma_place ma_place_lane(ma_attention a);
ma_place ma_place_application(const char *id);
bool ma_place_is_application(const ma_place *p);
bool ma_place_equal(const ma_place *a, const ma_place *b);
/* "calendar"-style tokens: the lane's raw value, or applications:<id>. */
void ma_place_token(const ma_place *p, char *out, size_t n);
/* The persisted spelling: the lane's raw value, or the bare id. */
const char *ma_place_memory_token(const ma_place *p);
/* Inverse of ma_place_token: splits on the first colon; only `applications` spells dynamics. */
bool ma_place_from_token(const char *token, ma_place *out);
/* strcmp of the two tokens: the candidates' order, and every other deterministic order. */
int ma_place_compare(const ma_place *a, const ma_place *b);

/* ---- the roster: what the machine can do (AbilityCapabilityIndex + ApplicationProfile) ---- */

typedef enum ma_paradigm {
    MA_PARADIGM_DISCIPLINE,             /* a portable craft: writing, multimedia, awareness */
    MA_PARADIGM_APPLICATION_EXPERTISE,  /* one application's skills */
    MA_PARADIGM_SYSTEM_CONTROL,         /* the computer itself */
} ma_paradigm;

const char *ma_paradigm_name(ma_paradigm p);                /* "discipline", "applicationExpertise", "systemControl" */

#define MA_ABILITIES_MAX 16
#define MA_ABILITY_TRIGGERS_MAX 40
#define MA_REGISTRATIONS_MAX 32
#define MA_ALIASES_MAX 6
#define MA_PER_APP_ABILITIES 4
#define MA_TARGET_CLASSES_MAX 4

/* An Ability as the ambient layer sees it: its id, its paradigm, and the words that
 * ask for it. On the Mac the words are embedded against each package's authored
 * triggers; on MaryOS the triggers are matched as whole words (PORTING.md deviation 13). */
typedef struct ma_ability {
    char id[32];
    ma_paradigm paradigm;
    const char *triggers[MA_ABILITY_TRIGGERS_MAX];      /* lowercase words or phrases; NULL-terminated */
    const char *transforms[MA_ABILITY_TRIGGERS_MAX];    /* the `transform` seed family; NULL-terminated */
} ma_ability;

/* ApplicationProfile + ApplicationRegistration: one application the roster knows. */
typedef struct ma_registration {
    char id[MA_ID_MAX];                 /* lp_app.id: "textedit" */
    char name[MA_NAME_MAX];             /* the display name */
    char aliases[MA_ALIASES_MAX][32];   /* besides the id and the name */
    int alias_count;
    char abilities[MA_PER_APP_ABILITIES][32];
    int ability_count;
    char target_classes[MA_TARGET_CLASSES_MAX][32];
    int target_class_count;
    bool has_eyes;                      /* it publishes a surface */
    int poll_seconds;                   /* how often the desktop republishes it (0: on change only) */
    char document_noun[16];             /* "document"; "file" for a coding place */
} ma_registration;

typedef struct ma_roster {
    ma_ability abilities[MA_ABILITIES_MAX];
    int ability_count;
    ma_registration apps[MA_REGISTRATIONS_MAX];
    int app_count;
} ma_roster;

/* MaryOS's roster: the disciplines and the desktop's applications. */
void ma_roster_maryos(ma_roster *r);
void ma_roster_clear(ma_roster *r);
const ma_ability *ma_roster_ability(const ma_roster *r, const char *id);
const ma_registration *ma_roster_registration(const ma_roster *r, const char *id);
ma_registration *ma_roster_add(ma_roster *r, const char *id, const char *name);
/* Renames and adds applications from the desktop's skills{apps} (an array of {id, name}); an app the
 * seed does not know joins with no abilities and no eyes. */
struct json_object;
void ma_roster_merge_skills(ma_roster *r, struct json_object *apps);

/* requestedAbilities(in:): the abilities the words ask for, sorted by id. Returns how many. */
int ma_roster_requested_abilities(const ma_roster *r, const char *utterance, char out[][32], int max);
/* The paradigm of an ability; applicationExpertise when unknown. */
ma_paradigm ma_roster_paradigm(const ma_roster *r, const char *ability_id);
bool ma_roster_is_discipline(const ma_roster *r, const char *ability_id);
/* discipline(in:): the one discipline whose triggers the words hit; NULL when none or two tie. */
const char *ma_roster_discipline_in(const ma_roster *r, const char *utterance);
/* namesTransform(in:): a transformation, asked for or offered. */
bool ma_roster_names_transform(const ma_roster *r, const char *text);

/* ApplicationProfile.isMentioned: any alias as a run of whole words. */
bool ma_registration_is_mentioned(const ma_registration *reg, const char *utterance);
ma_place ma_registration_place(const ma_registration *reg);
/* placeClass, hasEyes, focus and ability, asked of the roster. */
bool ma_place_has_eyes(const ma_place *p, const ma_roster *r);
/* The discipline this place hosts, or NULL. */
const char *ma_place_focus(const ma_place *p, const ma_roster *r);
/* The craft this place is for (the registry's first discipline, else the first ability by id), or NULL. */
const char *ma_place_ability(const ma_place *p, const ma_roster *r);
const char *ma_place_display_name(const ma_place *p, const ma_roster *r);
/* Stable render order: lanes keep their positions, registrations sort after in roster order. */
int ma_place_order(const ma_place *p, const ma_roster *r);
const char *ma_place_class_name(const ma_place *p, const ma_roster *r);    /* workspace | dataSource | service | perceptionOnly */

/* Words of a text, lowercased, split on anything that is not a letter or a digit (an apostrophe
 * is kept when keep_apostrophe). Returns how many; each word is NUL-terminated in `out`. */
int ma_words(const char *text, char out[][32], int max, bool keep_apostrophe);

#endif
